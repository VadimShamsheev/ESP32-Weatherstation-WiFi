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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"

// ПОДКЛЮЧАЕМ НАШ НАТИВНЫЙ ДАТЧИК
#include "dht_native.h" 

#define WIFI_SSID           "SNR-CPE-4740"
#define WIFI_PASS           "9012100280"
#define BROKER_URL          "mqtt://192.168.1.119:1883"
#define MQTT_TOPIC          "home/weather/metrics"
#define LED_PIN             8

#define TIME_TO_SLEEP_15M   15 * 60 * 1000000ULL

static const char *MQTT_TAG = "MQTT_MAIN",
                  *SLEEP_MODE_TAG = "SLEEP_MODE_TAG";

static esp_mqtt_client_handle_t global_mqtt_client = NULL;

typedef enum {
    LED_WIFI_CONNECTING,
    LED_WIFI_CONNECTED,
    LED_WIFI_ERROR
} led_enum;

led_enum current_led_mode;

void led_task(void *pvParameters) {
    switch (current_led_mode) {
        case LED_WIFI_CONNECTING:
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(500));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        case LED_WIFI_CONNECTED:
            gpio_set_level(LED_PIN, 1);
            break;
        case LED_WIFI_ERROR:
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        default:
            break;
    }
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
            gpio_set_level(LED_PIN, 1);
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
        current_led_mode = LED_WIFI_CONNECTING;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(MQTT_TAG, "Wi-Fi не подключен. Повторная попытка...");
        current_led_mode = LED_WIFI_ERROR;
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(MQTT_TAG, "Успешно! Получен IP адрес " IPSTR, IP2STR(&event->ip_info.ip));
        current_led_mode = LED_WIFI_CONNECTED;
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
    // Настраиваем светодиод — признак бодрствования станции
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 0);

    //xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    // Раскомментируй это, когда будешь заливать финальный код в плату на улице
    /*
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("MQTT_MAIN", ESP_LOG_INFO);
    esp_log_level_set("SLEEP_MODE_TAG", ESP_LOG_INFO);
    */
    
    // 1. Опрашиваем датчик (5 секунд паузы, затем чтение)
    vTaskDelay(pdMS_TO_TICKS(5000));
    dht_update_current_values();

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