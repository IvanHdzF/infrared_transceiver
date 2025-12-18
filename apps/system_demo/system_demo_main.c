#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "mocks.h"
#include "retrofit_os_types.h" 


//TODO: Include a generic types header that enumerates system events, types, etc.

#define SYSTEM_DEMO_TICK_DELAY_MS 1000

static const char *TAG = "SYS_DEMO_MAIN";

/* System Demo mocks */
static void system_demo_init(void)
{
    // Initialization code for the system demo application
    ESP_LOGI(TAG, "System demo initialized.");

    // Initialize components 
    mock_event_bus_init();
    mock_storage_init();
    mock_clock_init();
    mock_errmgr_init();
    mock_orch_init();
    mock_auth_init();
    mock_ble_init();
    mock_wifi_init();
    mock_power_init();  /* optional */
    mock_sched_init();
    mock_ir_init();
    mock_cmd_init();

}

static void system_demo_run(void)
{
    // Main loop code for the system demo application
    // This may include handling events, processing data, etc.
    while (1) {
        // Example: Print a message every second
        static uint32_t counter = 0;
        ESP_LOGI(TAG, "System demo is running...");
        mock_system_step(counter);
        counter++;
        vTaskDelay(pdMS_TO_TICKS(SYSTEM_DEMO_TICK_DELAY_MS));
    }
}

void app_main(void)
{
    // Initialize the system demo application
    system_demo_init();

    // Start the main loop of the system demo
    system_demo_run();
}