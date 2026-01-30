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

#define STORAGE_SLOTS        16

// -------------------- Golden frame --------------------
static const rmt_frame_obj_t nec_golden_frame = {
    .rmt_frame_data = {
        { .level0 = 1, .duration0 = 9000, .level1 = 0, .duration1 = 4500 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 1690 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
        { .level0 = 1, .duration0 = 560, .level1 = 0, .duration1 = 560 },
    },
    .num_syms = 34,
};

static void print_rmt_frame_as_c_initializer(const rmt_symbol_word_t *syms, size_t n)
{
    printf(".rmt_frame_data = {\n");

    for (size_t i = 0; i < n; i++) {
        printf("    { .level0 = %u, .duration0 = %u, .level1 = %u, .duration1 = %u },\n",
               syms[i].level0,
               syms[i].duration0,
               syms[i].level1,
               syms[i].duration1);
    }

    printf("},\n");
    printf(".num_syms = %u,\n", (unsigned)n);
}

// -------------------- RAM storage backend --------------------
static rmt_frame_obj_t s_slots[STORAGE_SLOTS];
static bool s_slot_valid[STORAGE_SLOTS];

static bool test_store(uint16_t slot, const void *data, uint16_t len)
{
    ESP_LOGI(TAG, "test_store called with slot=%u, len=%u", slot, len);
    if (slot >= STORAGE_SLOTS || data == NULL) {
        TEST_MESSAGE("test_store: invalid arg");
        return false;
    }
    if (len != (uint16_t)sizeof(rmt_frame_obj_t)) {
        TEST_MESSAGE("test_store: invalid size");
        return false;
    }
    memcpy(&s_slots[slot], data, sizeof(rmt_frame_obj_t));
    s_slot_valid[slot] = true;
    ESP_LOGI(TAG, "Stored frame in slot %u:", slot);
    print_rmt_frame_as_c_initializer(s_slots[slot].rmt_frame_data, s_slots[slot].num_syms);
    return true;
}

static bool test_load(uint16_t slot, void *out, uint16_t max_len, uint16_t *out_len)
{
    ESP_LOGI(TAG, "test_load called with slot=%u, max_len=%u", slot, max_len);
    if (out_len == NULL || out == NULL) {
        TEST_MESSAGE("test_load: out_len or out is NULL");
        return false;
    }
    if (slot >= STORAGE_SLOTS) {
        TEST_MESSAGE("test_load: slot out of bounds");
        return false;
    }
    if (!s_slot_valid[slot]) {
        TEST_MESSAGE("test_load: slot not valid");
        return false;
    }
    if (max_len < (uint16_t)sizeof(rmt_frame_obj_t)) {
        TEST_MESSAGE("test_load: max_len too small");
        return false;
    }
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

// -------------------- Helpers --------------------
static void wait_until_busy_or_fail(int timeout_ms)
{
    int waited = 0;
    while (!ir_ctrl_is_busy() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    TEST_ASSERT_TRUE_MESSAGE(ir_ctrl_is_busy(), "did not enter BUSY state in time");
}

static void wait_until_not_busy_or_fail(int timeout_ms)
{
    ESP_LOGI(TAG, "Waiting up to %d ms for ir_ctrl to exit BUSY state...", timeout_ms);
    int waited = 0;
    while (!ir_ctrl_is_busy() && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
    print_ir_ctrl_rx_isr_stats();
    TEST_ASSERT_FALSE_MESSAGE(ir_ctrl_is_busy(), "did not exit BUSY state in time");
}

static void assert_symbols_equal_aligned(const rmt_symbol_word_t *a, const rmt_symbol_word_t *b, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(a[i].level0,    b[i].level0,    "level0 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].duration0, b[i].duration0, "duration0 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].level1,    b[i].level1,    "level1 mismatch");
        TEST_ASSERT_EQUAL_MESSAGE(a[i].duration1, b[i].duration1, "duration1 mismatch");
    }
}

// Some RMT RX paths may yield a non-zero "trailer duration1" or omit it.
// For happy-path stability, compare either:
// - full length if it matches, else
// - first (n-1) symbols and only check last symbol's level0/duration0/level1.
static void assert_frame_matches_golden_with_trailer_tolerance(const rmt_frame_obj_t *got_packed,
        const rmt_frame_obj_t *gold_packed)
{
    // Copy packed members into aligned arrays before comparing
    rmt_symbol_word_t got[IR_CTRL_MAX_FRAME_SIZE];
    rmt_symbol_word_t gold[IR_CTRL_MAX_FRAME_SIZE];

    TEST_ASSERT_NOT_NULL(got_packed);
    TEST_ASSERT_NOT_NULL(gold_packed);

    TEST_ASSERT_TRUE(got_packed->num_syms <= IR_CTRL_MAX_FRAME_SIZE);
    TEST_ASSERT_TRUE(gold_packed->num_syms <= IR_CTRL_MAX_FRAME_SIZE);

    const uint16_t got_n = got_packed->num_syms;
    const uint16_t gold_n = gold_packed->num_syms;

    memcpy(got,  got_packed->rmt_frame_data,  (size_t)got_n  * sizeof(rmt_symbol_word_t));
    memcpy(gold, gold_packed->rmt_frame_data, (size_t)gold_n * sizeof(rmt_symbol_word_t));

    if (got_n == gold_n) {
        // Try strict compare first
        bool strict_ok = true;
        for (uint16_t i = 0; i < gold_n; i++) {
            if (got[i].level0 != gold[i].level0 ||
                got[i].duration0 != gold[i].duration0 ||
                got[i].level1 != gold[i].level1 ||
                got[i].duration1 != gold[i].duration1) {
                strict_ok = false;
                break;
            }
        }
        if (strict_ok) {
            return;
        }
    }

    // Fallback: compare up to n-1 strictly, allow last duration1 to differ.
    const uint16_t n = (got_n < gold_n) ? got_n : gold_n;
    TEST_ASSERT_TRUE_MESSAGE(n >= 1, "frame too short");

    if (n > 1) {
        assert_symbols_equal_aligned(got, gold, (uint16_t)(n - 1));
    }

    // Check last symbol (index n-1) with relaxed duration1
    const uint16_t i = (uint16_t)(n - 1);
    TEST_ASSERT_EQUAL_MESSAGE(got[i].level0,    gold[i].level0,    "last.level0 mismatch");
    TEST_ASSERT_EQUAL_MESSAGE(got[i].duration0, gold[i].duration0, "last.duration0 mismatch");
    TEST_ASSERT_EQUAL_MESSAGE(got[i].level1,    gold[i].level1,    "last.level1 mismatch");
    // duration1 intentionally not strictly checked
}

// -------------------- Test --------------------
TEST_CASE("ir_ctrl: send slot0 while learning slot1 (loopback happy path)", "[ir_ctrl]")
{
    storage_reset();

    ESP_LOGI(TAG, "Loopback required: TX(GPIO%d) -> RX(GPIO%d)", IR_CTRL_TX_GPIO, IR_CTRL_RX_GPIO);

    // Preload slot 0 with golden frame
    const uint16_t slot0 = 0;
    const uint16_t slot1 = 1;
    storage_preload(slot0, &nec_golden_frame);

    // Init ir_ctrl
    ir_ctrl_cfg_t cfg = {
        .resolution = IR_CTRL_RES_HZ,
        .tx_gpio_num = IR_CTRL_TX_GPIO,
        .rx_gpio_num = IR_CTRL_RX_GPIO,
        .decode_margin = 0,
        .store_frame_func = test_store,
        .load_frame_func = test_load,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_init(&cfg));
    TEST_ASSERT_FALSE(ir_ctrl_is_busy());
    ESP_LOGI(TAG, "ir_ctrl initialized with params: res=%luHz, TX GPIO=%d, RX GPIO=%d",
             cfg.resolution, cfg.tx_gpio_num, cfg.rx_gpio_num);

    // Start learning into slot1 (arms RX job asynchronously via worker)
    ir_learn_opts_t learn_opts = {
        .timeout_ms = 0,          // MVP impl may ignore; we rely on our own test timeout
        .min_symbols = 1,
        .max_symbols = IR_CTRL_MAX_FRAME_SIZE,
        .invert_levels = true,   // set true if your physical path inverts the logic levels
        .normalize = true,
        .postprocess = ir_post_nec_canon,
        .user_ctx = NULL,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_learn_start(slot1, &learn_opts));
    // wait_until_busy_or_fail(500);

    // // Give a short moment for worker to call rmt_receive() after marking LEARNING
    // vTaskDelay(pdMS_TO_TICKS(20));

    // Send preloaded slot0; loopback should be captured by the learning session into slot1
    ir_send_opts_t send_opts = {
        .repeat = 0,
        .gap_us = 0,
        .blocking = true,
        .override_carrier = false,
        .carrier_hz = 0,
        .duty_cycle = 0.0f,
    };

    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_send(slot0, send_opts));

    // Wait for learn completion (slot1 stored)
    wait_until_not_busy_or_fail(2000);

    // Read out slot1 and compare vs golden (with trailer tolerance)
    rmt_frame_obj_t learned1 = {0};
    TEST_ASSERT_EQUAL(ESP_OK, ir_ctrl_get_slot_info(slot1, &learned1));
    assert_frame_matches_golden_with_trailer_tolerance(&learned1, &nec_golden_frame);

    ESP_LOGI(TAG, "PASS: slot1 captured transmitted slot0 waveform (loopback)");
}

static bool rx_done_cb(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)ch;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, edata, &hp);
    return hp == pdTRUE;
}

