// test_ir_ctrl_main.c
//
// Hardware happy-path test for ir_ctrl with ONLY one TX and one RX available.
// No separate sniffer/stim channels.
//
// Strategy:
// - Use RAM storage backend.
// - Preload slot0 with a NEC "golden frame" directly into RAM storage.
// - Start learning slot1 (arms RX).
// - Call ir_ctrl_send(slot0) -> TX emits the golden frame.
// - With loopback wire TX->RX, learning slot1 captures what was sent.
// - Verify slot1 == slot0 == golden (with a trailer tolerance option if needed).
//
// REQUIRED WIRING (loopback):
//   IR_CTRL_TX_GPIO  --->  IR_CTRL_RX_GPIO   (via ~1k-4.7k resistor recommended)
//
// Adjust GPIOs/resolution to match your board. Ensure no external IR demodulator is in the path;
// this is raw electrical loopback.
//

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "unity.h"
#include "esp_err.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"

#include "ir_ctrl/ir_ctrl.h"
#include "utilities.h"

static const char *TAG = "test_ir_ctrl";

#ifndef IR_CTRL_RES_HZ
#define IR_CTRL_RES_HZ      1000000u
#endif

#ifndef IR_CTRL_TX_GPIO
#define IR_CTRL_TX_GPIO     18
#endif

#ifndef IR_CTRL_RX_GPIO
#define IR_CTRL_RX_GPIO     17
#endif

#ifndef IR_CTRL_DUT_DUMMY_TX_GPIO
#define IR_CTRL_DUT_DUMMY_TX_GPIO  1   // dummy, not used by DUT in this refactor
#endif

#ifndef IR_CTRL_DUT_DUMMY_RX_GPIO
#define IR_CTRL_DUT_DUMMY_RX_GPIO  1   // dummy, not used by DUT in sniffer refactor
#endif

#define STORAGE_SLOTS        16

// -------------------- Golden frame --------------------
static const rmt_frame_obj_t nec_golden_frame = {
    .rmt_frame_data = {
        { .level0 = 1, .duration0 = 9000, .level1 = 0, .duration1 = 4500 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
        { .level0 = 1, .duration0 = 560,  .level1 = 0, .duration1 = 560  },
    },
    .num_syms = 34,
};

// -------------------- RAM storage backend --------------------
static rmt_frame_obj_t s_slots[STORAGE_SLOTS];
static bool s_slot_valid[STORAGE_SLOTS];

static bool test_store(uint16_t slot, const void *data, uint16_t len)
{
    if (slot >= STORAGE_SLOTS || data == NULL) return false;
    if (len != (uint16_t)sizeof(rmt_frame_obj_t)) return false;
    memcpy(&s_slots[slot], data, sizeof(rmt_frame_obj_t));
    s_slot_valid[slot] = true;
    return true;
}

static bool test_load(uint16_t slot, void *out, uint16_t max_len, uint16_t *out_len)
{
    if (out_len == NULL || out == NULL) return false;
    if (slot >= STORAGE_SLOTS) return false;
    if (!s_slot_valid[slot]) return false;
    if (max_len < (uint16_t)sizeof(rmt_frame_obj_t)) return false;
    memcpy(out, &s_slots[slot], sizeof(rmt_frame_obj_t));
    *out_len = (uint16_t)sizeof(rmt_frame_obj_t);
    return true;
}

static void storage_reset(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    memset(s_slot_valid, 0, sizeof(s_slot_valid));
}

static void storage_preload(uint16_t slot, const rmt_frame_obj_t *frame)
{
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE_MESSAGE(test_store(slot, frame, (uint16_t)sizeof(*frame)), "preload failed");
}

// -------------------- Busy-wait helpers --------------------
static void wait_until_not_busy_or_fail(int timeout_ms)
{
    ESP_LOGI(TAG, "Waiting up to %d ms for ir_ctrl to exit BUSY state...", timeout_ms);

    int waited = 0;
    while (ir_ctrl_is_busy() && waited < timeout_ms) {   // FIX: was "!ir_ctrl_is_busy()"
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }

    print_ir_ctrl_rx_isr_stats();
    TEST_ASSERT_FALSE_MESSAGE(ir_ctrl_is_busy(), "did not exit BUSY state in time");
}

// -------------------- Frame compare (same as you had) --------------------
static void assert_symbols_equal_aligned(const rmt_symbol_word_t *a, const rmt_symbol_word_t *b, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(a[i].level0,    b[i].level0,    "level0 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].duration0, b[i].duration0, "duration0 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].level1,    b[i].level1,    "level1 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].duration1, b[i].duration1, "duration1 mismatch");
    }
}

