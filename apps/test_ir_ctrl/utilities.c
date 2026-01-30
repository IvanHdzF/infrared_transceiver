#include "utilities.h"

#include <stdlib.h>  // abs
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "driver/rmt_types.h"
#include "ir_ctrl/ir_ctrl.h"

#define NEC_LEAD0_US   9000
#define NEC_LEAD1_US   4500
#define NEC_0_0_US      560
#define NEC_0_1_US      560
#define NEC_1_0_US      560
#define NEC_1_1_US     1690
#define NEC_REP0_US    9000
#define NEC_REP1_US    2250

static const char *TAG = "ir_util";

static inline int iabs_i(int x)
{
    return x < 0 ? -x : x;
}

static inline bool within_u32(uint32_t v, uint32_t ref, uint32_t tol)
{
    int dv = (int)v - (int)ref;
    return (uint32_t)iabs_i(dv) <= tol;
}

static inline uint32_t snap2(uint32_t v, uint32_t a, uint32_t b)
{
    return (iabs_i((int)v - (int)a) <= iabs_i((int)v - (int)b)) ? a : b;
}

// Canonicalize a single NEC symbol by snapping to known bins with tolerances.
static void nec_canon_symbol(rmt_symbol_word_t *s)
{
    const uint32_t d0 = s->duration0;
    const uint32_t d1 = s->duration1;

    // Preserve explicit EOT marker on last symbol (duration1=0) if present
    if (d1 == 0) {
        // Still snap duration0 to a plausible short pulse
        s->duration0 = snap2(d0, NEC_0_0_US, NEC_1_0_US); // both 560 anyway
        return;
    }

    // Leading code (wide tolerance)
    if (within_u32(d0, NEC_LEAD0_US, 1200) && within_u32(d1, NEC_LEAD1_US, 1200)) {
        s->duration0 = NEC_LEAD0_US;
        s->duration1 = NEC_LEAD1_US;
        return;
    }

    // Repeat code (wide tolerance)
    if (within_u32(d0, NEC_REP0_US, 1200) && within_u32(d1, NEC_REP1_US, 800)) {
        s->duration0 = NEC_REP0_US;
        s->duration1 = NEC_REP1_US;
        return;
    }

    // Logic 0 (tight-ish tolerance)
    if (within_u32(d0, NEC_0_0_US, 250) && within_u32(d1, NEC_0_1_US, 250)) {
        s->duration0 = NEC_0_0_US;
        s->duration1 = NEC_0_1_US;
        return;
    }

    // Logic 1 (d1 needs a bit more tolerance)
    if (within_u32(d0, NEC_1_0_US, 250) && within_u32(d1, NEC_1_1_US, 350)) {
        s->duration0 = NEC_1_0_US;
        s->duration1 = NEC_1_1_US;
        return;
    }

    // Fallback: snap each duration independently to nearest short/long bins.
    // (This is more stable than k-means-with-statics and doesn't drift.)
    const uint32_t short_us = 560;
    const uint32_t long_us  = 1690;
    s->duration0 = snap2(d0, short_us, long_us);
    s->duration1 = snap2(d1, short_us, long_us);
}

// Signature matches your opts.postprocess() usage in ir_handle_rx_done()
void ir_post_nec_canon(const rmt_symbol_word_t *in,
                       uint16_t in_n,
                       rmt_symbol_word_t *out,
                       uint16_t *out_n,
                       void *user_ctx)
{
    (void)user_ctx;
    ESP_LOGI(TAG, "ir_post_nec_canon: normalizing %u symbols", (unsigned)in_n);

    if (!out || !out_n) return;
    if (in_n == 0 || in_n > IR_CTRL_MAX_FRAME_SIZE) {
        *out_n = 0;
        return;
    }

    // Copy through
    for (uint16_t i = 0; i < in_n; i++) out[i] = in[i];

    // Canonicalize durations
    for (uint16_t i = 0; i < in_n; i++) nec_canon_symbol(&out[i]);

    // Optional: force EOT on last symbol (comment in if you want the stored frame to always have EOT)
    // out[in_n - 1].duration1 = 0;

    *out_n = in_n;
}
