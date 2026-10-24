/*
 * Vittil quickstart: connect to Wi-Fi, start Vittil, and run.
 *
 * Before the first build, set the project key and the Wi-Fi name and
 * password in sdkconfig.defaults (or later in idf.py menuconfig, under Vittil
 * and Example Connection Configuration), then idf.py build flash monitor.
 * Press the board's BOOT button for a test crash.
 */
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#include "vittil.h"

static const char *TAG = "quickstart";

/* The BOOT button of Espressif's development boards. A press counts only
 * after the pin has read released, so a pin held low from boot never crashes
 * the example. */
#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || \
    CONFIG_IDF_TARGET_ESP32C61 || CONFIG_IDF_TARGET_ESP32H2
#define TEST_CRASH_GPIO GPIO_NUM_9
#elif CONFIG_IDF_TARGET_ESP32C5
#define TEST_CRASH_GPIO GPIO_NUM_28
#elif CONFIG_IDF_TARGET_ESP32P4
#define TEST_CRASH_GPIO GPIO_NUM_35
#else
#define TEST_CRASH_GPIO GPIO_NUM_0
#endif

/* Connect to Wi-Fi without aborting on a wrong setting: the example keeps
 * running offline and says what to fix, instead of rebooting in a loop. */
static void connect_wifi(void)
{
#if CONFIG_EXAMPLE_CONNECT_WIFI && !CONFIG_EXAMPLE_WIFI_SSID_PWD_FROM_STDIN
    const size_t ssid = strlen(CONFIG_EXAMPLE_WIFI_SSID);
    const size_t pass = strlen(CONFIG_EXAMPLE_WIFI_PASSWORD);

    if (ssid == 0) {
        ESP_LOGE(TAG, "no Wi-Fi name: set CONFIG_EXAMPLE_WIFI_SSID in sdkconfig "
                      "(idf.py menuconfig, Example Connection Configuration); running offline");
        return;
    }
    if (pass > 0 && pass < 8) {
        ESP_LOGE(TAG, "Wi-Fi password is %u characters, a WPA password has at least 8: "
                      "check CONFIG_EXAMPLE_WIFI_PASSWORD; running offline", (unsigned)pass);
        return;
    }
#endif
    if (example_connect() != ESP_OK) {
        ESP_LOGE(TAG, "the network did not connect: check Example Connection Configuration "
                      "in idf.py menuconfig; running offline");
    }
}

void app_main(void)
{
    /* Start Vittil first: it reports the last crash and pings once the
     * network is up, and never blocks app_main. */
    vittil_start();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    connect_wifi();

    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << TEST_CRASH_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_LOGI(TAG, "press BOOT for a test crash");

    bool released = false;
    for (int t = 0;; t++) {
        if (t % 100 == 0) {
            ESP_LOGI(TAG, "running, %d s", t / 10);
        }
        const int level = gpio_get_level(TEST_CRASH_GPIO);
        if (level == 1) {
            released = true;
        } else if (released) {
            ESP_LOGW(TAG, "BOOT pressed: test crash");
            abort();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
