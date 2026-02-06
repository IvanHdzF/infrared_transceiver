// nimble_ctrl_gatt_db.c
//
// Handle-keyed GATT access dispatcher + binding registry (Option A2).
//
// Usage contract for app layer:
//  - In your ble_gatt_svc_def tables, set .access_cb = nimble_ctrl_gatt_access_cb
//    for every characteristic and descriptor you want nimble_ctrl to manage.
//  - After ble_gatts_add_svcs() has run (i.e., once val_handle/descriptor handles are assigned),
//    call nimble_ctrl_gatt_bind_*() with the concrete handles.
//
// NOTE ON HEADERS:
//  - Functions marked PUBLIC should be declared in include/nimble_ctrl/nimble_ctrl.h
//  - Functions marked PRIVATE should be declared in a private header (e.g. src/nimble_ctrl_priv.h)
//    and only used by other .c files inside the component.

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "nimble_ctrl/nimble_ctrl.h"
#include "nimble_ctrl_priv.h"

#include "esp_log.h"
#include "os/os_mbuf.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_gatt.h"

static const char* TAG = "nimble_ctrl.gatt_db";

/* =========================
 * Helpers (PRIVATE)
 * ========================= */

static int entry_cmp_by_handle(const void* pa, const void* pb)
{
    const gatt_db_entry_t* a = (const gatt_db_entry_t*)pa;
    const gatt_db_entry_t* b = (const gatt_db_entry_t*)pb;
    return (a->attr_handle > b->attr_handle) - (a->attr_handle < b->attr_handle);
}

