// test_ble_nimble_ctrl.c
//

#include "unity.h"

#include "nimble_ctrl/nimble_ctrl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

static const char* TAG = "test_ble";

/* ---------- GATT DB (app side) ---------- */

static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12,
                     0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

static uint8_t gatt_svr_chr_val;
static uint16_t gatt_svr_chr_val_handle;
static const ble_uuid128_t gatt_svr_chr_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
                     0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33);

static uint8_t gatt_svr_dsc_val;
static uint16_t gatt_svr_dsc_handle; // we will capture the descriptor handle
static const ble_uuid128_t gatt_svr_dsc_uuid =
    BLE_UUID128_INIT(0x01, 0x01, 0x01, 0x01, 0x12, 0x12, 0x12, 0x12,
                     0x23, 0x23, 0x23, 0x23, 0x34, 0x34, 0x34, 0x34);

// Descriptor handle capture: NimBLE doesn't provide a direct dsc_handle pointer in the dsc_def.
// If you need descriptor binding, you can:
//  - (A) expose descriptor handles from your service registration layer (recommended), or
//  - (B) bind only characteristics for now.
// For this test, we'll bind the characteristic (storage) and keep descriptor read untested.

static const struct ble_gatt_svc_def gatt_svc[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &gatt_svr_chr_uuid.u,
                // Key change: use nimble_ctrl dispatcher
                .access_cb = nimble_ctrl_gatt_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
                .val_handle = &gatt_svr_chr_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = &gatt_svr_dsc_uuid.u,
                        .att_flags = BLE_ATT_F_READ,
                        // Also managed by nimble_ctrl dispatcher
                        .access_cb = nimble_ctrl_gatt_access_cb,
                        // NOTE: no handle pointer here in NimBLE API
                    },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    { 0 },
};

const struct ble_gatt_svc_def* gatt_get_svc(void* user)
{
    (void)user;
    return gatt_svc;
}

/* ---------- Probe ---------- */

typedef struct {
    EventGroupHandle_t eg;

    uint16_t last_conn_handle;
    uint8_t  last_disc_reason;

    uint16_t last_mtu;

    uint16_t last_attr_handle;
    bool     last_notify_enabled;
    bool     last_indicate_enabled;

    uint8_t  last_sec_level;
    bool     last_sec_bonded;
    uint8_t  last_sec_key_size;
    int      last_sec_status;

    uint32_t last_cb_time_ms;
} cb_probe_t;

#define BIT_CONNECTED    (1u << 0)
#define BIT_DISCONNECTED (1u << 1)
#define BIT_MTU_CHANGED  (1u << 2)
#define BIT_SUB_CHANGED  (1u << 3)
#define BIT_SEC_CHANGED  (1u << 4)

#define PROBE_BITS_ALL   (BIT_CONNECTED | BIT_DISCONNECTED | BIT_MTU_CHANGED | BIT_SUB_CHANGED | BIT_SEC_CHANGED)
#define EVENTGROUP_SAFE_MASK  (0x00FFFFFFu)

static cb_probe_t g_probe;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void cb_probe_zero_snapshots(cb_probe_t* p)
{
    p->last_conn_handle = 0;
    p->last_disc_reason = 0;
    p->last_mtu = 0;
    p->last_attr_handle = 0;
    p->last_notify_enabled = false;
    p->last_indicate_enabled = false;
    p->last_sec_level = 0;
    p->last_sec_bonded = false;
    p->last_sec_key_size = 0;
    p->last_sec_status = 0;
    p->last_cb_time_ms = 0;
}

static void cb_probe_init(cb_probe_t* p)
{
    TEST_ASSERT_NOT_NULL(p);

    if (p->eg == NULL) {
        p->eg = xEventGroupCreate();
        TEST_ASSERT_NOT_NULL_MESSAGE(p->eg, "EventGroupCreate failed");
    }

    (void)xEventGroupClearBits(p->eg, (PROBE_BITS_ALL & EVENTGROUP_SAFE_MASK));
    cb_probe_zero_snapshots(p);
}

static void cb_probe_deinit(cb_probe_t* p)
{
    TEST_ASSERT_NOT_NULL(p);
    if (p->eg) {
        vEventGroupDelete(p->eg);
        p->eg = NULL;
    }
    cb_probe_zero_snapshots(p);
}

static void cb_probe_reset(cb_probe_t* p, EventBits_t bits)
{
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(p->eg);
    (void)xEventGroupClearBits(p->eg, (bits & EVENTGROUP_SAFE_MASK));
}

static void cb_probe_expect(cb_probe_t* p, EventBits_t bit, uint32_t timeout_ms, const char* msg)
{
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(p->eg);

    const EventBits_t got = xEventGroupWaitBits(
                                p->eg,
                                (bit & EVENTGROUP_SAFE_MASK),
                                pdTRUE,
                                pdTRUE,
                                pdMS_TO_TICKS(timeout_ms)
                            );

    if ((got & bit) != bit) {
        TEST_FAIL_MESSAGE(msg);
    }
}

/* ---------- Callbacks ---------- */

