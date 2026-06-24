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
#define BROKER_URL          "mqtt://192.168.1.218:1883"
#define MQTT_TOPIC          "test/esp32c3/data"
#define LED_PIN             8
#define TIME_TO_SLEEP(min)  min * 60 * 1000000ULL
#define TIME_TO_SLEEP_15S   15 * 1000000ULL

static const char *MQTT_TAG = "MQTT_MAIN",
                  *SLEEP_MODE_TAG = "SLEEP_MODE_TAG",
                  *DEBUG_TAG = "DEBUG_TAG";

// Глобальный указатель на MQTT-клиента (нужен для отправки из таски)
static esp_mqtt_client_handle_t global_mqtt_client = NULL;
// Флаг, сигнализирующий, что есть активное подключение к брокеру
static volatile bool mqtt_connected = false;

void goToSleep() {
    ESP_LOGI(SLEEP_MODE_TAG, "Станция переходит в спящий режим");
    esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP_15S);
    esp_wifi_stop();
    gpio_set_level(LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50)); // Даем логам успеть выплюнуться в UART
    esp_deep_sleep_start();
}

// Новая задача: отправка данных метеостанции на сервер
void mqtt_data_publish() {
    char json_buffer[128];
    // Отправляем данные только если Wi-Fi и MQTT успешно подключены
    if (mqtt_connected && global_mqtt_client != NULL) {
        // Формируем красивую JSON-строку с данными датчика
        // %.1f вставит вещественное число с одним знаком после запятой
        snprintf(json_buffer, sizeof(json_buffer), 
                 "{\"t\": %.1f, \"h\": %.1f}", 
                 current_temperature, current_humidity);
        // Публикуем сообщение на брокер (QoS 1 — с подтверждением доставки)
        int msg_id = esp_mqtt_client_publish(global_mqtt_client, MQTT_TOPIC, json_buffer, 0, 1, 0);
        
        if (msg_id >= 0) {
            ESP_LOGI(MQTT_TAG, "Отправлено в топик: %s (ID: %d)", json_buffer, msg_id);
        } else {
            ESP_LOGE(MQTT_TAG, "Ошибка отправки в MQTT!");
        }
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "Брокер MQTT успешно подключен!");
            global_mqtt_client = event->client; // Сохраняем указатель на клиента
            mqtt_data_publish();
            //mqtt_connected = true;              // Разрешаем отправку данных
            break;
            
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "Брокер подтвердил получение сообщения! ID: %d", event->msg_id);
            goToSleep();
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(MQTT_TAG, "Потеряно соединение с MQTT брокером.");
            //mqtt_connected = false;               // Запрещаем отправку, пока связи нет
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(MQTT_TAG, "Произошла ошибка в работе MQTT.");
            goToSleep();
            break;
            
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(MQTT_TAG, "Wi-Fi отключен. Выполняю переподключение...");
        mqtt_connected = false; // Если упал Wi-Fi, то и MQTT слать нельзя
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(MQTT_TAG, "Получен IP адрес " IPSTR, IP2STR(&event->ip_info.ip));
        
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
        },
    };
    
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_conf);
    esp_wifi_start();
}

void app_main(void) { 
    ESP_LOGI(DEBUG_TAG, "Мы пришли к началу app_main");
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_PIN, 1);

    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(DEBUG_TAG, "Мы пришли к началу опроса датчика");
    dht_update_current_values();
    // Инициализация NVS и запуск Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(MQTT_TAG, "Инициализация Wi-Fi...");
    wifi_init_sta();

    vTaskDelay(pdMS_TO_TICKS(20000));
    ESP_LOGI(SLEEP_MODE_TAG, "Сторожевой таймер истек, аварийный уход в сон!");
    goToSleep();
}