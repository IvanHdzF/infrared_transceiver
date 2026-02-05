#ifndef RETROFIT_OS_TYPES_H
#define RETROFIT_OS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ==========================================================================
 * Core shared contracts (public, stable)
 * ========================================================================== */

typedef int32_t os_err_t;

typedef enum {
    OS_OK = 0,
    OS_EFAIL    = -1,
    OS_EINVAL   = -2,
    OS_ETIMEOUT = -3,
    OS_ENOMEM   = -4,
    OS_EBUSY    = -5,
    OS_ESTATE   = -6,
    OS_EPERM    = -7,
    OS_ENOTSUP  = -8,
    OS_ECRC     = -9,
    OS_EFULL    = -10,
} os_err_code_t;

/* ==========================================================================
 * Module IDs (for tracing/filtering)
 * ========================================================================== */

typedef uint16_t os_mod_id_t;

typedef enum {
    OS_MOD_NONE = 0,
    OS_MOD_ORCH,
    OS_MOD_AUTH,
    OS_MOD_BLE,
    OS_MOD_WIFI,
    OS_MOD_MQTT,
    OS_MOD_CLOCK,
    OS_MOD_SCHED,
    OS_MOD_IR,
    OS_MOD_STORAGE,
    OS_MOD_POWER,
    OS_MOD_OTA,
    OS_MOD_CMD,
    OS_MOD_MONITOR,
    OS_MOD_MAX
} os_module_id_t;

/* ==========================================================================
 * Global Event IDs (from EVT table)
 * NOTE: Once you ship logs/protocols, treat enum ordering as ABI-stable.
 * ========================================================================== */

typedef uint16_t os_evt_id_t;

typedef enum {
    EVT_NONE = 0,

    /* Auth */
    EVT_ORCH_STATE_CHANGED,

    /* Comms */
    EVT_BLE_CONN_CHANGED,
    EVT_BLE_SEC_CHANGED,
    EVT_WIFI_STATE_CHANGED,
    EVT_MQTT_STATE_CHANGED,

    /* Time */
    EVT_TIME_SYNCED,
    EVT_TIME_JUMPED,

    /* Scheduler */
    EVT_SCHEDULE_TABLE_UPDATED,
    EVT_SCHEDULE_DUE,

    /* IR */
    EVT_IR_LEARN_STARTED,
    EVT_IR_LEARN_RESULT,
    EVT_IR_SLOT_WRITTEN,
    EVT_IR_SEND_STARTED,
    EVT_IR_SEND_RESULT,

    /* Storage */
    EVT_STORAGE_CORRUPT,
    EVT_STORAGE_FULL,
    EVT_FACTORY_RESET_DONE,

    /* Power */
    EVT_POWER_MODE_CHANGED,
    EVT_BATTERY_STATE,

    /* OTA */
    EVT_OTA_AVAILABLE,
    EVT_OTA_START,
    EVT_OTA_PROGRESS,
    EVT_OTA_DONE,

    /* Command */
    EVT_CMD_REJECTED,

    /* Health */
    EVT_WATCHDOG_WARNING,
    EVT_HEALTH_TICK,

    EVT__MAX
} os_event_id_t;

/* ==========================================================================
 * Event Bus Envelope (no pointer payloads; always copied)
 *
 * POLICY:
 * - Delivery: callbacks execute on event-bus task context (serialized, in-order)
 * - Blocking: callbacks MUST NOT block; enqueue heavy work to module workers
 * ========================================================================== */

#ifndef OS_EVT_INLINE_MAX
#define OS_EVT_INLINE_MAX 16u /* tune based on typical payload sizes */
#endif

typedef struct {
    os_evt_id_t id;        /* EVT_* */
    os_mod_id_t src;       /* OS_MOD_* */
    uint32_t    ts_ms;     /* optional: 0 if unknown */
    uint16_t    len;       /* bytes; must be <= OS_EVT_INLINE_MAX */
    uint8_t     payload[OS_EVT_INLINE_MAX]; /* event-specific POD bytes */
} os_evt_t;

/* Callback signature for subscribers (stored alongside user_ctx in bus table) */
typedef void (*os_evt_cb_t)(const os_evt_t *evt, void *user_ctx);

