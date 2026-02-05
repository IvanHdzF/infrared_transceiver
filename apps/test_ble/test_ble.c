#include "unity.h"

#include "nimble_ctrl/nimble_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

const char* TAG = "test_ble";

/* GATT DB declaration and getter */
/*** Maximum number of characteristics with the notify flag ***/
#define MAX_NOTIFY 5

static const ble_uuid128_t gatt_svr_svc_uuid =
    BLE_UUID128_INIT(0x2d, 0x71, 0xa2, 0x59, 0xb4, 0x58, 0xc8, 0x12,
                     0x99, 0x99, 0x43, 0x95, 0x12, 0x2f, 0x46, 0x59);

/* A characteristic that can be subscribed to */
static uint8_t gatt_svr_chr_val;
static uint16_t gatt_svr_chr_val_handle;
static const ble_uuid128_t gatt_svr_chr_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
                     0x22, 0x22, 0x22, 0x22, 0x33, 0x33, 0x33, 0x33);

/* A custom descriptor */
static uint8_t gatt_svr_dsc_val;
static const ble_uuid128_t gatt_svr_dsc_uuid =
    BLE_UUID128_INIT(0x01, 0x01, 0x01, 0x01, 0x12, 0x12, 0x12, 0x12,
                     0x23, 0x23, 0x23, 0x23, 0x34, 0x34, 0x34, 0x34);

static int
gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                struct ble_gatt_access_ctxt *ctxt,
                void *arg);

static const struct ble_gatt_svc_def gatt_svc[] = {
    {
        /*** Service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svr_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        { {
                /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                .uuid = &gatt_svr_chr_uuid.u,
                .access_cb = gatt_svc_access,
#if CONFIG_EXAMPLE_ENCRYPTION
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC |
                         BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
#else
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
#endif
                .val_handle = &gatt_svr_chr_val_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = &gatt_svr_dsc_uuid.u,
#if CONFIG_EXAMPLE_ENCRYPTION
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
#else
                        .att_flags = BLE_ATT_F_READ,
#endif
                        .access_cb = gatt_svc_access,
                    }, {
                        0, /* No more descriptors in this characteristic */
                    }
                },
            }, {
                0, /* No more characteristics in this service. */
            }
        },
    },

    {
        0, /* No more services. */
    },
};

const struct ble_gatt_svc_def* gatt_get_svc(void* user)
{
    return gatt_svc;
}

static int
gatt_svr_write(struct os_mbuf *om, uint16_t min_len, uint16_t max_len,
               void *dst, uint16_t *len)
{
    uint16_t om_len;
    int rc;

    om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    rc = ble_hs_mbuf_to_flat(om, dst, max_len, len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return 0;
}

/**
 * Access callback whenever a characteristic/descriptor is read or written to.
 * Here reads and writes need to be handled.
 * ctxt->op tells weather the operation is read or write and
 * weather it is on a characteristic or descriptor,
 * ctxt->dsc->uuid tells which characteristic/descriptor is accessed.
 * attr_handle give the value handle of the attribute being accessed.
 * Accordingly do:
 *     Append the value to ctxt->om if the operation is READ
 *     Write ctxt->om to the value if the operation is WRITE
 **/
static int
gatt_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid;
    int rc;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Characteristic read; conn_handle=%d attr_handle=%d\n",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG, "Characteristic read by NimBLE stack; attr_handle=%d\n",
                     attr_handle);
        }
        uuid = ctxt->chr->uuid;
        if (attr_handle == gatt_svr_chr_val_handle) {
            rc = os_mbuf_append(ctxt->om,
                                &gatt_svr_chr_val,
                                sizeof(gatt_svr_chr_val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto unknown;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Characteristic write; conn_handle=%d attr_handle=%d",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG, "Characteristic write by NimBLE stack; attr_handle=%d",
                     attr_handle);
        }
        uuid = ctxt->chr->uuid;
        if (attr_handle == gatt_svr_chr_val_handle) {
            rc = gatt_svr_write(ctxt->om,
                                sizeof(gatt_svr_chr_val),
                                sizeof(gatt_svr_chr_val),
                                &gatt_svr_chr_val, NULL);
            ble_gatts_chr_updated(attr_handle);
            ESP_LOGI(TAG, "Notification/Indication scheduled for "
                     "all subscribed peers.\n");
            return rc;
        }
        goto unknown;

    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGI(TAG, "Descriptor read; conn_handle=%d attr_handle=%d\n",
                     conn_handle, attr_handle);
        } else {
            ESP_LOGI(TAG, "Descriptor read by NimBLE stack; attr_handle=%d\n",
                     attr_handle);
        }
        uuid = ctxt->dsc->uuid;
        if (ble_uuid_cmp(uuid, &gatt_svr_dsc_uuid.u) == 0) {
            rc = os_mbuf_append(ctxt->om,
                                &gatt_svr_dsc_val,
                                sizeof(gatt_svr_chr_val));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        goto unknown;

    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        goto unknown;

    default:
        goto unknown;
    }

unknown:
    /* Unknown characteristic/descriptor;
     * The NimBLE host should not have called this function;
     */
    assert(0);
    return BLE_ATT_ERR_UNLIKELY;
}

/* Internal helpers */
static void on_connected(uint16_t conn_handle, void* user)
{
    (void)user;
    ESP_LOGI(TAG, "Connected: handle=%u", conn_handle);
}

static void on_disconnected(uint16_t conn_handle, uint8_t reason, void* user)
{
    (void)user;
    ESP_LOGI(TAG, "Disconnected: handle=%u, reason=%u", conn_handle, reason);
}

static void on_security_changed(uint16_t conn_handle,
                                uint8_t level,
                                bool bonded,
                                uint8_t key_size,
                                int status,
                                void* user)
{
    (void)user;
    ESP_LOGI(TAG, "Security changed: handle=%u, level=%u, bonded=%u, key_size=%u, status=%d",
             conn_handle, level, bonded, key_size, status);
}

static void on_mtu_changed(uint16_t conn_handle, uint16_t mtu, void* user)
{
    (void)user;
    ESP_LOGI(TAG, "MTU changed: handle=%u, mtu=%u", conn_handle, mtu);
}
void on_subscribe_changed(uint16_t conn_handle,
                          uint16_t attr_handle,
                          bool notify_enabled,
                          bool indicate_enabled,
                          void* user)
{
    (void)user;
    ESP_LOGI(TAG, "Subscribe changed: handle=%u, attr_handle=%u, notify_enabled=%u, indicate_enabled=%u",
             conn_handle, attr_handle, notify_enabled, indicate_enabled);
}

/* Test cases */
TEST_CASE("nimble_ctrl: Happy path", "[ble]")
{
    int err = 0;
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
        .user = NULL,
    };
    err = nimble_ctrl_init(&cfg);
    TEST_ASSERT_EQUAL_INT16(0, err);

    err = nimble_ctrl_start();
    TEST_ASSERT_EQUAL_INT16(0, err);

    vTaskDelay(pdMS_TO_TICKS(100000));

    err = nimble_ctrl_stop();
    TEST_ASSERT_EQUAL_INT16(0, err);
}

/* ---------------- App entry ---------------- */

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
