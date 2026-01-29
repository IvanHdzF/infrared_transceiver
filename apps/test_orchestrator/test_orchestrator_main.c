#include "unity.h"

#include "orch.h"
#include "evt_bus/evt_bus.h"
#include "retrofit_os_types.h"
#include "freertos/FreeRTOS.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

// ---- mock handler globals (from orch_handlers_mock.c) ----
extern volatile uint32_t g_orch_mock_calls_auth_login;
extern volatile uint32_t g_orch_mock_calls_ir_send;
extern volatile uint32_t g_orch_mock_last_op_id;
extern volatile cmd_id_t g_orch_mock_last_cmd_id;

/* Helpers */
static void wait_orch_state(orch_state_t target)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (orch__test_state_get() != target) {
        TEST_ASSERT_TRUE_MESSAGE(xTaskGetTickCount() < deadline, "orch state not applied");
        vTaskDelay(1);
    }
}

// ----------------------------------------------------------

static void reset_mocks(void)
{
    g_orch_mock_calls_auth_login = 0;
    g_orch_mock_calls_ir_send = 0;
    g_orch_mock_last_op_id = 0;
    g_orch_mock_last_cmd_id = (cmd_id_t)0;
}

/* ---------------- Tests ---------------- */

TEST_CASE("orch: state change event gates commands", "[orch]")
{
    reset_mocks();
    evt_bus_init();
    orch_init();

    // Start LOCKED
    evt_orch_state_changed_t lock_evt = {
        .old_state = ORCH_BOOT,
        .new_state = ORCH_LOCKED,
        .reason = 0,
        .op_id = 0,
    };

    bool res = evt_bus_publish(EVT_ORCH_STATE_CHANGED, &lock_evt, sizeof(lock_evt));
    TEST_ASSERT_TRUE(res);

    /* Wait for dispatch event */
    wait_orch_state(ORCH_LOCKED);
    cmd_ctx_t bad_cmd = {
        .cmd_id = CMD_IR_SEND,
        .client_id = 1,
        .payload = NULL,
        .payload_len = 0,
    };

    os_err_t r = orch_process_req(&bad_cmd);
    TEST_ASSERT_EQUAL(OS_ESTATE, r);
    TEST_ASSERT_EQUAL(0u, g_orch_mock_calls_ir_send);

    // Unlock
    evt_orch_state_changed_t unlock_evt = {
        .old_state = ORCH_LOCKED,
        .new_state = ORCH_NORMAL,
        .reason = 0,
        .op_id = 0,
    };

    evt_bus_publish(EVT_ORCH_STATE_CHANGED, &unlock_evt, sizeof(unlock_evt));
    /* Wait for dispatch event */
    wait_orch_state(ORCH_NORMAL);

    r = orch_process_req(&bad_cmd);
    TEST_ASSERT_EQUAL(OS_OK, r);
    TEST_ASSERT_EQUAL(1u, g_orch_mock_calls_ir_send);
    TEST_ASSERT_NOT_EQUAL(0u, g_orch_mock_last_op_id);
}

TEST_CASE("orch: login allowed while locked", "[orch]")
{
    reset_mocks();
    evt_bus_init();
    orch_init();

    evt_orch_state_changed_t lock_evt = {
        .old_state = ORCH_BOOT,
        .new_state = ORCH_LOCKED,
        .reason = 0,
        .op_id = 0,
    };
    evt_bus_publish(EVT_ORCH_STATE_CHANGED, &lock_evt, sizeof(lock_evt));
    /* Wait for dispatch event */
    wait_orch_state(ORCH_LOCKED);

    cmd_ctx_t login = {
        .cmd_id = CMD_AUTH_LOGIN,
        .client_id = 3,
        .payload = "pw",
        .payload_len = 2,
    };

    os_err_t r = orch_process_req(&login);
    TEST_ASSERT_EQUAL(OS_OK, r);
    TEST_ASSERT_EQUAL(1u, g_orch_mock_calls_auth_login);
    TEST_ASSERT_EQUAL(CMD_AUTH_LOGIN, g_orch_mock_last_cmd_id);
}

/* ---------------- App entry ---------------- */

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
