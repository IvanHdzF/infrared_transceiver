#include "orch.h"
#include "retrofit_os_types.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Mock handlers (stubs) for tests.
 * Same symbols/signatures as production.
 *
 * Usage pattern:
 * - Your test sets the globals below (expected/return codes)
 * - Calls orch_process_req(...)
 * - Asserts that call counters/op_id/last ctx fields match expectations
 */

volatile uint32_t g_orch_mock_calls_auth_login = 0u;
volatile uint32_t g_orch_mock_calls_auth_logout = 0u;
volatile uint32_t g_orch_mock_calls_ir_learn_start = 0u;
volatile uint32_t g_orch_mock_calls_ir_learn_cancel = 0u;
volatile uint32_t g_orch_mock_calls_ir_send = 0u;
volatile uint32_t g_orch_mock_calls_sched_set_table = 0u;
volatile uint32_t g_orch_mock_calls_factory_reset = 0u;

volatile uint32_t g_orch_mock_last_op_id = 0u;
volatile cmd_id_t g_orch_mock_last_cmd_id = (cmd_id_t)0;

volatile os_err_t g_orch_mock_rc_auth_login = OS_OK;
volatile os_err_t g_orch_mock_rc_auth_logout = OS_OK;
volatile os_err_t g_orch_mock_rc_ir_learn_start = OS_OK;
volatile os_err_t g_orch_mock_rc_ir_learn_cancel = OS_OK;
volatile os_err_t g_orch_mock_rc_ir_send = OS_OK;
volatile os_err_t g_orch_mock_rc_sched_set_table = OS_OK;
volatile os_err_t g_orch_mock_rc_factory_reset = OS_OK;

static inline void mock_record(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_last_op_id = op_id;
    g_orch_mock_last_cmd_id = (ctx != NULL) ? ctx->cmd_id : (cmd_id_t)0;
}

os_err_t orch_handle_auth_login(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_auth_login++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_auth_login;
}

os_err_t orch_handle_auth_logout(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_auth_logout++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_auth_logout;
}

os_err_t orch_handle_ir_learn_start(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_ir_learn_start++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_ir_learn_start;
}

os_err_t orch_handle_ir_learn_cancel(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_ir_learn_cancel++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_ir_learn_cancel;
}

os_err_t orch_handle_ir_send(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_ir_send++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_ir_send;
}

os_err_t orch_handle_sched_set_table(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_sched_set_table++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_sched_set_table;
}

os_err_t orch_handle_factory_reset(uint32_t op_id, const cmd_ctx_t *ctx)
{
    g_orch_mock_calls_factory_reset++;
    mock_record(op_id, ctx);
    return g_orch_mock_rc_factory_reset;
}
