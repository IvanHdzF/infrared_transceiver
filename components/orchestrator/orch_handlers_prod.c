#include "orch.h"
#include "retrofit_os_types.h"
#include <stddef.h>
#include <stdint.h>

/*
 * Production “barebones” handlers.
 * These are the symbols your dispatch table points to.
 *
 * NOTE: Replace the TODOs with calls into your real modules (auth/ir/ota/etc).
 * Keep these handlers thin: validate ctx/payload, call module API, return os_err_t.
 */

os_err_t orch_handle_auth_login(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }
    if ((ctx->payload == NULL) || (ctx->payload_len == 0u)) {
        return OS_EINVAL;
    }

    // TODO: return auth_login_start(op_id, ctx->payload, ctx->payload_len);
    return OS_ENOTSUP;
}

os_err_t orch_handle_auth_logout(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: return auth_logout_start(op_id);
    return OS_ENOTSUP;
}

os_err_t orch_handle_ir_learn_start(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: return ir_learn_start(op_id, ctx->slot_id);
    return OS_ENOTSUP;
}

os_err_t orch_handle_ir_learn_cancel(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: return ir_learn_cancel(op_id);
    return OS_ENOTSUP;
}

os_err_t orch_handle_ir_send(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: return ir_send(op_id, ctx->slot_id);
    return OS_ENOTSUP;
}

os_err_t orch_handle_sched_set_table(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: validate payload points to schedule table, then call scheduler_set_table(...)
    return OS_ENOTSUP;
}

os_err_t orch_handle_factory_reset(uint32_t op_id, const cmd_ctx_t *ctx)
{
    (void)op_id;

    if (ctx == NULL) {
        return OS_EINVAL;
    }

    // TODO: call factory_reset_start(op_id) or equivalent
    return OS_ENOTSUP;
}
