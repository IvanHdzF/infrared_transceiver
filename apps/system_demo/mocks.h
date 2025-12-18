#ifndef MOCKS_H

#define MOCKS_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "retrofit_os_types.h"   /* EVT_* / payload structs / os_evt_t */

/* Mock component initialization for now, real functions will have the same names minus the "mock_" prefix */

/* Infrastructure */
os_err_t mock_event_bus_init(void);
os_err_t mock_storage_init(void);
os_err_t mock_clock_init(void);

/* Core coordination */
os_err_t mock_errmgr_init(void);
os_err_t mock_orch_init(void);
os_err_t mock_auth_init(void);

/* Communication */
os_err_t mock_ble_init(void);
os_err_t mock_wifi_init(void);

/* Power */
os_err_t mock_power_init(void);

/* Functional services */
os_err_t mock_sched_init(void);
os_err_t mock_ir_init(void);
os_err_t mock_cmd_init(void);


void mock_system_step(uint32_t step);

#ifdef __cplusplus
}
#endif
#endif // MOCKS_H