static void rmt_direct_loopback_once(void)
{
    // --- RX channel ---
    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_CTRL_RES_HZ,
        .mem_block_symbols = 64,
        .gpio_num = IR_CTRL_RX_GPIO,
        .flags.invert_in = 0,      // try 1 if needed
        .flags.with_dma = 0,
        .flags.io_loop_back = 0,
    };

    rmt_channel_handle_t rx = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx));

    QueueHandle_t q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rx_done_cb };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx, &cbs, q));

    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 1250,      // matches Espressif example
        .signal_range_max_ns = 12000000,  // matches Espressif example
    };

    // --- TX channel ---
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_CTRL_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .gpio_num = IR_CTRL_TX_GPIO,
        .flags.invert_out = 0,     // try 1 if needed
        .flags.with_dma = 0,
        .flags.io_loop_back = 0,
    };

    rmt_channel_handle_t tx = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &tx));

    // Carrier (your hardware expects 38k)
    rmt_carrier_config_t carrier = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33f,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx, &carrier));

    // Copy encoder
    rmt_encoder_handle_t enc = NULL;
    rmt_copy_encoder_config_t ecfg = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&ecfg, &enc));

    ESP_ERROR_CHECK(rmt_enable(rx));
    ESP_ERROR_CHECK(rmt_enable(tx));

    // Arm RX
    static rmt_symbol_word_t rx_buf[64];
    ESP_ERROR_CHECK(rmt_receive(rx, rx_buf, sizeof(rx_buf), &rcfg));

    // TX the golden symbols
    rmt_transmit_config_t tcfg = {
        .loop_count = 0,
        .flags.eot_level = 1,  // idle high after TX (often helps)
    };

    ESP_LOGI(TAG, "TX %u symbols...", (unsigned)nec_golden_frame.num_syms);
    ESP_ERROR_CHECK(rmt_transmit(tx, enc,
                                 nec_golden_frame.rmt_frame_data,
                                 nec_golden_frame.num_syms * sizeof(rmt_symbol_word_t),
                                 &tcfg));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx, pdMS_TO_TICKS(1000)));

    // Wait RX done
    rmt_rx_done_event_data_t ed;
    if (xQueueReceive(q, &ed, pdMS_TO_TICKS(1000)) == pdPASS) {
        ESP_LOGI(TAG, "RX DONE: num_symbols=%u", (unsigned)ed.num_symbols);
        for (size_t i = 0; i < ed.num_symbols; i++) {
            printf("{%d:%d},{%d:%d}\r\n",
                   ed.received_symbols[i].level0, ed.received_symbols[i].duration0,
                   ed.received_symbols[i].level1, ed.received_symbols[i].duration1);
        }
    } else {
        ESP_LOGE(TAG, "RX timeout (no RX_DONE)");
    }

    // Cleanup (optional for a one-shot test)
    rmt_disable(tx);
    rmt_disable(rx);
    rmt_del_encoder(enc);
    rmt_del_channel(tx);
    rmt_del_channel(rx);
    vQueueDelete(q);
}

/* ---------------- App entry ---------------- */

void app_main(void)
{
    //rmt_direct_loopback_once();
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}
