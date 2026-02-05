/* ============================================================================
 * components/nimble_ctrl/src/nimble_ctrl_gap.c
 * ============================================================================
 */
#include "nimble_ctrl/nimble_ctrl.h"
#include "nimble_ctrl_priv.h"

#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include "esp_log.h"
#include "nimble/ble.h"

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#ifdef __cplusplus
extern "C" {
#endif

static const char* TAG_GAP = "nimble_ctrl.gap";

#if CONFIG_EXAMPLE_RANDOM_ADDR
static uint8_t s_own_addr_type = BLE_OWN_ADDR_RANDOM;
#else
static uint8_t s_own_addr_type;
#endif

/* Minimal subscription cache (optional) */
#ifndef NIMBLE_CTRL_MAX_SUBS
#define NIMBLE_CTRL_MAX_SUBS 8
#endif

typedef struct {
    uint16_t conn_handle;
    uint16_t attr_handle;
    bool notify_enabled;
    bool indicate_enabled;
} sub_entry_t;

static struct {
    bool adv_active;
    sub_entry_t subs[NIMBLE_CTRL_MAX_SUBS];
} s_gap;

/* Helpers to access cfg safely */
static inline const nimble_ctrl_cfg_t* cfgp(void)
{
    extern nimble_ctrl_cfg_t* nimble_ctrl_cfg_ptr(void); /* not provided; see note below */
    (void)nimble_ctrl_cfg_ptr;

    nimble_ctrl_ctx_t* ctx = (nimble_ctrl_ctx_t*)nimble_ctrl_ctx_get();
    return &ctx->cfg;
}

static inline void set_conn_handle(uint16_t h)
{
    nimble_ctrl_ctx_t* ctx = (nimble_ctrl_ctx_t*)nimble_ctrl_ctx_get();
    ctx->conn_handle = h;
}

static void subs_update(uint16_t conn, uint16_t attr, bool n, bool i)
{
    /* Replace existing or insert new */
    for (int k = 0; k < NIMBLE_CTRL_MAX_SUBS; k++) {
        if (s_gap.subs[k].conn_handle == conn && s_gap.subs[k].attr_handle == attr) {
            s_gap.subs[k].notify_enabled = n;
            s_gap.subs[k].indicate_enabled = i;
            return;
        }
    }
    for (int k = 0; k < NIMBLE_CTRL_MAX_SUBS; k++) {
        if (s_gap.subs[k].conn_handle == 0 && s_gap.subs[k].attr_handle == 0) {
            s_gap.subs[k] = (sub_entry_t) {
                .conn_handle = conn,
                .attr_handle = attr,
                .notify_enabled = n,
                .indicate_enabled = i,
            };
            return;
        }
    }
}

static void subs_clear_conn(uint16_t conn)
{
    for (int k = 0; k < NIMBLE_CTRL_MAX_SUBS; k++) {
        if (s_gap.subs[k].conn_handle == conn) {
            s_gap.subs[k] = (sub_entry_t) {
                0
            };
        }
    }
}

/* GAP event callback (single source of truth) */
static int gap_event(struct ble_gap_event* event, void* arg);

static void on_reset(int reason)
{
    ESP_LOGE(TAG_GAP, "reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc;
    ESP_LOGI(TAG_GAP, "sync; own_addr_type=%d", s_own_addr_type);

    /* Ensure identity address exists; prefer public if present */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG_GAP, "ensure_addr failed rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG_GAP, "infer_auto failed rc=%d", rc);
        return;
    }

    const nimble_ctrl_cfg_t* cfg = cfgp();

    /* Auto-start advertising if enabled */
    if (cfg->advertising.enable) {
        (void)ble_gap_adv_stop(); /* ensure clean start */
        struct ble_hs_adv_fields fields;
        struct ble_gap_adv_params advp;

        memset(&fields, 0, sizeof(fields));
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
        const char* name = ble_svc_gap_device_name();
        if (name) {
            fields.name = (uint8_t*)name;
            fields.name_len = (uint8_t)strlen(name);
            fields.name_is_complete = 1;
        }
#endif

        rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG_GAP, "adv_set_fields rc=%d", rc);
            return;
        }

        memset(&advp, 0, sizeof(advp));
        advp.conn_mode = BLE_GAP_CONN_MODE_UND;
        advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

        if (cfg->advertising.interval_min && cfg->advertising.interval_max) {
            advp.itvl_min = cfg->advertising.interval_min;
            advp.itvl_max = cfg->advertising.interval_max;
        }

        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
        if (rc == 0) {
            s_gap.adv_active = true;
            ESP_LOGI(TAG_GAP, "advertising started");
        } else {
            s_gap.adv_active = false;
            ESP_LOGE(TAG_GAP, "adv_start rc=%d", rc);
        }
    }
}

static void dispatch_connected(uint16_t ch)
{
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (cfg->cbs.on_connected) cfg->cbs.on_connected(ch, cfg->user);
}

static void dispatch_disconnected(uint16_t ch, uint8_t reason)
{
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (cfg->cbs.on_disconnected) cfg->cbs.on_disconnected(ch, reason, cfg->user);
}

static void dispatch_mtu(uint16_t ch, uint16_t mtu)
{
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (cfg->cbs.on_mtu_changed) cfg->cbs.on_mtu_changed(ch, mtu, cfg->user);
}

