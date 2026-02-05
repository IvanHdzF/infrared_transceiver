#ifndef NIMBLE_CTRL_PRIV_H
#define NIMBLE_CTRL_PRIV_H

#include "nimble_ctrl/nimble_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 *  Internal types and helpers for nimble_ctrl component
 * ==========================================================================*/

typedef struct {
    bool inited;
    bool started;

    nimble_ctrl_cfg_t cfg;

    /* Resolved roles */
    nimble_ctrl_role_mask_t roles_effective;

    /* Current connection (optional, for convenience) */
    uint16_t conn_handle; /* BLE_HS_CONN_HANDLE_NONE if none */
} nimble_ctrl_ctx_t;

nimble_ctrl_ctx_t* nimble_ctrl_ctx_get(void);

/* GAP */
int nimble_ctrl_gap_init(void);
int nimble_ctrl_gap_start(void);
int nimble_ctrl_gap_stop(void);
int nimble_ctrl_gap_adv_start(void);
int nimble_ctrl_gap_adv_stop(void);

/* GATT */
int nimble_ctrl_gatt_init(void);
int nimble_ctrl_gatt_notify(uint16_t conn_handle, uint16_t attr_handle, const void* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif // NIMBLE_CTRL_PRIV_H
