/* ============================================================================
 * components/nimble_ctrl/src/nimble_ctrl_gatt.c
 * ============================================================================
 */
#include "nimble_ctrl/nimble_ctrl.h"
#include "nimble_ctrl_priv.h"

#include <string.h>
#include <errno.h>

#include "esp_log.h"

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#ifdef __cplusplus
extern "C" {
#endif

static const char* TAG_GATT = "nimble_ctrl.gatt";

static inline const nimble_ctrl_cfg_t* cfgp_gatt(void)
{
    nimble_ctrl_ctx_t* ctx = (nimble_ctrl_ctx_t*)nimble_ctrl_ctx_get();
    return &ctx->cfg;
}

const struct ble_gatt_svc_def* nimble_ctrl_app_gatt_svcs_get(const nimble_ctrl_cfg_t* cfg)
{
    if (cfg->get_gatt_svc != NULL) {
        return cfg->get_gatt_svc(cfg->user);
    }
    return NULL;
}

int nimble_ctrl_gatt_init(void)
{

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    ble_svc_gap_init();
#endif
    ble_svc_gatt_init();

    //TODO: Decide if needed for our use case?
    //ble_svc_ans_init();

    const nimble_ctrl_cfg_t* cfg = cfgp_gatt();
    const struct ble_gatt_svc_def* svcs = nimble_ctrl_app_gatt_svcs_get(cfg);
    if (svcs == NULL) {
        ESP_LOGE(TAG_GATT, "No GATT services provided (gatt_svcs_get returned NULL)");
        return -EINVAL;
    }

    int rc = ble_gatts_count_cfg(svcs);
    if (rc != 0) {
        ESP_LOGE(TAG_GATT, "ble_gatts_count_cfg rc=%d", rc);
        return -EIO;
    }

    rc = ble_gatts_add_svcs(svcs);
    if (rc != 0) {
        ESP_LOGE(TAG_GATT, "ble_gatts_add_svcs rc=%d", rc);
        return -EIO;
    }

    return 0;
}

int nimble_ctrl_gatt_notify(uint16_t conn_handle, uint16_t attr_handle, const void* data, size_t len)
{
    /* Build an mbuf containing the payload */
    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (om == NULL) return -ENOMEM;

    /* Notify with explicit payload; avoids "stored value" semantics */
    int rc = ble_gattc_notify_custom(conn_handle, attr_handle, om);

    if (rc == 0) return 0;

    /* ble_gattc_notify_custom consumes om on success; on error it may or may not.
     * NimBLE typically frees on failure too, but being safe: if rc != 0 and om still owned,
     * you’d need os_mbuf_free_chain(om). ESP-IDF NimBLE usually handles it; if you see leaks,
     * add os_mbuf_free_chain(om) here.
     */

    if (rc == BLE_HS_ENOTCONN) return -ENOTCONN;
    if (rc == BLE_HS_EBUSY || rc == BLE_HS_EAGAIN) return -EAGAIN;
    return -EIO;
}

#ifdef __cplusplus
}
#endif
