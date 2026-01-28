#include "unity.h"

#include "stg_man/stg_man.h"
#include "retrofit_os_types.h"
#include "freertos/FreeRTOS.h"

#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------- App entry ---------------- */

void app_main(void)
{
    stg_man_init();
}