static gatt_db_entry_t* find_entry(nimble_ctrl_gatt_db_t* db, uint16_t attr_handle)
{
    // Binary search in sorted entries[].
    int lo = 0;
    int hi = (int)db->count - 1;

    while (lo <= hi) {
        int mid = lo + ((hi - lo) / 2);
        uint16_t h = db->entries[mid].attr_handle;

        if (h == attr_handle) return &db->entries[mid];
        if (h < attr_handle) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

static bool handle_exists(nimble_ctrl_gatt_db_t* db, uint16_t attr_handle)
{
    return find_entry(db, attr_handle) != NULL;
}

static int storage_read(struct os_mbuf* om, const void* src, uint16_t len)
{
    int rc = os_mbuf_append(om, src, len);
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int storage_write(struct os_mbuf* om, uint16_t min_len, uint16_t max_len, void* dst)
{
    const uint16_t om_len = OS_MBUF_PKTLEN(om);
    if (om_len < min_len || om_len > max_len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint16_t out_len = 0;
    int rc = ble_hs_mbuf_to_flat(om, dst, max_len, &out_len);
    return (rc == 0) ? 0 : BLE_ATT_ERR_UNLIKELY;
}

static int add_entry(nimble_ctrl_gatt_db_t* db,
                     uint16_t attr_handle,
                     uint8_t kind,
                     nimble_ctrl_gatt_read_cb_t read_cb,
                     nimble_ctrl_gatt_write_cb_t write_cb,
                     const nimble_ctrl_gatt_storage_t* storage,
                     void* user)
{
    if (!db || attr_handle == 0) return BLE_ATT_ERR_UNLIKELY;
    if (db->locked) return BLE_ATT_ERR_UNLIKELY;                 // or your own error code
    if (db->count >= (sizeof(db->entries) / sizeof(db->entries[0])))
        return BLE_ATT_ERR_INSUFFICIENT_RES;

    if (handle_exists(db, attr_handle)) {
        // policy: either "replace" or "reject duplicates".
        // For A2 ergonomics, I'd REPLACE existing.
        gatt_db_entry_t* e = find_entry(db, attr_handle);
        e->kind = kind;
        e->read_cb = read_cb;
        e->write_cb = write_cb;
        e->storage = storage ? *storage : (nimble_ctrl_gatt_storage_t) {
            0
        };
        e->user = user;
        return 0;
    }

    gatt_db_entry_t* e = &db->entries[db->count++];
    memset(e, 0, sizeof(*e));
    e->attr_handle = attr_handle;
    e->kind = kind;
    e->read_cb = read_cb;
    e->write_cb = write_cb;
    e->storage = storage ? *storage : (nimble_ctrl_gatt_storage_t) {
        0
    };
    e->user = user;

    // Keep table sorted by handle for binary search.
    // ESP-IDF provides qsort via newlib.
    extern void qsort(void* base, unsigned int nmemb, unsigned int size,
                      int (*compar)(const void*, const void*));
    qsort(db->entries, db->count, sizeof(db->entries[0]), entry_cmp_by_handle);

    return 0;
}

/* =========================
 * Lifecycle (PRIVATE) - declare in private header
 * ========================= */

void nimble_ctrl_gatt_db_init(nimble_ctrl_gatt_db_t* db)
{
    if (!db) return;
    memset(db, 0, sizeof(*db));
}

void nimble_ctrl_gatt_db_reset(nimble_ctrl_gatt_db_t* db)
{
    if (!db) return;
    db->count = 0;
    db->locked = false;
    memset(db->entries, 0, sizeof(db->entries));
}

void nimble_ctrl_gatt_db_lock(nimble_ctrl_gatt_db_t* db, bool lock)
{
    if (!db) return;
    db->locked = lock;
}

/* =========================
 * Binding API (PUBLIC)
 * ========================= */

int nimble_ctrl_gatt_db_bind_chr(uint16_t val_handle,
                                 nimble_ctrl_gatt_read_cb_t read_cb,
                                 nimble_ctrl_gatt_write_cb_t write_cb,
                                 void* user,
                                 nimble_ctrl_gatt_db_t* arg_db)
{
    nimble_ctrl_gatt_db_t* db = arg_db;
    return add_entry(db, val_handle, 0, read_cb, write_cb, NULL, user);
}

int nimble_ctrl_gatt_db_bind_dsc(uint16_t dsc_handle,
                                 nimble_ctrl_gatt_read_cb_t read_cb,
                                 nimble_ctrl_gatt_write_cb_t write_cb,
                                 void* user,
                                 nimble_ctrl_gatt_db_t* arg_db)
{
    nimble_ctrl_gatt_db_t* db = arg_db;
    return add_entry(db, dsc_handle, 1, read_cb, write_cb, NULL, user);
}

int nimble_ctrl_gatt_db_bind_storage(uint16_t val_handle,
                                     void* buf,
                                     uint16_t min_len,
                                     uint16_t max_len,
                                     bool notify_on_write,
                                     void* user,
                                     nimble_ctrl_gatt_db_t* arg_db)
{
    nimble_ctrl_gatt_db_t* db = arg_db;

    nimble_ctrl_gatt_storage_t st = {
        .buf = buf,
        .min_len = min_len,
        .max_len = max_len,
        .notify_on_write = notify_on_write,
    };
    ESP_LOGI(TAG, "Binding storage for handle 0x%04X (buf=%p, min=%u, max=%u, notify_on_write=%d)",
             val_handle, buf, min_len, max_len, notify_on_write ? 1 : 0);
    return add_entry(db, val_handle, 0, NULL, NULL, &st, user);
}

void nimble_ctrl_gatt_db_clear_all(nimble_ctrl_gatt_db_t* arg_db)
{
    nimble_ctrl_gatt_db_t* db = arg_db;
    nimble_ctrl_gatt_db_reset(db);
}

/* =========================
 * Access dispatcher (PUBLIC) - declare in public header
 * ========================= */

int nimble_ctrl_gatt_db_access_cb(uint16_t conn_handle,
                                  uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt* ctxt,
                                  nimble_ctrl_gatt_db_t* arg_db)
{
    nimble_ctrl_gatt_db_t* db = arg_db;
    if (!db || !ctxt) return BLE_ATT_ERR_UNLIKELY;

    gatt_db_entry_t* e = find_entry(db, attr_handle);
    if (!e) {
        // Policy choice:
        // - return READ/WRITE not permitted (cleaner for clients)
        // - or UNLIKELY (signals internal mapping miss)
        switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
        case BLE_GATT_ACCESS_OP_READ_DSC:
            return BLE_ATT_ERR_READ_NOT_PERMITTED;
        case BLE_GATT_ACCESS_OP_WRITE_CHR:
        case BLE_GATT_ACCESS_OP_WRITE_DSC:
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        default:
            return BLE_ATT_ERR_UNLIKELY;
        }
    }

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
    case BLE_GATT_ACCESS_OP_READ_DSC:
        if (e->read_cb) {
            return e->read_cb(conn_handle, attr_handle, ctxt, e->user);
        }
        if (e->storage.buf && e->storage.max_len) {
            return storage_read(ctxt->om, e->storage.buf, e->storage.max_len);
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;

    case BLE_GATT_ACCESS_OP_WRITE_CHR:
    case BLE_GATT_ACCESS_OP_WRITE_DSC:
        if (e->write_cb) {
            return e->write_cb(conn_handle, attr_handle, ctxt, e->user);
        }
        if (e->storage.buf) {
            int rc = storage_write(ctxt->om, e->storage.min_len, e->storage.max_len, e->storage.buf);
            if (rc == 0 && e->storage.notify_on_write && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
                ble_gatts_chr_updated(attr_handle);
            }
            return rc;
        }
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}
