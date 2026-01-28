#include "orch.h"
#include "retrofit_os_types.h"
#include "evt_bus/evt_bus.h"
#include "orch_handlers.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define ORCH_EVT_ID(e) ((evt_id_t)(e))

// ---------------------------- Types ----------------------------

typedef os_err_t (*orch_handler_t)(uint32_t op_id, const cmd_ctx_t *ctx);

/* static variables */
static const orch_handler_t handlers[CMD_ID_MAX] = {
    [CMD_AUTH_LOGIN]      = orch_handle_auth_login,
    [CMD_AUTH_LOGOUT]     = orch_handle_auth_logout,
    [CMD_IR_LEARN_START]  = orch_handle_ir_learn_start,
    [CMD_IR_LEARN_CANCEL] = orch_handle_ir_learn_cancel,
    [CMD_IR_SEND]         = orch_handle_ir_send,
    [CMD_SCHED_SET_TABLE] = orch_handle_sched_set_table,
    [CMD_FACTORY_RESET]   = orch_handle_factory_reset,
};


static _Atomic orch_state_t g_state = ORCH_BOOT;
static _Atomic uint32_t g_next_op_id = 1;

/* Helpers */
static inline void orch_state_update(orch_state_t s)
{
    atomic_store_explicit(&g_state, s, memory_order_relaxed);
}

static inline orch_state_t orch_state_get(void)
{
    return atomic_load_explicit(&g_state, memory_order_relaxed);
}

static inline bool cmd_is_valid_in_locked(const cmd_id_t id)
{
	return (id == CMD_AUTH_LOGIN);
}

static inline uint32_t orch_next_op_id(void)
{
    uint32_t id = atomic_fetch_add_explicit(&g_next_op_id, 1u, memory_order_relaxed);
    if (id == 0) { // wrapped
        id = atomic_fetch_add_explicit(&g_next_op_id, 1u, memory_order_relaxed);
    }
    return id;
}

/* Callbacks */
void on_state_change(const evt_t *evt, void *user_ctx)
{
	(void)user_ctx;
	if (evt == NULL)
	{
		return;
	}

	/* Sanity check */
	if (evt->id != EVT_ORCH_STATE_CHANGED)
	{
		// TODO: Log issue here this is not normal
		return;
	}

	if (evt->len != sizeof(evt_orch_state_changed_t))
	{
		// TODO: Log as a warning
		return;
	}
	evt_orch_state_changed_t p;
	(void)memcpy(&p, evt->payload, sizeof(p));
	orch_state_update(p.new_state);
}

/* API */
void orch_init(void)
{
	// TODO: Set susbcriptions callbacks here
	evt_bus_subscribe(ORCH_EVT_ID(EVT_ORCH_STATE_CHANGED), on_state_change, NULL);
	
}

os_err_t orch_process_req(const cmd_ctx_t* ctx)
{
	if (!ctx || ctx->cmd_id >= CMD_ID_MAX)
	{
		return OS_EINVAL;
	}

	orch_state_t state = orch_state_get();
	bool cmd_is_valid = false;

	/* Validate command */
	switch (state)
	{
		case ORCH_BOOT:
		{
			return OS_EBUSY;
		}

		case ORCH_LOCKED:
		{
			cmd_is_valid = cmd_is_valid_in_locked(ctx->cmd_id);
			break;
		}
		case ORCH_NORMAL:
		{
			cmd_is_valid = true;
			break;
		}
		default:
		{
			break;
		}
	}
	orch_handler_t handler = handlers[ctx->cmd_id];
	if (handler == NULL)
	{
		return OS_ENOTSUP;
	}
	if (!cmd_is_valid)
	{
		return OS_ESTATE;
	}
	uint32_t op_id = orch_next_op_id();
	return handler(op_id, ctx);
}

/* Conditional testing hooks */
#if defined(CONFIG_ORCH_USE_MOCK_HANDLERS)
orch_state_t orch__test_state_get(void)
{
    return orch_state_get();
}
void orch__test_state_set(orch_state_t s)
{
    return orch_state_update(s);
}
#endif // CONFIG_ORCH_USE_MOCK_HANDLERS