static cb_probe_t* probe_from_user(void* user)
{
    return (cb_probe_t*)user;
}

static void on_connected(uint16_t conn_handle, void* user)
{
    cb_probe_t* p = probe_from_user(user);
    if (!p || !p->eg) return;
    p->last_conn_handle = conn_handle;
    p->last_cb_time_ms = now_ms();
    xEventGroupSetBits(p->eg, BIT_CONNECTED);
    ESP_LOGI(TAG, "Connected: handle=%u", conn_handle);
}

static void on_disconnected(uint16_t conn_handle, uint8_t reason, void* user)
{
    cb_probe_t* p = probe_from_user(user);
    if (!p || !p->eg) return;
    p->last_conn_handle = conn_handle;
    p->last_disc_reason = reason;
    p->last_cb_time_ms = now_ms();
    xEventGroupSetBits(p->eg, BIT_DISCONNECTED);
    ESP_LOGI(TAG, "Disconnected: handle=%u, reason=%u", conn_handle, reason);
}

static void on_security_changed(uint16_t conn_handle,
                                uint8_t level,
                                bool bonded,
                                uint8_t key_size,
                                int status,
                                void* user)
{
    cb_probe_t* p = probe_from_user(user);
    if (!p || !p->eg) return;
    p->last_conn_handle = conn_handle;
    p->last_sec_level = level;
    p->last_sec_bonded = bonded;
    p->last_sec_key_size = key_size;
    p->last_sec_status = status;
    p->last_cb_time_ms = now_ms();
    xEventGroupSetBits(p->eg, BIT_SEC_CHANGED);
    ESP_LOGI(TAG, "Security changed: handle=%u level=%u bonded=%u key_size=%u status=%d",
             conn_handle, level, bonded, key_size, status);
}

static void on_mtu_changed(uint16_t conn_handle, uint16_t mtu, void* user)
{
    cb_probe_t* p = probe_from_user(user);
    if (!p || !p->eg) return;
    p->last_conn_handle = conn_handle;
    p->last_mtu = mtu;
    p->last_cb_time_ms = now_ms();
    xEventGroupSetBits(p->eg, BIT_MTU_CHANGED);
    ESP_LOGI(TAG, "MTU changed: handle=%u mtu=%u", conn_handle, mtu);
}

static void on_subscribe_changed(uint16_t conn_handle,
                                 uint16_t attr_handle,
                                 bool notify_enabled,
                                 bool indicate_enabled,
                                 void* user)
{
    cb_probe_t* p = probe_from_user(user);
    if (!p || !p->eg) return;
    p->last_conn_handle = conn_handle;
    p->last_attr_handle = attr_handle;
    p->last_notify_enabled = notify_enabled;
    p->last_indicate_enabled = indicate_enabled;
    p->last_cb_time_ms = now_ms();
    xEventGroupSetBits(p->eg, BIT_SUB_CHANGED);
    ESP_LOGI(TAG, "Subscribe changed: handle=%u attr_handle=%u notify=%u indicate=%u",
             conn_handle, attr_handle, notify_enabled, indicate_enabled);
}

/* ---------- Helpers ---------- */

static void wait_until_started(uint32_t timeout_ms)
{
    const uint32_t start = now_ms();
    while (!nimble_ctrl_test_is_started()) {
        if ((now_ms() - start) > timeout_ms) {
            TEST_ASSERT_MESSAGE(false, "Timeout waiting for NimBLE start");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static nimble_ctrl_cfg_t make_cfg(cb_probe_t* probe)
{
    nimble_ctrl_cfg_t cfg = {
        .roles = NIMBLE_CTRL_ROLE_PERIPHERAL,
        .advertising = {
            .enable = true,
            .interval_min = 100,
            .interval_max = 150,
            .auto_restart_on_disconnect = true,
        },
        .cbs = {
            .on_connected = on_connected,
            .on_disconnected = on_disconnected,
            .on_mtu_changed = on_mtu_changed,
            .on_subscribe_changed = on_subscribe_changed,
            .on_security_changed = on_security_changed,
        },
        .security = {
            .enable = false,
            .bonding = false,
            .mitm = false,
            .secure_connections = false,
            .io_capabilities = 0,
            .key_size_min = 0,
            .key_size_max = 0,
        },
        .get_gatt_svc = gatt_get_svc,
        .user = probe,
    };
    return cfg;
}

// Bind GATT handlers after nimble_ctrl_init() so handles are valid.
static void bind_gatt_after_init(void)
{
    // At this point gatt_svr_chr_val_handle is assigned by ble_gatts_add_svcs.
    TEST_ASSERT_NOT_EQUAL_UINT16_MESSAGE(0, gatt_svr_chr_val_handle, "val_handle not assigned (GATT not registered yet?)");

    // For this test, bind storage for the characteristic to avoid writing any access switch.
    // Your gatt_db layer will handle:
    //  - READ_CHR: os_mbuf_append(buf, max_len)
    //  - WRITE_CHR: ble_hs_mbuf_to_flat -> buf, with min/max len checks
    //  - notify_on_write: ble_gatts_chr_updated(handle)
    int rc = nimble_ctrl_gatt_bind_storage(
                 gatt_svr_chr_val_handle,
                 &gatt_svr_chr_val,
                 sizeof(gatt_svr_chr_val),
                 sizeof(gatt_svr_chr_val),
                 true,
                 NULL
             );
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "nimble_ctrl_gatt_bind_storage failed");

    // Optional: if/when you expose descriptor handles (or provide a resolve API),
    // you can bind the descriptor read handler here too.
    (void)gatt_svr_dsc_handle;
}

/* ---------- Unity fixtures ---------- */

void setUp(void)
{
    cb_probe_init(&g_probe);
}

void tearDown(void)
{
    (void)nimble_ctrl_stop();
    // If you expose a gatt clear API, call it to keep tests hermetic.
    // (No-op if db reset is already part of stop/init.)
    // nimble_ctrl_gatt_clear_all();
    cb_probe_deinit(&g_probe);
}

/* ---------- Tests ---------- */

TEST_CASE("nimble_ctrl: start/stop basic", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);
    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
}

TEST_CASE("nimble_ctrl: start twice, stop twice", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);

    (void)nimble_ctrl_start();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
    (void)nimble_ctrl_stop();
}