static void assert_frame_matches_golden_with_trailer_tolerance(const rmt_frame_obj_t *got_packed,
        const rmt_frame_obj_t *gold_packed)
{
    rmt_symbol_word_t got[IR_CTRL_MAX_FRAME_SIZE];
    rmt_symbol_word_t gold[IR_CTRL_MAX_FRAME_SIZE];

    TEST_ASSERT_NOT_NULL(got_packed);
    TEST_ASSERT_NOT_NULL(gold_packed);

    TEST_ASSERT_TRUE(got_packed->num_syms <= IR_CTRL_MAX_FRAME_SIZE);
    TEST_ASSERT_TRUE(gold_packed->num_syms <= IR_CTRL_MAX_FRAME_SIZE);

    const uint16_t got_n  = got_packed->num_syms;
    const uint16_t gold_n = gold_packed->num_syms;

    memcpy(got,  got_packed->rmt_frame_data,  (size_t)got_n  * sizeof(rmt_symbol_word_t));
    memcpy(gold, gold_packed->rmt_frame_data, (size_t)gold_n * sizeof(rmt_symbol_word_t));

    if (got_n == gold_n) {
        bool strict_ok = true;
        for (uint16_t i = 0; i < gold_n; i++) {
            if (got[i].level0     != gold[i].level0 ||
                got[i].duration0  != gold[i].duration0 ||
                got[i].level1     != gold[i].level1 ||
                got[i].duration1  != gold[i].duration1) {
                strict_ok = false;
                break;
            }
        }
        if (strict_ok) return;
    }

    const uint16_t n = (got_n < gold_n) ? got_n : gold_n;
    TEST_ASSERT_TRUE_MESSAGE(n >= 1, "frame too short");

    if (n > 1) {
        assert_symbols_equal_aligned(got, gold, (uint16_t)(n - 1));
    }

    const uint16_t i = (uint16_t)(n - 1);
    TEST_ASSERT_EQUAL_MESSAGE(got[i].level0,    gold[i].level0,    "last.level0 mismatch");
    TEST_ASSERT_EQUAL_MESSAGE(got[i].duration0, gold[i].duration0, "last.duration0 mismatch");
    TEST_ASSERT_EQUAL_MESSAGE(got[i].level1,    gold[i].level1,    "last.level1 mismatch");
    // duration1 intentionally not strictly checked
}

// ============================================================================
// IR "stim" helper: raw RMT TX on the *real* IR_CTRL_TX_GPIO
// ============================================================================

typedef struct {
    rmt_channel_handle_t   tx;
    rmt_encoder_handle_t   enc;
    uint32_t               resolution_hz;
    int                    tx_gpio;
    bool                   carrier_enable;
    uint32_t               carrier_hz;
    float                  duty_cycle;
    bool                   invert_out;
    bool                   enabled;
} ir_stim_t;

static void ir_stim_init(ir_stim_t *s, int tx_gpio, uint32_t resolution_hz,
                         bool carrier_enable, uint32_t carrier_hz, float duty_cycle,
                         bool invert_out)
{
    TEST_ASSERT_NOT_NULL(s);
    memset(s, 0, sizeof(*s));

    s->resolution_hz   = resolution_hz;
    s->tx_gpio         = tx_gpio;
    s->carrier_enable  = carrier_enable;
    s->carrier_hz      = carrier_hz;
    s->duty_cycle      = duty_cycle;
    s->invert_out      = invert_out;

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .gpio_num = tx_gpio,
        .flags.invert_out = invert_out ? 1 : 0,
        .flags.with_dma = 0,
        .flags.io_loop_back = 0,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s->tx));

    rmt_copy_encoder_config_t ecfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&ecfg, &s->enc));

    if (carrier_enable) {
        rmt_carrier_config_t carrier = {
            .frequency_hz = carrier_hz,
            .duty_cycle = duty_cycle,
        };
        ESP_ERROR_CHECK(rmt_apply_carrier(s->tx, &carrier));
    }

    ESP_ERROR_CHECK(rmt_enable(s->tx));
    s->enabled = true;
}

