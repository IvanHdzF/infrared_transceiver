#ifndef RETROFIT_ORCH_H
#define RETROFIT_ORCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "retrofit_os_types.h"


void orch_init(void);

os_err_t orch_process_req(const cmd_ctx_t* ctx);

#if defined(CONFIG_ORCH_USE_MOCK_HANDLERS)
orch_state_t orch__test_state_get(void);
void orch__test_state_set(orch_state_t s);
#endif // CONFIG_ORCH_USE_MOCK_HANDLERS

#ifdef __cplusplus
}
#endif

#endif /* RETROFIT_ORCH_H */