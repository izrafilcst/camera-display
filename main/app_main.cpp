#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "pinout.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot");
    bool ok = display_init(LCD_SPI_HZ_SAFE);
    ESP_LOGI(TAG, "display_init returned %d", ok);
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
