#ifndef NIMBLE_CTRL_H
#define NIMBLE_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
/* ============================================================================
 *  Constants and Macros
 * ==========================================================================*/
typedef uint8_t nimble_ctrl_role_mask_t;
#define NIMBLE_CTRL_ROLE_PERIPHERAL  (1u << 0)
#define NIMBLE_CTRL_ROLE_CENTRAL     (1u << 1)
#define NIMBLE_CTRL_ROLE_BROADCASTER (1u << 2)
#define NIMBLE_CTRL_ROLE_OBSERVER    (1u << 3)

/* ============================================================================
 *  Types
 * ==========================================================================*/

typedef struct {
    /* Connection lifecycle */
    void (*on_connected)(uint16_t conn_handle, void* user);
    void (*on_disconnected)(uint16_t conn_handle, uint8_t reason, void* user);

    /* Security */
    void (*on_security_changed)(uint16_t conn_handle,
                                uint8_t level,
                                bool bonded,
                                uint8_t key_size,
                                int status,
                                void* user);

    /* GATT-related */
    void (*on_mtu_changed)(uint16_t conn_handle, uint16_t mtu, void* user);
    void (*on_subscribe_changed)(uint16_t conn_handle,
                                 uint16_t attr_handle,
                                 bool notify_enabled,
                                 bool indicate_enabled,
                                 void* user);
} nimble_ctrl_cbs_t;

/*
 * App-provided hook to register GATT services / characteristics.
 * Called during init when in PERIPHERAL role.
 *
 * Return 0 on success, negative on failure.
 */
typedef const struct ble_gatt_svc_def* (*nimble_ctrl_gatt_get_svc_fn)(void* user);

/* Security configuration (applied if enabled) */
typedef struct {
    bool     enable;
    bool     bonding;
    bool     mitm;
    bool     secure_connections;
    uint8_t  io_capabilities;
    uint8_t  key_size_min;
    uint8_t  key_size_max;
} nimble_ctrl_security_cfg_t;

/* Advertising configuration (PERIPHERAL / BROADCASTER) */
typedef struct {
    bool     enable;
    bool     auto_restart_on_disconnect;
    uint16_t interval_min;
    uint16_t interval_max;
} nimble_ctrl_adv_cfg_t;

/* Top-level configuration */
typedef struct {
    nimble_ctrl_role_mask_t roles;   // 0 => use compiled-in defaults (Enable all compiled roles)
    nimble_ctrl_cbs_t cbs;
    void*                user;

    nimble_ctrl_gatt_get_svc_fn get_gatt_svc; /* optional unless PERIPHERAL */

    nimble_ctrl_security_cfg_t security;
    nimble_ctrl_adv_cfg_t      advertising;
} nimble_ctrl_cfg_t;

/* ============================================================================
 *  API
 * ==========================================================================*/

/* Initialize NimBLE controller (idempotent) */
int nimble_ctrl_init(const nimble_ctrl_cfg_t* cfg);

/* Start NimBLE host + selected role */
int nimble_ctrl_start(void);

/* Stop host / advertising (mainly for tests) */
int nimble_ctrl_stop(void);

/* Advertising control (no-op if role unsupported) */
int nimble_ctrl_adv_start(void);
int nimble_ctrl_adv_stop(void);

/*
 * Send notification on an attribute handle.
 * Returns 0 on success, -EAGAIN if backpressure, negative on error.
 */
int nimble_ctrl_notify(uint16_t conn_handle,
                       uint16_t attr_handle,
                       const void* data,
                       size_t len);

#ifdef __cplusplus
}
#endif

#endif // NIMBLE_CTRL_H