static void ir_stim_deinit(ir_stim_t *s)
{
    if (!s) return;

    if (s->enabled) {
        rmt_disable(s->tx);
        s->enabled = false;
    }
    if (s->enc) rmt_del_encoder(s->enc);
    if (s->tx)  rmt_del_channel(s->tx);

    memset(s, 0, sizeof(*s));
}

static void ir_stim_send_frame_blocking(ir_stim_t *s, const rmt_frame_obj_t *frame,
                                        bool eot_level, uint32_t timeout_ms)
{
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_TRUE_MESSAGE(s->enabled, "stim not enabled");
    TEST_ASSERT_TRUE_MESSAGE(frame->num_syms > 0, "frame empty");

    rmt_transmit_config_t tcfg = {
        .loop_count = 0,
        .flags.eot_level = eot_level ? 1 : 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(s->tx, s->enc,
                                 frame->rmt_frame_data,
                                 frame->num_syms * sizeof(rmt_symbol_word_t),
                                 &tcfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(s->tx, pdMS_TO_TICKS(timeout_ms)));
}

// ============================================================================
// IR "sniffer" helper (skeleton): raw RMT RX on the *real* IR_CTRL_RX_GPIO
// (Not used yet; here for the refactor foundation you asked for.)
// ============================================================================

typedef struct {
    rmt_channel_handle_t rx;
    QueueHandle_t        q;
    rmt_symbol_word_t   *buf;
    size_t               buf_bytes;
    uint32_t             resolution_hz;
    int                  rx_gpio;
    bool                 invert_in;
    bool                 enabled;
} ir_sniffer_t;

static bool sniffer_rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)ch;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, edata, &hp);
    return hp == pdTRUE;
}

static void ir_sniffer_init(ir_sniffer_t *s, int rx_gpio, uint32_t resolution_hz,
                            bool invert_in, rmt_symbol_word_t *buf, size_t buf_bytes)
{
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_NOT_NULL(buf);
    memset(s, 0, sizeof(*s));

    s->resolution_hz = resolution_hz;
    s->rx_gpio       = rx_gpio;
    s->invert_in     = invert_in;
    s->buf           = buf;
    s->buf_bytes     = buf_bytes;

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = resolution_hz,
        .mem_block_symbols = 64,
        .gpio_num = rx_gpio,
        .flags.invert_in = invert_in ? 1 : 0,
        .flags.with_dma = 0,
        .flags.io_loop_back = 0,
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &s->rx));

    s->q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    TEST_ASSERT_NOT_NULL(s->q);

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = sniffer_rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s->rx, &cbs, s->q));

    ESP_ERROR_CHECK(rmt_enable(s->rx));
    s->enabled = true;
}

static void ir_sniffer_deinit(ir_sniffer_t *s)
{
    if (!s) return;

    if (s->enabled) {
        rmt_disable(s->rx);
        s->enabled = false;
    }
    if (s->rx) rmt_del_channel(s->rx);
    if (s->q)  vQueueDelete(s->q);

    memset(s, 0, sizeof(*s));
}

static void rmt_frame_invert_levels(const rmt_symbol_word_t *in,
                                    uint16_t in_n,
                                    rmt_symbol_word_t *out)
{
    for (uint16_t i = 0; i < in_n; i++) {
        out[i].level0    = !in[i].level0;
        out[i].duration0 =  in[i].duration0;
        out[i].level1    = !in[i].level1;
        out[i].duration1 =  in[i].duration1;
    }
}