static void dispatch_subscribe(uint16_t ch, uint16_t attr, bool n, bool i)
{
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (cfg->cbs.on_subscribe_changed) cfg->cbs.on_subscribe_changed(ch, attr, n, i, cfg->user);
}

static void dispatch_security(uint16_t ch, uint8_t level, bool bonded, uint8_t key_size, int status)
{
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (cfg->cbs.on_security_changed) cfg->cbs.on_security_changed(ch, level, bonded, key_size, status, cfg->user);
}

static int gap_event(struct ble_gap_event* event, void* arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        bool is_remote_sup_feat_status = false;
        // Current ESP-IDF Port of NimBLE calls this event as BLE_GAP_EVENT_CONNECT when it should not be called
        // See ble_gap_rx_rd_rem_sup_feat_complete in ble_gap.c for more details
        is_remote_sup_feat_status = (event->connect.status == (int)BLE_ERR_UNSUPP_REM_FEATURE);

        if (event->connect.status == 0 || is_remote_sup_feat_status) {
            set_conn_handle(event->connect.conn_handle);
            s_gap.adv_active = false;
            dispatch_connected(event->connect.conn_handle);
        } else {
            /* connect attempt failed; resume advertising if configured */
            const nimble_ctrl_cfg_t* cfg = cfgp();
            if (cfg->advertising.enable) {
                ESP_LOGW(TAG_GAP, "Connection failed (status: %d); restarting advertising", event->connect.status);
                (void)nimble_ctrl_gap_adv_start();
            }
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        const uint16_t ch = event->disconnect.conn.conn_handle;
        const uint8_t reason = (uint8_t)event->disconnect.reason;

        subs_clear_conn(ch);
        set_conn_handle(BLE_HS_CONN_HANDLE_NONE);

        dispatch_disconnected(ch, reason);

        const nimble_ctrl_cfg_t* cfg = cfgp();
        if (cfg->advertising.enable && cfg->advertising.auto_restart_on_disconnect) {
            (void)nimble_ctrl_gap_adv_start();
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        dispatch_mtu(event->mtu.conn_handle, (uint16_t)event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        const uint16_t ch = event->subscribe.conn_handle;
        const uint16_t ah = event->subscribe.attr_handle;
        const bool n = event->subscribe.cur_notify;
        const bool i = event->subscribe.cur_indicate;

        subs_update(ch, ah, n, i);
        dispatch_subscribe(ch, ah, n, i);
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        /* Map NimBLE sec state to your callback signature */
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
        if (rc == 0) {
            const uint8_t level =
                desc.sec_state.authenticated ? 2 :
                desc.sec_state.encrypted     ? 1 : 0;
            dispatch_security(desc.conn_handle,
                              level,
                              desc.sec_state.bonded != 0,
                              desc.sec_state.key_size,
                              event->enc_change.status);
        } else {
            dispatch_security(event->enc_change.conn_handle, 0, false, 0, event->enc_change.status);
        }
        return 0;
    }

    case BLE_GAP_EVENT_ADV_COMPLETE: {
        /* Advertising stopped; restart if user wants it */
        const nimble_ctrl_cfg_t* cfg = cfgp();
        s_gap.adv_active = false;
        if (cfg->advertising.enable) {
            (void)nimble_ctrl_gap_adv_start();
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* Private API */

int nimble_ctrl_gap_init(void)
{
    memset(&s_gap, 0, sizeof(s_gap));

    /* Hook host callbacks */
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;

    return 0;
}

int nimble_ctrl_gap_start(void)
{
    /* nothing needed; on_sync will run once host is up */
    return 0;
}

int nimble_ctrl_gap_stop(void)
{
    (void)ble_gap_adv_stop();
    s_gap.adv_active = false;
    return 0;
}

int nimble_ctrl_gap_adv_start(void)
{
    ESP_LOGI(TAG_GAP, "Starting advertising");
    const nimble_ctrl_cfg_t* cfg = cfgp();
    if (!cfg->advertising.enable) return 0;

    /* If host isn't synced yet, ble_gap_adv_start will fail; caller can retry. */
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params advp;
    int rc;

    (void)ble_gap_adv_stop();

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

#if CONFIG_BT_NIMBLE_GAP_SERVICE
    const char* name = ble_svc_gap_device_name();
    if (name) {
        fields.name = (uint8_t*)name;
        fields.name_len = (uint8_t)strlen(name);
        fields.name_is_complete = 1;
        ESP_LOGI(TAG_GAP, "Advertising name: %s", name);
    }
#endif

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG_GAP, "adv_set_fields rc=%d", rc);
        return -EIO;
    }

    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (cfg->advertising.interval_min && cfg->advertising.interval_max) {
        advp.itvl_min = cfg->advertising.interval_min;
        advp.itvl_max = cfg->advertising.interval_max;
    }

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    if (rc == 0) {
        s_gap.adv_active = true;
        ESP_LOGI(TAG_GAP, "advertising started");
    } else {
        s_gap.adv_active = false;
        ESP_LOGE(TAG_GAP, "adv_start rc=%d", rc);
    }
    return (rc == 0) ? 0 : -EIO;
}

int nimble_ctrl_gap_adv_stop(void)
{
    (void)ble_gap_adv_stop();
    s_gap.adv_active = false;
    return 0;
}

#ifdef __cplusplus
}
#endif
