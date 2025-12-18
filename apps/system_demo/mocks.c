/* mocks.c — Sprint 0 wiring mocks (no real HW, no real bus)
 *
 * Goal: let app_main/orchestrator skeleton compile + run, and emit fake events.
 */

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "retrofit_os_types.h"   /* EVT_* / payload structs / os_evt_t */
#include "mocks.h"

static const char *TAG = "MOCKS";

/* -------------------------------------------------------------------------- */
/* Mock module contexts (keep tiny; later these become real module ctx structs) */
/* -------------------------------------------------------------------------- */

typedef struct { uint8_t authed; } mock_auth_t;
typedef struct { os_link_state_t ble_up; } mock_ble_t;
typedef struct { os_link_state_t wifi_up; } mock_wifi_t;
typedef struct { os_power_mode_t pwr; } mock_power_t;

static mock_auth_t  g_auth;
static mock_ble_t   g_ble;
static mock_wifi_t  g_wifi;
static mock_power_t g_pwr;

/* -------------------------------------------------------------------------- */
/* Fake “event bus deliver” for Sprint 0.
 * Replace with real event_bus_publish() later.
 * For now we just log the event and (optionally) call orchestrator hook.
 * -------------------------------------------------------------------------- */

static void mock_publish(os_mod_id_t src, os_evt_id_t id, const void *payload, uint16_t len)
{
  os_evt_t evt = {
    .id = id,
    .src = src,
    .ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
    .len = len,
  };

  if (len > OS_EVT_INLINE_MAX) {
    ESP_LOGE(TAG, "publish drop: len=%u > OS_EVT_INLINE_MAX=%u", (unsigned)len, (unsigned)OS_EVT_INLINE_MAX);
    return;
  }
  if (payload && len){
    memcpy(evt.payload, payload, len);
  }

  ESP_LOGI(TAG, "EVT id=%u src=%u len=%u", (unsigned)evt.id, (unsigned)evt.src, (unsigned)evt.len);

  /* TODO Sprint 0: if you have an orchestrator_process(evt) stub, call it here:
   * mock_orch_process(&evt);
   */
}

/* -------------------------------------------------------------------------- */
/* Mock init APIs (match your planned “real” module init names eventually)     */
/* -------------------------------------------------------------------------- */

os_err_t mock_auth_init(void)
{
  memset(&g_auth, 0, sizeof(g_auth));
  ESP_LOGI(TAG, "mock_auth_init");
  return OS_OK;
}

os_err_t mock_ble_init(void)
{
  memset(&g_ble, 0, sizeof(g_ble));
  ESP_LOGI(TAG, "mock_ble_init");
  return OS_OK;
}

os_err_t mock_wifi_init(void)
{
  memset(&g_wifi, 0, sizeof(g_wifi));
  ESP_LOGI(TAG, "mock_wifi_init");
  return OS_OK;
}

os_err_t mock_power_init(void)
{
  g_pwr.pwr = PWR_ACTIVE;
  ESP_LOGI(TAG, "mock_power_init");
  return OS_OK;
}

os_err_t mock_ir_init(void)      { ESP_LOGI(TAG, "mock_ir_init"); return OS_OK; }
os_err_t mock_sched_init(void)   { ESP_LOGI(TAG, "mock_sched_init"); return OS_OK; }
os_err_t mock_storage_init(void) { ESP_LOGI(TAG, "mock_storage_init"); return OS_OK; }
os_err_t mock_clock_init(void)   { ESP_LOGI(TAG, "mock_clock_init"); return OS_OK; }
os_err_t mock_cmd_init(void)     { ESP_LOGI(TAG, "mock_cmd_init"); return OS_OK; }
os_err_t mock_orch_init(void)    { ESP_LOGI(TAG, "mock_orch_init"); return OS_OK; }
os_err_t mock_errmgr_init(void)  { ESP_LOGI(TAG, "mock_errmgr_init"); return OS_OK; }
os_err_t mock_event_bus_init(void) { ESP_LOGI(TAG, "mock_event_bus_init"); return OS_OK; }

/* -------------------------------------------------------------------------- */
/* Mock “tick/process” to generate realistic events                            */
/* Call this from your main loop initially.                                   */
/* -------------------------------------------------------------------------- */

void mock_system_step(uint32_t step)
{
  if (step == 0) {
    mock_publish(OS_MOD_ORCH, EVT_HEALTH_TICK, NULL, 0);
    return;
  }

  if ((step % 5u) == 0u) {
    g_ble.ble_up = (g_ble.ble_up == OS_LINK_UP) ? OS_LINK_DOWN : OS_LINK_UP;
    evt_ble_conn_changed_t p = { .state = g_ble.ble_up };
    mock_publish(OS_MOD_BLE, EVT_BLE_CONN_CHANGED, &p, sizeof(p));
  }

  if ((step % 7u) == 0u) {
    g_auth.authed ^= 1u;
    /* define an auth payload later; for now send no payload */
    mock_publish(OS_MOD_AUTH, EVT_AUTH_STATE_CHANGED, NULL, 0);
  }

  if ((step % 11u) == 0u) {
    evt_schedule_due_t p = { .schedule_id = 42u };
    mock_publish(OS_MOD_SCHED, EVT_SCHEDULE_DUE, &p, sizeof(p));
  }
}