/* Optional: future unsubscribe handle */
typedef struct {
    os_evt_id_t id;
    uint16_t    slot;      /* implementation-defined */
} os_evt_sub_handle_t;

/* ==========================================================================
 * Minimal payload structs (optional in Sprint 0; keep POD and <= INLINE_MAX)
 * ========================================================================== */

typedef enum { OS_LINK_DOWN = 0, OS_LINK_UP = 1 } os_link_state_t;

typedef struct {
    os_link_state_t state;
} evt_ble_conn_changed_t;

typedef struct {
    uint8_t  bonded    : 1;
    uint8_t  encrypted : 1;
    uint8_t  rsvd      : 6;
    uint16_t mtu;
} evt_ble_sec_changed_t;

typedef struct {
    os_link_state_t state;
    uint32_t ip_v4_be; /* 0 if none; network byte order */
} evt_wifi_state_changed_t;

typedef struct {
    int32_t delta_seconds;
} evt_time_jumped_t;
typedef struct {
    uint32_t schedule_id;
} evt_schedule_due_t;

typedef enum { IR_RES_OK = 0, IR_RES_FAIL = 1 } ir_result_t;
typedef struct {
    ir_result_t result;
    uint16_t slot;
} evt_ir_learn_result_t;
typedef struct {
    uint16_t slot;
    uint32_t crc32;
} evt_ir_slot_written_t;
typedef struct {
    ir_result_t result;
} evt_ir_send_result_t;

typedef enum { PWR_ACTIVE = 0, PWR_IDLE = 1, PWR_SLEEP = 2 } os_power_mode_t;
typedef struct {
    os_power_mode_t mode;
} evt_power_mode_changed_t;

typedef enum { CMD_REJ_AUTH = 0, CMD_REJ_STATE = 1, CMD_REJ_PARAM = 2, CMD_REJ_BUSY = 3 } os_cmd_reject_reason_t;
typedef struct {
    os_cmd_reject_reason_t reason;
} evt_cmd_rejected_t;

/* ==========================================================================
 * Optional contracts for "init/process" style modules
 * ========================================================================== */

typedef os_err_t (*os_init_fn_t)(void);
typedef os_err_t (*os_process_fn_t)(const os_evt_t *evt);

// TODO: Maybe move this later into cmd.h?
typedef enum {
    CMD_AUTH_LOGIN = 0,
    CMD_AUTH_LOGOUT,
    CMD_IR_LEARN_START,
    CMD_IR_LEARN_CANCEL,
    CMD_IR_SEND,
    CMD_SCHED_SET_TABLE,
    CMD_FACTORY_RESET,
    CMD_ID_MAX
} cmd_id_t;
typedef struct {
    cmd_id_t  cmd_id;
    uint32_t  client_id;  // opaque (BLE conn handle / WiFi socket index) for op_id routing in Comms
    const void *payload;  // optional pointer for future (schedule table, etc.)
    uint32_t  payload_len;
    uint32_t     flags;   // optional: sync/async, auth-required bypass, etc
} cmd_ctx_t;

typedef enum {
    ORCH_BOOT = 0,
    ORCH_LOCKED,
    ORCH_NORMAL,
} orch_state_t;

typedef enum {
    CMD_OK = 0,
    CMD_ACCEPTED,         // accepted + async completion will arrive via EVT_*_RESULT
    CMD_REJECTED_AUTH,
    CMD_REJECTED_STATE,
    CMD_INVALID_ARG,
    CMD_BUSY,
    CMD_NOT_SUPPORTED,
    CMD_INTERNAL_ERR,
} cmd_status_t;

typedef struct {
    cmd_status_t status;
    uint32_t op_id;       // valid when status==CMD_ACCEPTED (or CMD_OK if you want)
    uint32_t detail;      // optional (err code, etc.)
} cmd_rsp_t;

// Orchestrator state changed event payload (publish on evt_bus)
typedef struct {
    orch_state_t old_state;
    orch_state_t new_state;
    uint32_t reason;
    uint32_t op_id;       // optional
} evt_orch_state_changed_t;

#ifdef __cplusplus
}
#endif

#endif /* RETROFIT_OS_TYPES_H */
