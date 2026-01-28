#ifndef STG_MAN_H
#define STG_MAN_H

#include "esp_err.h"

esp_err_t stg_man_init();
esp_err_t stg_man_write();
esp_err_t stg_man_read();

#endif // STG_MAN_H