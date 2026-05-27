#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "camera-display Sprint 1 boot");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
