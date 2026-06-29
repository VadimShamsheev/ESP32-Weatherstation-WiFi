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

#define I2C_MASTER_SDA_IO   8
#define I2C_MASTER_SCL_IO   9
#define I2C_MASTER_FREQ_HZ  400000

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

void app_main(void) { 
    // Инициализация памяти NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    init_display();
    oled_clear();

    // Ожидаем инициализации датчика и дисплея
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        dht_update_current_values();
        oled_clear();
        display_weather_data(current_temperature, current_humidity);
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
    }
}