TEST_CASE("nimble_ctrl: connect/disconnect callbacks fire", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);

    struct ble_gap_event evt = {
        .type = BLE_GAP_EVENT_CONNECT,
        .connect = { .status = 0, .conn_handle = 1 },
    };

    cb_probe_reset(&g_probe, BIT_CONNECTED);
    nimble_ctrl_test_gap_inject_evt(&evt, NULL);
    cb_probe_expect(&g_probe, BIT_CONNECTED, 1000, "on_connected not called");
    TEST_ASSERT_EQUAL_UINT16(1, g_probe.last_conn_handle);

    evt.type = BLE_GAP_EVENT_DISCONNECT;
    evt.disconnect.conn = (struct ble_gap_conn_desc) {
        .conn_handle = 1
    };
    evt.disconnect.reason = 0x13;

    cb_probe_reset(&g_probe, BIT_DISCONNECTED);
    nimble_ctrl_test_gap_inject_evt(&evt, NULL);
    cb_probe_expect(&g_probe, BIT_DISCONNECTED, 1000, "on_disconnected not called");
    TEST_ASSERT_EQUAL_UINT16(1, g_probe.last_conn_handle);
    TEST_ASSERT_EQUAL_UINT8(0x13, g_probe.last_disc_reason);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
}

TEST_CASE("nimble_ctrl: mtu changed callback fires", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);

    struct ble_gap_event evt = {
        .type = BLE_GAP_EVENT_MTU,
        .mtu = { .conn_handle = 2, .value = 185 },
    };

    cb_probe_reset(&g_probe, BIT_MTU_CHANGED);
    nimble_ctrl_test_gap_inject_evt(&evt, NULL);
    cb_probe_expect(&g_probe, BIT_MTU_CHANGED, 1000, "on_mtu_changed not called");
    TEST_ASSERT_EQUAL_UINT16(2, g_probe.last_conn_handle);
    TEST_ASSERT_EQUAL_UINT16(185, g_probe.last_mtu);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
}

TEST_CASE("nimble_ctrl: subscribe callback fires", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);

    struct ble_gap_event evt = {
        .type = BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe = {
            .conn_handle = 3,
            .attr_handle = 0x002A,
            .cur_notify = 1,
            .cur_indicate = 0,
        },
    };

    cb_probe_reset(&g_probe, BIT_SUB_CHANGED);
    nimble_ctrl_test_gap_inject_evt(&evt, NULL);
    cb_probe_expect(&g_probe, BIT_SUB_CHANGED, 1000, "on_subscribe_changed not called");
    TEST_ASSERT_EQUAL_UINT16(3, g_probe.last_conn_handle);
    TEST_ASSERT_EQUAL_UINT16(0x002A, g_probe.last_attr_handle);
    TEST_ASSERT_TRUE(g_probe.last_notify_enabled);
    TEST_ASSERT_FALSE(g_probe.last_indicate_enabled);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
}

TEST_CASE("nimble_ctrl: security changed callback fires", "[ble]")
{
    nimble_ctrl_cfg_t cfg = make_cfg(&g_probe);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_init(&cfg));
    bind_gatt_after_init();

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_start());
    wait_until_started(5000);

    struct ble_gap_event evt = {
        .type = BLE_GAP_EVENT_ENC_CHANGE,
        .enc_change = {
            .status = 0,
            .conn_handle = 4,
        },
    };

    cb_probe_reset(&g_probe, BIT_SEC_CHANGED);
    nimble_ctrl_test_gap_inject_evt(&evt, NULL);
    cb_probe_expect(&g_probe, BIT_SEC_CHANGED, 1000, "on_security_changed not called");

    TEST_ASSERT_EQUAL_UINT16(4, g_probe.last_conn_handle);

    TEST_ASSERT_EQUAL_INT16(0, nimble_ctrl_stop());
}

/* ---------- App entry ---------- */

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
