/* ============================================================================
 * components/nimble_ctrl/src/nimble_ctrl.c
 * ============================================================================
 *
 * NOTE (v1 assumptions):
 * - Your nimble_ctrl_cfg_t uses option (A): app provides a svc-def array getter.
 *   That means your header should have something like:
 *
 *     typedef const struct ble_gatt_svc_def* (*nimble_ctrl_gatt_svcs_get_fn)(void* user);
 *     ...
 *     nimble_ctrl_gatt_svcs_get_fn gatt_svcs_get; // optional unless peripheral
 *
 * - Callbacks execute in NimBLE host task context (do not block).
 */

#include "nimble_ctrl/nimble_ctrl.h"
#include "nimble_ctrl_priv.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"
#include "nvs_flash.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

/* Bond store (only used if bonding enabled) */
#include "store/config/ble_store_config.h"

/* GATT/GAP service (optional but common) */
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

void ble_store_config_init(void);

/* Internal shared context (single instance) */

static const char* TAG = "nimble_ctrl";
static nimble_ctrl_ctx_t s_ctx;

nimble_ctrl_ctx_t* nimble_ctrl_ctx_get(void)
{
    return &s_ctx;
}

/* Resolve compiled-in roles into a mask */
static nimble_ctrl_role_mask_t compiled_roles_mask(void)
{
    nimble_ctrl_role_mask_t m = 0;
#if CONFIG_BT_NIMBLE_ROLE_PERIPHERAL
    m |= NIMBLE_CTRL_ROLE_PERIPHERAL;
#endif
#if CONFIG_BT_NIMBLE_ROLE_CENTRAL
    m |= NIMBLE_CTRL_ROLE_CENTRAL;
#endif
#if CONFIG_BT_NIMBLE_ROLE_BROADCASTER
    m |= NIMBLE_CTRL_ROLE_BROADCASTER;
#endif
#if CONFIG_BT_NIMBLE_ROLE_OBSERVER
    m |= NIMBLE_CTRL_ROLE_OBSERVER;
#endif
    return m;
}

/* Internal helpers */
static void ble_host_task(void* param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();                  /* Returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
    ESP_LOGI(TAG, "NimBLE host task stopped");
}

static int ensure_nvs_ready(void)
{
    /* NimBLE uses NVS for PHY calib; bonding uses NVS via ble_store_config_init() */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return (ret == ESP_OK) ? 0 : -EIO;
}

static void apply_security_cfg(const nimble_ctrl_security_cfg_t* sc)
{
    /* Keep this conservative; adjust as needed for your product policy */
    ble_hs_cfg.sm_io_cap = sc->io_capabilities;
    ble_hs_cfg.sm_bonding = sc->bonding ? 1 : 0;
    ble_hs_cfg.sm_mitm = sc->mitm ? 1 : 0;
    ble_hs_cfg.sm_sc = sc->secure_connections ? 1 : 0;

    /* Exchange encryption keys if bonding enabled */
    if (sc->bonding) {
        ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
        ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
        /* If you later enable privacy / resolving addresses, also set ID dist. */
    }
}

/* API */

int nimble_ctrl_init(const nimble_ctrl_cfg_t* cfg)
{
    if (cfg == NULL) return -EINVAL;
    if (s_ctx.inited) return 0;

    int rc = ensure_nvs_ready();
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_nvs_ready failed rc=%d", rc);
        return rc;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    /* Copy cfg by value so the caller can pass a stack object safely */
    memcpy(&s_ctx.cfg, cfg, sizeof(*cfg));

    const nimble_ctrl_role_mask_t compiled = compiled_roles_mask();
    if (compiled == 0) return -ENOTSUP;

    s_ctx.roles_effective = (cfg->roles != 0) ? (cfg->roles & compiled) : compiled;
    if (s_ctx.roles_effective == 0) return -ENOTSUP;

    rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed rc=%d", rc);
        return -EIO;
    }

    /* Host callbacks / GAP setup lives in nimble_ctrl_gap.c */
    rc = nimble_ctrl_gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_ctrl_gap_init failed rc=%d", rc);
        return rc;
    }
    /* Security (optional) */
    if (s_ctx.cfg.security.enable) {
        ESP_LOGI(TAG, "Applying security configuration");
        apply_security_cfg(&s_ctx.cfg.security);
        if (s_ctx.cfg.security.bonding) {
            /* Enables default NimBLE store config */
            ESP_LOGI(TAG, "Initializing BLE bond storage");
            ble_store_config_init();
            ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
        }
    }

#if MYNEWT_VAL(BLE_GATTS)
    /* GATT init only if PERIPHERAL role effective */
    ESP_LOGI(TAG, "s_ctx.roles_effective=0x%02x", s_ctx.roles_effective);
    if (s_ctx.roles_effective & NIMBLE_CTRL_ROLE_PERIPHERAL) {
        rc = nimble_ctrl_gatt_init();
        if (rc != 0) return rc;
    }
#endif

    s_ctx.inited = true;
    return 0;
}

int nimble_ctrl_start(void)
{
    if (!s_ctx.inited) return -EINVAL;
    if (s_ctx.started) return 0;

    nimble_port_freertos_init(ble_host_task);

    /* Any "start" logic that must run post-task creation (optional) */
    int rc = nimble_ctrl_gap_start();
    if (rc != 0) return rc;

    s_ctx.started = true;
    return 0;
}

int nimble_ctrl_stop(void)
{
    if (!s_ctx.started) return 0;

    (void)nimble_ctrl_gap_stop();
    nimble_port_stop(); /* Host task will exit and deinit in ble_host_task() */

    s_ctx.started = false;
    return 0;
}

int nimble_ctrl_adv_start(void)
{
    if (!s_ctx.inited) return -EINVAL;
    if (!s_ctx.started) {
        ESP_LOGE(TAG, "nimble_ctrl_adv_start called when not started");
        return -ENETDOWN;
    }
    if (!(s_ctx.roles_effective & (NIMBLE_CTRL_ROLE_PERIPHERAL | NIMBLE_CTRL_ROLE_BROADCASTER))) {
        return -ENOTSUP;
    }
    if (!s_ctx.cfg.advertising.enable) return 0;
    return nimble_ctrl_gap_adv_start();
}

int nimble_ctrl_adv_stop(void)
{
    if (!s_ctx.inited) return -EINVAL;
    return nimble_ctrl_gap_adv_stop();
}

int nimble_ctrl_notify(uint16_t conn_handle, uint16_t attr_handle, const void* data, size_t len)
{
    if (!s_ctx.inited) return -EINVAL;
    if (!(s_ctx.roles_effective & NIMBLE_CTRL_ROLE_PERIPHERAL)) return -ENOTSUP;
    if (data == NULL || len == 0) return -EINVAL;
    return nimble_ctrl_gatt_notify(conn_handle, attr_handle, data, len);
}
#ifdef __cplusplus
}
#endif