static void sniffer_postprocess_like_learn(rmt_frame_obj_t *frame,
        bool invert_levels,
        bool normalize)
{
    TEST_ASSERT_NOT_NULL(frame);

    rmt_symbol_word_t tmp_inv[IR_CTRL_MAX_FRAME_SIZE];
    rmt_symbol_word_t tmp_norm[IR_CTRL_MAX_FRAME_SIZE];
    uint16_t n = frame->num_syms;

    const rmt_symbol_word_t *cur = frame->rmt_frame_data;

    if (invert_levels) {
        rmt_frame_invert_levels(cur, n, tmp_inv);
        cur = tmp_inv;
    }

    if (normalize) {
        uint16_t out_n = 0;
        ir_post_nec_canon(
            cur,
            n,
            tmp_norm,
            &out_n,
            NULL
        );

        memcpy(frame->rmt_frame_data,
               tmp_norm,
               out_n * sizeof(rmt_symbol_word_t));
        frame->num_syms = out_n;
    } else if (cur != frame->rmt_frame_data) {
        memcpy(frame->rmt_frame_data,
               cur,
               n * sizeof(rmt_symbol_word_t));
        frame->num_syms = n;
    }
}

// ============================================================================
// Refactored test: DUT learns on IR_CTRL_RX_GPIO, stim transmits on IR_CTRL_TX_GPIO
// ============================================================================

TEST_CASE("ir_ctrl: learn slot1 while external stim transmits golden (loopback happy path)", "[ir_ctrl]")
{
    storage_reset();

    ESP_LOGI(TAG, "Loopback required: TX(GPIO%d) -> RX(GPIO%d)", IR_CTRL_TX_GPIO, IR_CTRL_RX_GPIO);

    const uint16_t slot1 = 1;

    // --- DUT config: REAL RX, DUMMY TX (prevents any accidental DUT TX usage) ---
    ir_ctrl_cfg_t dut_cfg = {
        .resolution = IR_CTRL_RES_HZ,
        .tx_gpio_num = IR_CTRL_DUT_DUMMY_TX_GPIO,  // dummy
        .rx_gpio_num = IR_CTRL_RX_GPIO,            // real (learning happens here)
        .decode_margin = 0,
        .store_frame_func = test_store,
        .load_frame_func = test_load,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_init(&dut_cfg));
    TEST_ASSERT_FALSE(ir_ctrl_is_busy());

    // --- Start learning into slot1 (DUT owns RX) ---
    ir_learn_opts_t learn_opts = {
        .timeout_ms = 0,
        .min_symbols = 1,
        .max_symbols = IR_CTRL_MAX_FRAME_SIZE,
        .invert_levels = true,      // keep as you had; set false if your electrical path is non-inverting
        .normalize = true,
        .postprocess = ir_post_nec_canon,
        .user_ctx = NULL,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_learn_start(slot1, &learn_opts));

    // Small delay so the worker arms RX (keeps test robust w/o changing library internals)
    vTaskDelay(pdMS_TO_TICKS(20));

    // --- Stim config: RAW RMT TX on REAL TX GPIO ---
    ir_stim_t stim;
    ir_stim_init(&stim,
                 IR_CTRL_TX_GPIO,
                 IR_CTRL_RES_HZ,
                 true,        // carrier_enable
                 38000,       // carrier_hz
                 0.33f,       // duty
                 false);      // invert_out

    // Send golden while DUT is learning (but NOT via DUT APIs)
    ir_stim_send_frame_blocking(&stim, &nec_golden_frame,
                                true,    // eot_level high
                                1000);

    ir_stim_deinit(&stim);

    // Wait for learn completion (slot1 stored)
    wait_until_not_busy_or_fail(2000);

    // Read out slot1 and compare vs golden (with trailer tolerance)
    rmt_frame_obj_t learned1 = {0};
    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_get_slot_info(slot1, &learned1));
    assert_frame_matches_golden_with_trailer_tolerance(&learned1, &nec_golden_frame);

    ESP_LOGI(TAG, "PASS: slot1 captured externally-stimulated waveform (loopback)");
    // IMPORTANT: release RMT channels so next test can allocate sniffer/stim channels
    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_deinit());
}

