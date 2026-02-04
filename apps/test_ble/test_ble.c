#include "unity.h"

#include "nimble_ctrl/nimble_ctrl.h"
#include "freertos/FreeRTOS.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------- App entry ---------------- */

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
