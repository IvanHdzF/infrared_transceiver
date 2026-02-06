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
    uint16_t attr_handle;  // characteristic value handle OR descriptor handle
    uint8_t  kind;         // 0=CHR, 1=DSC (used for debugging / policy)
    uint8_t  reserved[1];

    nimble_ctrl_gatt_read_cb_t  read_cb;
    nimble_ctrl_gatt_write_cb_t write_cb;

    nimble_ctrl_gatt_storage_t  storage;   // if storage.buf != NULL, provides default read/write
    void* user;
} gatt_db_entry_t;

typedef struct {
    // Keep this in nimble_ctrl context and pass pointer via .arg to access_cb.
    // If you want singleton, you can make one static instance and ignore arg.
    gatt_db_entry_t entries[CONFIG_NIMBLE_CTRL_MAX_GATT_DB_ENTRIES];
    uint16_t        count;
    bool            locked;   // optional: if true, disallow binds after start (policy)
} nimble_ctrl_gatt_db_t;

typedef struct {
    bool inited;
    bool started;

    nimble_ctrl_cfg_t cfg;

    /* Resolved roles */
    nimble_ctrl_role_mask_t roles_effective;

    /* Current connection (optional, for convenience) */
    uint16_t conn_handle; /* BLE_HS_CONN_HANDLE_NONE if none */

    /* Gatt DB */
    nimble_ctrl_gatt_db_t gatt_db;
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

/* GATT DB */
void nimble_ctrl_gatt_db_init(nimble_ctrl_gatt_db_t* db);
void nimble_ctrl_gatt_db_reset(nimble_ctrl_gatt_db_t* db);
void nimble_ctrl_gatt_db_lock(nimble_ctrl_gatt_db_t* db, bool lock);

int nimble_ctrl_gatt_db_bind_chr(uint16_t val_handle,
                                 nimble_ctrl_gatt_read_cb_t read_cb,
                                 nimble_ctrl_gatt_write_cb_t write_cb,
                                 void* user,
                                 nimble_ctrl_gatt_db_t* arg_db);

int nimble_ctrl_gatt_db_bind_dsc(uint16_t dsc_handle,
                                 nimble_ctrl_gatt_read_cb_t read_cb,
                                 nimble_ctrl_gatt_write_cb_t write_cb,
                                 void* user,
                                 nimble_ctrl_gatt_db_t* arg_db);

int nimble_ctrl_gatt_db_bind_storage(uint16_t val_handle,
                                     void* buf,
                                     uint16_t min_len,
                                     uint16_t max_len,
                                     bool notify_on_write,
                                     void* user,
                                     nimble_ctrl_gatt_db_t* arg_db);

void nimble_ctrl_gatt_db_clear_all(nimble_ctrl_gatt_db_t* arg_db);

int nimble_ctrl_gatt_db_access_cb(uint16_t conn_handle,
                                  uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt* ctxt,
                                  nimble_ctrl_gatt_db_t* arg_db);

#if (CONFIG_NIMBLE_CTRL_USE_TEST_HOOKS == y)
int nimble_ctrl_gap_inject_evt(struct ble_gap_event* event, void* arg);
#endif

#ifdef __cplusplus
}
#endif

#endif // NIMBLE_CTRL_PRIV_H
