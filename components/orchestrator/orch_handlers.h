#pragma once

#include <stdint.h>
#include "retrofit_os_types.h"
#include "orch.h"   // for cmd_ctx_t

os_err_t orch_handle_auth_login(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_auth_logout(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_ir_learn_start(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_ir_learn_cancel(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_ir_send(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_sched_set_table(uint32_t op_id, const cmd_ctx_t *ctx);
os_err_t orch_handle_factory_reset(uint32_t op_id, const cmd_ctx_t *ctx);
