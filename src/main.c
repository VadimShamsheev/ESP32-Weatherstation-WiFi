#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
//#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
//#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_adc/adc_oneshot.h"

// Библиотеки для работы с дисплеем
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

// Нативный драйвер датчика 
#include "dht_native.h" 

// Библиотека символов
#include "font8x16.h"

#define I2C_MASTER_SDA_IO   8
#define I2C_MASTER_SCL_IO   9
#define I2C_MASTER_FREQ_HZ  400000

#define MIN_HUMIDITY_ADC    3150        // В ВОЗДУХЕ
#define MAX_HUMIDITY_ADC    1000        // В СТАКАНЕ С ВОДОЙ

// Номера пинов, на которые повешаны кнопки
#define BUTTON_1            7
#define BUTTON_2            6
/*
    Пин №2 используется во время загрузки и должен быть на уровне HIGH
    Пин физически подтянут к линии питания через подтягивающий резистор
*/
#define BUTTON_3            2
#define BUTTON_4            10

/*
    Номера пинов для считавания показаний датчиков влажности почвы
    Работает на АЦП, поэтому подключается к пинам 0 и 1, на которые идет встроенный АЦП
*/
#define CAPACITOR_HUMIDIDY_PIN_1    1
#define CAPACITOR_HUMIDIDY_PIN_2    0

// Пины на помпы
#define PUMP_1_PIN          4
#define PUMP_2_3_PIN        3

// Теги для логов
#define DISPLAY_TAG         "Дисплей вещает"
#define SENSORS_TAG         "Датчики вещают"

typedef enum {
    WATERING_MODE_MOISTURE,
    WATERING_MODE_INTERVAL
} watering_mode_t;

typedef struct {
    watering_mode_t mode;
    int threshold_moisture; // в процентах (например, 30%)
    uint32_t interval_hours; 
    uint32_t ms_since_last_water;
    int pump_time_ms;       // время работы помпы после 1 сек давления
} plant_config_t;

// Настройки по умолчанию
plant_config_t plant1 = {WATERING_MODE_INTERVAL, 30, 120, 0, 1500}; // Маленькое растение (1.5 сек полив ~ 35мл)
plant_config_t plant2 = {WATERING_MODE_INTERVAL, 30, 120, 0, 2500}; // Большое растение (2.5 сек полив ~ 60мл)
plant_config_t *selected_plant;
plant_config_t plant_settings_buffer;

// Состояния интерфейса
typedef enum {
    STATE_MAIN_WINDOW,
    STATE_SETTINGS_MENU,
    STATE_PLANT1_SET,
    STATE_PLANT2_SET,
    STATE_DISPLAY_OFF
} ui_state_t;

ui_state_t current_state = STATE_MAIN_WINDOW;
uint32_t last_activity_time = 0;

int menu_cursor = 0;       // Текущий выбранный пункт (0, 1, 2...)
int settings_edit_idx = 0; // Индекс редактируемого параметра внутри настроек растения
bool is_editing = false;   // Флаг: мы просто листаем пункты или меняем значение

int moisture1 = 0, 
    moisture2 = 0;



i2c_master_bus_handle_t bus_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

// Буфер, хранящий последовательность бит 
static uint8_t frame_buffer[1024];

adc_oneshot_unit_handle_t adc1_handle;

/* ПОГЛЯДЕТЬ ПРО КАНАЛЫ */
void init_adc(void) {
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12, 
    };

    // Настройка канала для CAPACITOR_HUMIDIDY_PIN_2 (Пин 0)
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));
    
    // Настройка канала для CAPACITOR_HUMIDIDY_PIN_1 (Пин 1)
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_1, &config));
}

int get_moisture_raw(adc_channel_t channel) {
    int raw_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, channel, &raw_value));
    ESP_LOGI(SENSORS_TAG, "Датчик влажности вернул значение %d", raw_value);
    return raw_value;
}

int get_moisture_percent(adc_channel_t channel) {
    return (MIN_HUMIDITY_ADC - get_moisture_raw(channel)) / 2150;
}

