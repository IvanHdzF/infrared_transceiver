#ifndef TEST_IR_CTRL_UTILITIES_H
#define TEST_IR_CTRL_UTILITIES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/rmt_types.h"
#include "ir_ctrl/ir_ctrl.h"
#include <stdint.h>

void ir_post_nec_canon(const rmt_symbol_word_t *in,
                       uint16_t in_n,
                       rmt_symbol_word_t *out,
                       uint16_t *out_n,
                       void *user_ctx);

#ifdef __cplusplus
}
#endif

#endif //TEST_IR_CTRL_UTILITIES_H
