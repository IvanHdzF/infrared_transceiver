#ifndef RETROFIT_ORCH_H
#define RETROFIT_ORCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "retrofit_os_types.h"


os_err_t orchestrator_init(void);

os_err_t orchestrator_process(const os_evt_t *evt);

#ifdef __cplusplus
}
#endif

#endif /* RETROFIT_ORCH_H */