/*
    Изменить заполненность бита по координатам 
    int x - координата х (от 1 до 128)
    int y - координата у (от 1 до 64)
    bool color - 1 - если бит заполнен, 0 - если нет
*/
void draw_pixel(int x, int y, bool color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;

    if (color) {
        frame_buffer[x + (y / 8) * 128] |= (1 << (y %  8));
    } else {
        frame_buffer[x + (y / 8) * 128] &= ~(1 << (y %  8));
    }
}

/*
    Протолкнуть значения из буфера на дисплей
*/
void oled_flush(void) {
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 128, 64, frame_buffer);
}

/*
    Инициализация шины I2C
*/
void init_i2c_bus(void) {
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));
}

/*
    Инициализация дисплея
*/
void init_display(void) {
    init_i2c_bus();
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = 0x3C,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus_handle, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .bits_per_pixel = 1,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

/*
    Очистка дисплея
*/
void oled_clear(void) {
    uint8_t clear_buff[1024] = {0};
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 128, 64, clear_buff);
}

/*
    Нарисовать символ из библиотеки символов (файл font8x16.h) 
    int x - координата х (от 1 до 128)
    int y - координата у (от 1 до 64)
    char c - символ, который нужно вывести
*/
void oled_draw_char(int x, int y, char c) {
    const uint8_t *symbol = get_font_bytes_array((wchar_t)c);
    ESP_LOGI(DISPLAY_TAG, "Найденный символ %hhd", symbol[0]);
    for (int i = 0; i < 5; i++) {
        uint8_t line = symbol[i];
        for (int j = 0; j < 8; j++) {
            if (line & (1 << j)) {
                draw_pixel(x + i, y + j, true);
            }
        }
    }
    /*
    int idx = get_font_index(c);
    for (int i = 0; i < 5; i++) {
        uint8_t line = font_5x7[idx][i];
        for (int j = 0; j < 8; j++) {
            if (line & (1 << j)) {
                draw_pixel(x + i, y + j, true);
            }
        }
    }*/
}

/*
    Нарисовать символ по его индексу в списке в файле font8x16.h
    int x - координата х (от 1 до 128)
    int y - координата у (от 1 до 64)
    int idx - индекс символа
*/
/*void oled_draw_char_by_idx(int x, int y, int idx) {
    for (int i = 0; i < 5; i++) {
        uint8_t line = font_5x7[idx][i];
        for (int j = 0; j < 8; j++) {
            if (line & (1 << j)) {
                draw_pixel(x + i, y + j, true);
            }
        }
    }
}*/

/*
    Напечатать на экран строку, начиная с координаты х, у
    int x - координата х (от 1 до 128)
    int y - координата у (от 1 до 64)
    const char *str - строка, которая будет выведена на экран
*/
void oled_print_string(int x, int y, const char *str) {
    while (*str) {
        oled_draw_char(x, y, *str);
        x += 6;
        str++;
    }
}

/*
    Очистка буфера передаваемых битов frame_buffer
*/
void oled_buffer_clear(void) {
    memset(frame_buffer, 0, sizeof(frame_buffer));
}

/*
    Вывести на экран показания термогигрометра
    float temp - значение температуры, полученное от датчика
    float hum - значение влажности, полученное от датчика
*/
void display_weather_data(float temp, float hum) {
    char buf_temp[16];
    char buf_hum[16];

    snprintf(buf_temp, sizeof(buf_temp), "T: %.1f C", temp);
    snprintf(buf_hum, sizeof(buf_hum), "H: %.1f %%", hum);

    oled_buffer_clear();

    for (int x = 0; x < 128; x++) {
        draw_pixel(x, 0, true);
        draw_pixel(x, 63, true);
    }

    //oled_draw_char_by_idx(10, 16, 18);
    oled_print_string(10, 36, buf_temp);
    oled_print_string(10, 48, buf_hum);

    oled_flush();
}

