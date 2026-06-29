#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"

// Библиотеки для работы с дисплеем
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

// ПОДКЛЮЧАЕМ НАШ НАТИВНЫЙ ДАТЧИК
#include "dht_native.h" 

#include "font8x16.h"

#define WIFI_SSID           "SNR-CPE-4740"
#define WIFI_PASS           "9012100280"
#define BROKER_URL          "mqtt://192.168.1.119:1883"
#define MQTT_TOPIC          "home/weather/metrics"

#define I2C_MASTER_SDA_IO   8
#define I2C_MASTER_SCL_IO   9
#define I2C_MASTER_FREQ_HZ  400000

#define TIME_TO_SLEEP_15M   15 * 60 * 1000000ULL

static const char *MQTT_TAG = "MQTT_MAIN",
                  *SLEEP_MODE_TAG = "SLEEP_MODE_TAG";

static esp_mqtt_client_handle_t global_mqtt_client = NULL;

i2c_master_bus_handle_t bus_handle = NULL;
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_panel_io_handle_t io_handle = NULL;

static uint8_t frame_buffer[1024];



void draw_pixel(int x, int y, bool color) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;

    if (color) {
        frame_buffer[x + (y / 8) * 128] |= (1 << (y %  8));
    } else {
        frame_buffer[x + (y / 8) * 128] &= ~(1 << (y %  8));
    }
}

void oled_flush(void) {
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 128, 64, frame_buffer);
}

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


void oled_clear(void) {
    uint8_t clear_buff[1024] = {0};
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 128, 64, clear_buff);
}

void oled_draw_char(int x, int y, char c) {
    int idx = get_font_index(c);
    for (int i = 0; i < 5; i++) {
        uint8_t line = font_5x7[idx][i];
        for (int j = 0; j < 8; j++) {
            if (line & (1 << j)) {
                draw_pixel(x + i, y + j, true);
            }
        }
    }
}

void oled_draw_char_by_idx(int x, int y, int idx) {
    for (int i = 0; i < 5; i++) {
        uint8_t line = font_5x7[idx][i];
        for (int j = 0; j < 8; j++) {
            if (line & (1 << j)) {
                draw_pixel(x + i, y + j, true);
            }
        }
    }
}

void oled_print_string(int x, int y, const char *str) {
    while (*str) {
        oled_draw_char(x, y, *str);
        x += 6;
        str++;
    }
}

void oled_buffer_clear(void) {
    memset(frame_buffer, 0, sizeof(frame_buffer));
}


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

    oled_draw_char_by_idx(10, 16, 18);
    oled_print_string(10, 36, buf_temp);
    oled_print_string(10, 48, buf_hum);

    oled_flush();
}


void goToSleep() {
    ESP_LOGI(SLEEP_MODE_TAG, "Станция переходит в спящий режим...");
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_15M);
    esp_wifi_stop();
    
    vTaskDelay(pdMS_TO_TICKS(50)); // Даем логам успеть выплюнуться в UART
    esp_deep_sleep_start();
}

// Функция публикации теперь принимает дескриптор клиента напрямую
void mqtt_data_publish(esp_mqtt_client_handle_t client) {
    char json_buffer[128];
    
    snprintf(json_buffer, sizeof(json_buffer), 
             "{\"t\": %.1f, \"h\": %.1f}", 
             current_temperature, current_humidity);
             
    // Публикуем с QoS 1, чтобы обязательно сработало событие MQTT_EVENT_PUBLISHED
    int msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC, json_buffer, 0, 1, 0);
    
    if (msg_id >= 0) {
        ESP_LOGI(MQTT_TAG, "Данные отправлены в очередь. Текст: %s (Msg ID: %d)", json_buffer, msg_id);
    } else {
        ESP_LOGE(MQTT_TAG, "Ошибка постановки сообщения в очередь MQTT!");
        goToSleep(); // Если не смогли даже отправить, то засыпаем, чтобы не зависнуть
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "Брокер MQTT успешно подключен! Инициируем отправку данных...");
            global_mqtt_client = event->client;
            
            // ВЫЗЫВАЕМ ОТПРАВКУ СТРОГО ПОСЛЕ ПОДКЛЮЧЕНИЯ
            mqtt_data_publish(event->client);
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "Брокер подтвердил получение сообщения! ID: %d. Задача выполнена.", event->msg_id);
            
            // ТЕПЕРЬ МОЖНО СПАТЬ: Данные гарантированно на сервере
            goToSleep();
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "Соединение с MQTT брокером разорвано или не установлено.");
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(MQTT_TAG, "Произошла ошибка в работе MQTT.");
            goToSleep(); // В любой непонятной ситуации — спим, сохраняя батарею
            break;
            
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /*esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif != NULL) {
            esp_netif_dhcpc_stop(netif);

            esp_netif_ip_info_t ip4_info;
            memset(&ip4_info, 0, sizeof(esp_netif_ip_info_t));

            esp_netif_str_to_ip4("192.168.1.178", &ip4_info.ip);
            esp_netif_str_to_ip4("192.168.1.1", &ip4_info.gw);
            esp_netif_str_to_ip4("255.255.255.0", &ip4_info.netmask);
            esp_err_t err = esp_netif_set_ip_info(netif, &ip4_info);
            if (err == ESP_OK) {
                ESP_LOGI(MQTT_TAG, "Статический IP успешно настроен: 192.168.1.178");
            } else {
                ESP_LOGI(MQTT_TAG, "Ошибка настройки статического IP: %s", esp_err_to_name(err));
            }
        }*/
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(MQTT_TAG, "Wi-Fi не подключен. Повторная попытка...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(MQTT_TAG, "Успешно! Получен IP адрес " IPSTR, IP2STR(&event->ip_info.ip));

        static bool mqtt_initialized = false;
        if (!mqtt_initialized) {
            esp_mqtt_client_config_t mqtt_conf = { .broker.address.uri = BROKER_URL };
            esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_conf);
            esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
            esp_mqtt_client_start(client);
            mqtt_initialized = true;
            
        }
    }
}

void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t conf = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&conf);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_conf = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /*.channel = 8,
            .scan_method = WIFI_FAST_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL*/
        },
    };
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_conf);
    esp_wifi_set_max_tx_power(82);
    esp_wifi_start();
}

void app_main(void) { 
    init_display();
    oled_clear();

    // 1. Опрашиваем датчик (5 секунд паузы, затем чтение)
    vTaskDelay(pdMS_TO_TICKS(5000));
    dht_update_current_values();

    display_weather_data(current_temperature, current_humidity);

    // Инициализация памяти NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Запускаем цепочку сетевых подключений
    ESP_LOGI(MQTT_TAG, "Инициализация Wi-Fi...");
    wifi_init_sta();

    // 3. Защитный сторожевой таймер (Watchdog)
    // Если за 30 секунд роутер или брокер заглючат и мы не дойдем до отправки,
    // эта задержка прервется и принудительно уложит плату спать, спасая аккумулятор.
    /*
    vTaskDelay(pdMS_TO_TICKS(30000));
    ESP_LOGE(MQTT_TAG, "Таймаут сети (30 сек). Плата не смогла отправить данные. Уходим в защитный сон.");
    goToSleep();*/
}