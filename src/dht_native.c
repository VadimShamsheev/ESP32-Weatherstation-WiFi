#include "dht_native.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *DHT_TAG = "DHT_NATIVE";

volatile float current_temperature = 0.0;
volatile float current_humidity = 0.0;

static int wait_or_timeout(uint16_t micro_sec, int level) {
    int micros_passed = 0;
    while (gpio_get_level(DHT_GPIO_PIN) == level) {
        if (micros_passed++ > micro_sec) {
            return -1;
        }
        esp_rom_delay_us(1);
    }
    return micros_passed;
}

static esp_err_t read_dht_native(float *humidity, float *temperature) {
    uint8_t data[5] = { 0, 0, 0, 0, 0 };
    gpio_set_direction(DHT_GPIO_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_GPIO_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DHT_GPIO_PIN, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(DHT_GPIO_PIN, GPIO_MODE_INPUT);
    if (wait_or_timeout(80, 0) < 0) return ESP_ERR_TIMEOUT;
    if (wait_or_timeout(80, 1) < 0) return ESP_ERR_TIMEOUT;

    for (int i = 0; i < 40; i++) {
        if (wait_or_timeout(50, 0) < 0) return ESP_ERR_TIMEOUT;
        int duration = wait_or_timeout(70, 1);
        if (duration < 0) return ESP_ERR_TIMEOUT;
        if (duration > 30) {
            data[i / 8] |= (1 << (7 - (i % 8)));
        }
    }

    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) 
        return ESP_ERR_INVALID_CRC;
    int16_t raw_humidity = (data[0] << 8) | data[1];
    int16_t raw_temperature = (data[2] << 8) | data[3];

    if (raw_temperature & 0x8000) {
        raw_temperature = -(raw_temperature & 0x7FFF);
    }

    *humidity = raw_humidity / 10.0f;
    *temperature = raw_temperature / 10.0f;

    return ESP_OK;
}

void dht_update_current_values() {
    float t = 0, h = 0;
    gpio_pullup_en(DHT_GPIO_PIN);
    vTaskDelay(pdMS_TO_TICKS(1000));

    esp_err_t err = read_dht_native(&h, &t);
    if (err == ESP_OK) {
        current_humidity = h;
        current_temperature = t;
        ESP_LOGI(DHT_TAG, "Данные опроса датчика: T = %.1f, H = %.1f", t, h);
    } else if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGI(DHT_TAG, "Превышено время ожидания ответа от датчика!");
    } else if (err == ESP_ERR_INVALID_CRC) {
        ESP_LOGI(DHT_TAG, "Неверная контрольная сумма, данные повреждены!");
    }
}