void water_plant(int pump_pin, int watering_duration_ms) {
    gpio_set_level(pump_pin, 1);
    
    // 1 секунда на нагнетание давления + время самого полива
    vTaskDelay(pdMS_TO_TICKS(1000 + watering_duration_ms)); 
    
    gpio_set_level(pump_pin, 0);
}

void watering_task(void *pvParameters) {
    gpio_reset_pin(PUMP_1_PIN);
    gpio_set_direction(PUMP_1_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(PUMP_2_3_PIN);
    gpio_set_direction(PUMP_2_3_PIN, GPIO_MODE_OUTPUT);

    while(1) {
        // Чтение АЦП с пинов влажности 1 и 0 (требуется настройка ADC1)
        moisture1 = get_moisture_percent(ADC_CHANNEL_0);
        moisture2 = get_moisture_percent(ADC_CHANNEL_1); 
        
        // Логика проверки условий полива для первого растения
        if ((plant1.mode == WATERING_MODE_MOISTURE && moisture1 < plant1.threshold_moisture && plant1.ms_since_last_water >= 600000) || 
            (plant1.mode = WATERING_MODE_INTERVAL && plant1.ms_since_last_water >= plant1.interval_hours * 60 * 60 * 1000)) {
            water_plant(PUMP_1_PIN, plant1.pump_time_ms);
            plant1.ms_since_last_water = 0;
        }
        if ((plant2.mode == WATERING_MODE_MOISTURE && moisture2 < plant2.threshold_moisture && plant2.ms_since_last_water >= 600000) ||
            (plant2.mode = WATERING_MODE_INTERVAL && plant2.ms_since_last_water >= plant2.interval_hours * 60 * 60 * 1000)) {
            water_plant(PUMP_2_3_PIN, plant2.pump_time_ms);
            plant2.ms_since_last_water = 0;
        }

        // Обновление счетчиков времени с использованием внутренних часов (TickCount)
        vTaskDelay(pdMS_TO_TICKS(60000)); // Проверка раз в минуту
        plant1.ms_since_last_water += 60000;
        plant2.ms_since_last_water += 60000;
    }
}

/*
    Таск, следящий за нажатием кнопки
*/
void button_poling_task(void *pvParameters) {
    // Инициализация пинов кнопок: 7, 6, 2, 10
    const int btns[] = {7, 6, 2, 10}; 
    for(int i=0; i<4; i++) {
        gpio_reset_pin(btns[i]);
        gpio_set_direction(btns[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(btns[i], GPIO_PULLUP_ONLY);
    }

    last_activity_time = xTaskGetTickCount();

    while (1) {
        bool any_pressed = false;
        int pressed_btn = -1;

        for(int i=0; i<4; i++) {
            if (gpio_get_level(btns[i]) == 0) {
                any_pressed = true;
                pressed_btn = i;
                break;
            }
        }

        if (any_pressed) {
            last_activity_time = xTaskGetTickCount(); // Сброс таймера сна
            
            if (current_state == STATE_DISPLAY_OFF) {
                // Пробуждение: если были в настройках - сбрасываем на главную
                current_state = STATE_MAIN_WINDOW;
                esp_lcd_panel_disp_on_off(panel_handle, true);
            } else {
                // Обработка нажатий в зависимости от текущего меню
                // btns[0] - Звездочка (ОК/Меню), btns[1] - Решетка (Отмена)
                // btns[2] - Вверх, btns[3] - Вниз
                if (pressed_btn == 0 && current_state == STATE_MAIN_WINDOW) {       // Открыть меню
                    current_state = STATE_SETTINGS_MENU;
                } else if (current_state == STATE_PLANT1_SET || current_state == STATE_PLANT2_SET) {    // Переключение настроек у конкретного растения
                    if (!is_editing) {
                        if (pressed_btn == 2) {
                            if (settings_edit_idx == 3) {
                                settings_edit_idx = 0;
                            } else {
                                settings_edit_idx++;
                            }
                        } else if (pressed_btn == 3) {
                            if (settings_edit_idx == 0) {
                                settings_edit_idx = 3;
                            } else {
                                settings_edit_idx--;
                            }
                        } else if (pressed_btn == 0) {
                            is_editing = true;
                        }
                    } else {
                        if (settings_edit_idx == 0) {
                            if (pressed_btn == 2 || pressed_btn == 3) {
                                plant_settings_buffer.mode = (plant_settings_buffer.mode == WATERING_MODE_INTERVAL) ? WATERING_MODE_MOISTURE : WATERING_MODE_INTERVAL;
                            } else if (pressed_btn == 1) {
                                plant_settings_buffer.mode = selected_plant->mode;
                                is_editing = false;
                            } else if (pressed_btn == 0) {
                                is_editing = false;
                            }
                        } else if (settings_edit_idx == 1) {          // Настройка порога влажности почвы
                            if (pressed_btn == 2) { 
                                if (plant_settings_buffer.threshold_moisture < 100) plant_settings_buffer.threshold_moisture += 5;
                            } else if (pressed_btn == 3) {
                                if (plant_settings_buffer.threshold_moisture > 0) plant_settings_buffer.threshold_moisture -= 5;
                            } else if (pressed_btn == 1) {
                                plant_settings_buffer.threshold_moisture = selected_plant->threshold_moisture;
                                is_editing = false;
                            } else if (pressed_btn == 0) {
                                is_editing = false;
                            }
                        } else if (settings_edit_idx == 2) {      // Настройка интервала полива
                            if (pressed_btn == 2) {
                                if (plant_settings_buffer.interval_hours < (365*24)) plant_settings_buffer.interval_hours += 24;
                            } else if (pressed_btn == 3) {
                                if (plant_settings_buffer.interval_hours >= 24) plant_settings_buffer.interval_hours -= 24;
                            } else if (pressed_btn == 1) {
                                plant_settings_buffer.interval_hours = selected_plant->interval_hours;
                                is_editing = false;
                            } else if (pressed_btn == 0) {
                                is_editing = false;
                            }
                        }
                    }
                    if (settings_edit_idx == 3 && pressed_btn == 0) {
                        *selected_plant = plant_settings_buffer;
                        is_editing = false;
                    }
                } else if (pressed_btn == 1 && current_state != STATE_MAIN_WINDOW) {    // Кнопка отмены
                    current_state = (current_state == STATE_SETTINGS_MENU) ? STATE_MAIN_WINDOW : STATE_SETTINGS_MENU;  
                    is_editing = false;                  
                } else if (current_state == STATE_SETTINGS_MENU) {  
                    if (pressed_btn == 2 || pressed_btn == 3) menu_cursor = (menu_cursor ^ 1);      // Переключение в настройках 
                } else if (pressed_btn == 0 && current_state == STATE_SETTINGS_MENU) {         // Войти в настройки конкретного растения
                    current_state = (menu_cursor == 0) ? STATE_PLANT1_SET : STATE_PLANT2_SET;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(200)); // Антидребезг
        }

        // Проверка таймаута бездействия (60 секунд)
        if (current_state != STATE_DISPLAY_OFF && 
           (xTaskGetTickCount() - last_activity_time > pdMS_TO_TICKS(60000))) {
            current_state = STATE_DISPLAY_OFF;
            esp_lcd_panel_disp_on_off(panel_handle, false); // Отключение экрана
        }

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// Функция отрисовки главной страницы
void draw_main_page() {
    char buf[32];
    oled_buffer_clear();

    oled_print_string(30, 0, "статус");

    // Информации по растению 1 (заглушки значений нужно заменить на реальные переменные)
    // Пример влажности
    int hours_left1 = (plant1.ms_since_last_water - plant1.interval_hours * 60 * 60 * 1000) / 3600000; // Пример оставшегося времени
    
    snprintf(buf, sizeof(buf), "р1: %d%%", moisture1);
    oled_print_string(0, 12, buf);
    
    if (plant1.mode == WATERING_MODE_INTERVAL) {
        
        snprintf(buf, sizeof(buf), "полив через: %dч", hours_left1);
    } else {
        snprintf(buf, sizeof(buf), "режим: влажность");
    }
    oled_print_string(0, 22, buf);

    // Информация по растению 2
    int moist2 = 30; 
    int hours_left2 = 2;
    
    snprintf(buf, sizeof(buf), "р2: %d%%", moist2);
    oled_print_string(0, 32, buf);
    
    if (plant2.mode == WATERING_MODE_INTERVAL) {
        snprintf(buf, sizeof(buf), "полив через: %dч", hours_left2);
    } else {
        snprintf(buf, sizeof(buf), "режим: влажность");
    }
    oled_print_string(0, 42, buf);

    dht_update_current_values();
    snprintf(buf, sizeof(buf), "%f'с   %f%%", current_temperature, current_humidity);
    oled_print_string(0, 54, buf);

    oled_flush();
}

// Функция отрисовки меню настроек
void draw_settings_menu() {
    oled_buffer_clear();
    
    oled_print_string(35, 0, "меню");

    // Отрисовка пунктов с курсором
    if (menu_cursor == 0) oled_print_string(0, 15, "> 1. растение 1");
    else oled_print_string(0, 15, "  1. растение 1");

    if (menu_cursor == 1) oled_print_string(0, 27, "> 2. растение 2");
    else oled_print_string(0, 27, "  2. растение 2");

    oled_flush();
}

// Функция отрисовки окна настройки полива (конкретное растение)
void draw_plant_settings() {
    char buf[32];
    oled_buffer_clear();
    
    oled_print_string(10, 0, "растение 1");

    // Пункт 1: Режим
    snprintf(buf, sizeof(buf), "режим: %s", selected_plant->mode == WATERING_MODE_INTERVAL ? "interval" : "moisture");
    if (settings_edit_idx == 0) {
        oled_print_string(0, 15, ">");
        if (is_editing) oled_print_string(110, 15, "*"); // Звездочка показывает режим редактирования
    }
    oled_print_string(12, 15, buf);

    // Пункт 2: Порог влажности
    snprintf(buf, sizeof(buf), "мин влаж: %d%%", selected_plant->threshold_moisture);
    if (settings_edit_idx == 1) {
        oled_print_string(0, 27, ">");
        if (is_editing) oled_print_string(110, 27, "*");
    }
    oled_print_string(12, 27, buf);

    // Пункт 3: Интервал в часах
    snprintf(buf, sizeof(buf), "интервал: %luч", selected_plant->interval_hours);
    if (settings_edit_idx == 2) {
        oled_print_string(0, 39, ">");
        if (is_editing) oled_print_string(110, 39, "*");
    }
    oled_print_string(12, 39, buf);

    snprintf(buf, sizeof(buf), "сохранить");
    if (settings_edit_idx == 3) {
        oled_print_string(0, 51, ">");
    }
    oled_print_string(12, 51, buf);


    oled_flush();
}

void app_main(void) { 
    // Инициализация памяти NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Запуск задачи для проверки состояния кнопок
    xTaskCreate(button_poling_task, "button_task", 2048, NULL, 5, NULL);
    
    init_display();
    oled_clear();

    init_adc();

    // Ожидаем инициализации датчика и дисплея
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        if (current_state != STATE_DISPLAY_OFF) {
            switch(current_state) {
                case STATE_MAIN_WINDOW:
                    draw_main_page();
                    break;
                case STATE_SETTINGS_MENU:
                    draw_settings_menu();
                    break;
                case STATE_PLANT1_SET:
                    selected_plant = &plant1;
                    plant_settings_buffer = plant1;
                    draw_plant_settings();
                    break;
                case STATE_PLANT2_SET:
                    selected_plant = &plant2;
                    plant_settings_buffer = plant2;
                    draw_plant_settings();
                    break;
                default:
                    break;
            }
        }
        
        // Обновляем экран довольно часто для отзывчивости интерфейса
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}