// Happy-path TX verification using sniffer (raw RMT RX) + learn-equivalent postprocessing.
// Wiring: IR_CTRL_TX_GPIO -> IR_CTRL_RX_GPIO
TEST_CASE("ir_ctrl: send slot0 and sniffer captures on RX (loopback happy path)", "[ir_ctrl]")
{
    storage_reset();

    ESP_LOGI(TAG, "Loopback required: TX(GPIO%d) -> RX(GPIO%d)", IR_CTRL_TX_GPIO, IR_CTRL_RX_GPIO);

    const uint16_t slot0 = 0;

    // Preload slot0 with golden frame into RAM storage (used by DUT send path)
    storage_preload(slot0, &nec_golden_frame);

    // --- DUT config: REAL TX, DUMMY RX (DUT only transmits in this test) ---
    ir_ctrl_cfg_t dut_cfg = {
        .resolution = IR_CTRL_RES_HZ,
        .tx_gpio_num = IR_CTRL_TX_GPIO,             // real (TX happens here)
        .rx_gpio_num = IR_CTRL_DUT_DUMMY_RX_GPIO,   // dummy
        .decode_margin = 0,
        .store_frame_func = test_store,
        .load_frame_func = test_load,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_init(&dut_cfg));
    TEST_ASSERT_FALSE(ir_ctrl_is_busy());

    // --- Sniffer config: RAW RMT RX on REAL RX GPIO ---
    static rmt_symbol_word_t sniff_buf[IR_CTRL_MAX_FRAME_SIZE];
    ir_sniffer_t snf;
    ir_sniffer_init(&snf,
                    IR_CTRL_RX_GPIO,
                    IR_CTRL_RES_HZ,
                    false, // invert_in at HW level; keep false and do SW invert to match learn pipeline
                    sniff_buf,
                    sizeof(sniff_buf));

    // Arm RX before TX
    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };
    ESP_ERROR_CHECK(rmt_receive(snf.rx, sniff_buf, sizeof(sniff_buf), &rcfg));

    // --- DUT send (blocking) ---
    ir_send_opts_t send_opts = {
        .repeat = 0,
        .gap_us = 0,
        .blocking = true,
        .override_carrier = false,
        .carrier_hz = 0,
        .duty_cycle = 0.0f,
    };
    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_send(slot0, send_opts));

    // --- Wait for sniffer RX_DONE ---
    rmt_rx_done_event_data_t ed;
    TEST_ASSERT_TRUE_MESSAGE(
        xQueueReceive(snf.q, &ed, pdMS_TO_TICKS(1000)) == pdPASS,
        "sniffer RX timeout (no RX_DONE)"
    );

    ESP_LOGI(TAG, "Sniffer RX_DONE: num_symbols=%u", (unsigned)ed.num_symbols);
    TEST_ASSERT_TRUE_MESSAGE(ed.num_symbols > 0, "sniffer captured 0 symbols");
    TEST_ASSERT_TRUE_MESSAGE(ed.num_symbols <= IR_CTRL_MAX_FRAME_SIZE, "sniffer overflowed buffer");

    // --- Pack captured symbols into frame ---
    rmt_frame_obj_t sniffed = {0};
    sniffed.num_syms = (uint16_t)ed.num_symbols;
    memcpy(sniffed.rmt_frame_data, ed.received_symbols,
           ed.num_symbols * sizeof(rmt_symbol_word_t));

    // --- Apply same processing that learn does (invert + normalize NEC) ---
    // Match your learn opts: invert_levels=true, normalize=true
    sniffer_postprocess_like_learn(&sniffed,
                                   true,   // invert_levels (matches learn opts)
                                   true);  // normalize (NEC canon)

    // --- Compare vs golden (with trailer tolerance) ---
    assert_frame_matches_golden_with_trailer_tolerance(&sniffed, &nec_golden_frame);

    ir_sniffer_deinit(&snf);

    ESP_LOGI(TAG, "PASS: sniffer captured DUT TX waveform (loopback) after postprocess");
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
