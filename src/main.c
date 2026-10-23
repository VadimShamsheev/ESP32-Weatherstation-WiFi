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
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "nvs.h"
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
    uint64_t ms_since_last_water;
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
    int percent = (MIN_HUMIDITY_ADC - get_moisture_raw(channel)) * 100 /
                  (MIN_HUMIDITY_ADC - MAX_HUMIDITY_ADC);
    return percent < 0 ? 0 : (percent > 100 ? 100 : percent);
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
void oled_draw_char(int x, int y, wchar_t c) {
    const uint8_t *symbol = get_font_bytes_array((wchar_t)c);
    if (!symbol) symbol = get_font_bytes_array(L' ');
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
    const unsigned char *text = (const unsigned char *)str;
    while (*text && x <= 122) {
        wchar_t c = *text++;
        if ((c & 0xE0) == 0xC0 && (*text & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (*text++ & 0x3F);
        } else if (c >= 0x80) {
            c = L' ';
        }
        oled_draw_char(x, y, c);
        x += 6;
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

static SemaphoreHandle_t config_mutex;
static const int buttons[] = {BUTTON_1, BUTTON_2, BUTTON_3, BUTTON_4};

static uint64_t interval_ms(const plant_config_t *plant) {
    return (uint64_t)plant->interval_hours * 3600000ULL;
}

static bool valid_config(const plant_config_t *p) {
    return (p->mode == WATERING_MODE_INTERVAL || p->mode == WATERING_MODE_MOISTURE) &&
        p->threshold_moisture >= 0 && p->threshold_moisture <= 100 &&
        p->interval_hours >= 24 && p->interval_hours <= 365 * 24 &&
        p->pump_time_ms > 0 && p->pump_time_ms <= 60000;
}

static void load_settings(void) {
    nvs_handle_t handle;
    if (nvs_open("watering", NVS_READONLY, &handle) != ESP_OK) return;
    plant_config_t *plants[] = {&plant1, &plant2};
    const char *keys[] = {"plant1", "plant2"};
    for (int i = 0; i < 2; ++i) {
        plant_config_t saved;
        size_t size = sizeof(saved);
        if (nvs_get_blob(handle, keys[i], &saved, &size) == ESP_OK &&
            size == sizeof(saved) && valid_config(&saved)) {
            saved.ms_since_last_water = 0;
            *plants[i] = saved;
        }
    }
    nvs_close(handle);
}

static esp_err_t save_settings(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("watering", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    plant_config_t saved = plant_settings_buffer;
    saved.ms_since_last_water = 0;
    err = nvs_set_blob(handle, selected_plant == &plant1 ? "plant1" : "plant2", &saved, sizeof(saved));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        saved.ms_since_last_water = selected_plant->ms_since_last_water;
        *selected_plant = saved;
    }
    return err;
}

static void watering_task(void *arg) {
    int64_t last_poll = esp_timer_get_time();
    for (;;) {
        int m1 = get_moisture_percent(ADC_CHANNEL_0);
        int m2 = get_moisture_percent(ADC_CHANNEL_1);
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(config_mutex, portMAX_DELAY);
        moisture1 = m1;
        moisture2 = m2;
        uint64_t elapsed = (now - last_poll) / 1000;
        plant1.ms_since_last_water += elapsed;
        plant2.ms_since_last_water += elapsed;
        last_poll = now;
        xSemaphoreGive(config_mutex);
        plant_config_t *plants[] = {&plant1, &plant2};
        const int pins[] = {PUMP_1_PIN, PUMP_2_3_PIN};
        const int moisture[] = {m1, m2};
        for (int i = 0; i < 2; ++i) {
            xSemaphoreTake(config_mutex, portMAX_DELAY);
            plant_config_t p = *plants[i];
            bool due = (p.mode == WATERING_MODE_MOISTURE &&
                        moisture[i] < p.threshold_moisture && p.ms_since_last_water >= 600000) ||
                       (p.mode == WATERING_MODE_INTERVAL && p.ms_since_last_water >= interval_ms(&p));
            xSemaphoreGive(config_mutex);
            if (due) {
                gpio_set_level(pins[i], 1);
                vTaskDelay(pdMS_TO_TICKS(1000 + p.pump_time_ms));
                gpio_set_level(pins[i], 0);
                xSemaphoreTake(config_mutex, portMAX_DELAY);
                // The next poll adds time since last_poll; subtract the part before completion.
                plants[i]->ms_since_last_water = 0;
                int64_t finished = esp_timer_get_time();
                uint64_t delta = (finished - last_poll) / 1000;
                plants[1-i]->ms_since_last_water += delta;
                last_poll = finished;
                xSemaphoreGive(config_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

static void sensor_task(void *arg) {
    for (;;) {
        dht_update_current_values();
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void handle_button(int btn) {
    last_activity_time = xTaskGetTickCount();
    if (current_state == STATE_DISPLAY_OFF) {
        current_state = STATE_MAIN_WINDOW;
        esp_lcd_panel_disp_on_off(panel_handle, true);
        return;
    }
    if (current_state == STATE_MAIN_WINDOW) {
        if (btn == 0) current_state = STATE_SETTINGS_MENU;
    } else if (current_state == STATE_SETTINGS_MENU) {
        if (btn == 1) current_state = STATE_MAIN_WINDOW;
        else if (btn == 2 || btn == 3) menu_cursor ^= 1;
        else if (btn == 0) {
            selected_plant = menu_cursor == 0 ? &plant1 : &plant2;
            plant_settings_buffer = *selected_plant;
            settings_edit_idx = 0;
            is_editing = false;
            current_state = menu_cursor == 0 ? STATE_PLANT1_SET : STATE_PLANT2_SET;
        }
    } else if (!is_editing) {
        if (btn == 1) current_state = STATE_SETTINGS_MENU;
        else if (btn == 2) settings_edit_idx = (settings_edit_idx + 3) % 4;
        else if (btn == 3) settings_edit_idx = (settings_edit_idx + 1) % 4;
        else if (btn == 0 && settings_edit_idx == 3) {
            esp_err_t err = save_settings();
            if (err == ESP_OK) current_state = STATE_SETTINGS_MENU;
            else ESP_LOGE("settings", "Save failed: %s", esp_err_to_name(err));
        } else if (btn == 0) is_editing = true;
    } else {
        if (btn == 0) is_editing = false;
        else if (btn == 1) {
            if (settings_edit_idx == 0) plant_settings_buffer.mode = selected_plant->mode;
            if (settings_edit_idx == 1) plant_settings_buffer.threshold_moisture = selected_plant->threshold_moisture;
            if (settings_edit_idx == 2) plant_settings_buffer.interval_hours = selected_plant->interval_hours;
            is_editing = false;
        } else {
            int direction = btn == 2 ? 1 : -1;
            if (settings_edit_idx == 0) plant_settings_buffer.mode ^= 1;
            if (settings_edit_idx == 1) {
                int value = plant_settings_buffer.threshold_moisture + direction * 5;
                if (value >= 0 && value <= 100) plant_settings_buffer.threshold_moisture = value;
            }
            if (settings_edit_idx == 2) {
                int value = (int)plant_settings_buffer.interval_hours + direction * 24;
                if (value >= 24 && value <= 365 * 24) plant_settings_buffer.interval_hours = value;
            }
        }
    }
}

static void draw_ui(void) {
    char buf[96];
    oled_buffer_clear();
    if (current_state == STATE_MAIN_WINDOW) {
        oled_print_string(30, 0, "статус");
        plant_config_t plants[] = {plant1, plant2};
        int values[] = {moisture1, moisture2};
        for (int i = 0; i < 2; ++i) {
            snprintf(buf, sizeof(buf), "р%d: %d%%", i + 1, values[i]);
            oled_print_string(0, 12 + i * 20, buf);
            if (plants[i].mode == WATERING_MODE_INTERVAL) {
                uint64_t period = interval_ms(&plants[i]);
                uint64_t left = plants[i].ms_since_last_water >= period ? 0 : period - plants[i].ms_since_last_water;
                snprintf(buf, sizeof(buf), "полив через: %luч", (unsigned long)((left + 3599999) / 3600000));
            } else snprintf(buf, sizeof(buf), "режим: влажность");
            oled_print_string(0, 22 + i * 20, buf);
        }
        snprintf(buf, sizeof(buf), "%.1f'с  %.1f%%", current_temperature, current_humidity);
        oled_print_string(0, 54, buf);
    } else if (current_state == STATE_SETTINGS_MENU) {
        oled_print_string(35, 0, "меню");
        for (int i = 0; i < 2; ++i) {
            snprintf(buf, sizeof(buf), "%s %d. растение %d", menu_cursor == i ? ">" : " ", i+1, i+1);
            oled_print_string(0, 15 + i*12, buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "растение %d", selected_plant == &plant1 ? 1 : 2);
        oled_print_string(10, 0, buf);
        for (int i = 0; i < 4; ++i) {
            if (i == 0) snprintf(buf, sizeof(buf), "режим: %s", plant_settings_buffer.mode == WATERING_MODE_INTERVAL ? "интервал" : "влажн");
            if (i == 1) snprintf(buf, sizeof(buf), "мин влаж: %d%%", plant_settings_buffer.threshold_moisture);
            if (i == 2) snprintf(buf, sizeof(buf), "интервал: %luч", (unsigned long)plant_settings_buffer.interval_hours);
            if (i == 3) snprintf(buf, sizeof(buf), "сохранить");
            oled_print_string(12, 15 + i*12, buf);
            if (settings_edit_idx == i) oled_print_string(0, 15 + i*12, ">");
            if (settings_edit_idx == i && is_editing) oled_print_string(120, 15 + i*12, "*");
        }
    }
    oled_flush();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_settings();
    config_mutex = xSemaphoreCreateMutex();
    configASSERT(config_mutex);
    int pumps[] = {PUMP_1_PIN, PUMP_2_3_PIN};
    for (int i = 0; i < 2; ++i) {
        gpio_reset_pin(pumps[i]);
        gpio_set_level(pumps[i], 0);
        gpio_set_direction(pumps[i], GPIO_MODE_OUTPUT);
    }
    for (int i = 0; i < 4; ++i) {
        gpio_reset_pin(buttons[i]);
        gpio_set_direction(buttons[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(buttons[i], GPIO_PULLUP_ONLY);
    }
    init_display();
    oled_clear();
    init_adc();
    configASSERT(xTaskCreate(watering_task, "watering", 4096, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(sensor_task, "dht", 3072, NULL, 4, NULL) == pdPASS);
    last_activity_time = xTaskGetTickCount();
    int previous = -1, stable = -1;
    TickType_t changed = xTaskGetTickCount();
    for (;;) {
        int pressed = -1;
        for (int i = 0; i < 4; ++i) if (gpio_get_level(buttons[i]) == 0) { pressed = i; break; }
        TickType_t now = xTaskGetTickCount();
        if (pressed != previous) { previous = pressed; changed = now; }
        xSemaphoreTake(config_mutex, portMAX_DELAY);
        if (now - changed >= pdMS_TO_TICKS(50) && stable != pressed) {
            stable = pressed;
            if (pressed >= 0) handle_button(pressed);
        }
        if (current_state != STATE_DISPLAY_OFF && now - last_activity_time >= pdMS_TO_TICKS(60000)) {
            current_state = STATE_DISPLAY_OFF;
            is_editing = false;
            esp_lcd_panel_disp_on_off(panel_handle, false);
        }
        if (current_state != STATE_DISPLAY_OFF) draw_ui();
        xSemaphoreGive(config_mutex);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
