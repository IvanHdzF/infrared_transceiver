// ir_ctrl.c
//
// IR controller built on ESP-IDF RMT.
// - Learn: capture RMT symbols and store via injected callback
// - Send: load stored symbols and transmit via copy encoder
// - States: IDLE / LEARNING / SENDING
//

//  - Validate slot bounds early (learn_start/send) if you have a fixed slot count in cfg.
//  - Validate opts.postprocess != NULL when opts.normalize==true (or treat normalize as requiring postprocess).
//  - Add a monotonically increasing "op_id" in ctx; stamp it into learn_start and compare in RX_DONE to ignore stale completions.
//  - Ensure cmd_q send failures are surfaced distinctly (ESP_ERR_TIMEOUT / ESP_ERR_NO_MEM) and don’t silently drop ops.
//  - Enf
#include "ir_ctrl/ir_ctrl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"

#define IR_CTRL_TAG "ir_ctrl"

#define IR_CTRL_RX_QUEUE_DEPTH      (4u)
#define IR_CTRL_CMD_QUEUE_DEPTH     (4u)
#define IR_CTRL_TASK_STACK_WORDS    (4096u)
#define IR_CTRL_TASK_PRIO           (10u)

typedef enum {
    IR_STATE_UNINIT = 0,
    IR_STATE_IDLE,
    IR_STATE_LEARNING,
    IR_STATE_SENDING,
} ir_state_t;

typedef enum {
    IR_CMD_LEARN_START = 0,
    IR_CMD_LEARN_CANCEL,
    IR_CMD_SEND,
    IR_CMD_TASK_STOP,
} ir_cmd_type_t;

typedef struct {
    ir_cmd_type_t type;
    union {
        struct {
            uint16_t slot;
            ir_learn_opts_t opts;
            bool opts_present;
        } learn_start;

        struct {
            uint16_t slot;
            ir_send_opts_t opts;
        } send;
    } u;
} ir_cmd_msg_t;

typedef struct {
    ir_ctrl_cfg_t cfg;

    rmt_channel_handle_t rx_chan;
    rmt_channel_handle_t tx_chan;
    rmt_encoder_handle_t copy_enc;

    QueueHandle_t rx_evt_q;     // rmt_rx_done_event_data_t
    QueueHandle_t cmd_q;        // ir_cmd_msg_t
    SemaphoreHandle_t lock;     // protects state + current op fields
    TaskHandle_t task;

    ir_state_t state;

    uint16_t learn_slot;
    uint16_t learn_gen;
    uint16_t active_learn_gen;
    ir_learn_opts_t learn_opts;
    bool learn_opts_present;

    rmt_symbol_word_t rx_raw[IR_CTRL_MAX_FRAME_SIZE];
    rmt_symbol_word_t scratch[IR_CTRL_MAX_FRAME_SIZE];

    bool rx_enabled;
    bool tx_enabled;

} ir_ctrl_ctx_t;

static ir_ctrl_ctx_t s_ctx;

static volatile uint32_t s_rx_isr_count;
static volatile uint32_t s_rx_isr_drop;

// ---------- Utilities ----------
static inline void ir_lock(void)
{
    (void)xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
}

static inline void ir_unlock(void)
{
    (void)xSemaphoreGive(s_ctx.lock);
}

static bool ir_is_inited_nolock(void)
{
    return (s_ctx.state != IR_STATE_UNINIT) &&
           (s_ctx.rx_chan != NULL) &&
           (s_ctx.tx_chan != NULL) &&
           (s_ctx.copy_enc != NULL);
}

static bool ir_busy_nolock(void)
{
    return (s_ctx.state == IR_STATE_LEARNING) || (s_ctx.state == IR_STATE_SENDING);
}

bool ir_ctrl_is_busy(void)
{
    bool busy;
    ir_lock();
    busy = ir_busy_nolock();
    ir_unlock();
    return busy;
}

static void ir_apply_invert(const rmt_symbol_word_t *in, rmt_symbol_word_t *out, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        out[i] = in[i];
        out[i].level0 = !in[i].level0;
        out[i].level1 = !in[i].level1;
    }
}

static esp_err_t ir_apply_carrier_if_needed(const ir_send_opts_t *opts)
{
    if (!opts || !opts->override_carrier) {
        return ESP_OK;
    }
    if (opts->carrier_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!(opts->duty_cycle > 0.0f && opts->duty_cycle < 1.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    rmt_carrier_config_t carrier = {
        .frequency_hz = opts->carrier_hz,
        .duty_cycle = opts->duty_cycle,
    };
    return rmt_apply_carrier(s_ctx.tx_chan, &carrier);
}

static esp_err_t rmt_ensure_rx_channel_enabled(void)
{
    if (s_ctx.rx_enabled) {
        return ESP_OK;
    }

    esp_err_t err = rmt_enable(s_ctx.rx_chan);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        s_ctx.rx_enabled = true;
    }

    return err;
}

static esp_err_t rmt_ensure_tx_channel_enabled(void)
{
    if (s_ctx.tx_enabled) {
        return ESP_OK;
    }

    esp_err_t err = rmt_enable(s_ctx.tx_chan);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (err == ESP_OK) {
        s_ctx.tx_enabled = true;
    }

    return err;
}

static void ir_ctrl_stop_task(void)
{
    if (!s_ctx.task) {
        return;
    }

    ir_cmd_msg_t m = { .type = IR_CMD_TASK_STOP };
    (void)xQueueSend(s_ctx.cmd_q, &m, 0);

    for (int i = 0; i < 50; i++) {
        if (eTaskGetState(s_ctx.task) == eDeleted) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (eTaskGetState(s_ctx.task) != eDeleted) {
        vTaskDelete(s_ctx.task);
    }

    s_ctx.task = NULL;
}

// ---------- RMT callbacks ----------
static bool IRAM_ATTR ir_rx_done_isr(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    (void)channel;
    BaseType_t hp = pdFALSE;
    s_rx_isr_count++;

    if (xQueueSendFromISR((QueueHandle_t)user_data, edata, &hp) != pdPASS) {
        s_rx_isr_drop++;
    }
    return (hp == pdTRUE);
}

// ---------- Core actions (task context) ----------
static esp_err_t ir_rx_start_locked(uint16_t max_syms)
{
    (void)max_syms;

    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 1250,
        .signal_range_max_ns = 12000000,
    };

    esp_err_t err = rmt_receive(s_ctx.rx_chan, s_ctx.rx_raw, sizeof(s_ctx.rx_raw), &rcfg);
    ESP_LOGI(IR_CTRL_TAG, "rx: rmt_receive(buf=%p bytes=%u) -> %s",
             s_ctx.rx_raw, (unsigned)sizeof(s_ctx.rx_raw), esp_err_to_name(err));
    return err;
}

static void ir_finish_to_idle_locked(void)
{
    s_ctx.state = IR_STATE_IDLE;
    s_ctx.learn_slot = 0;
    memset(&s_ctx.learn_opts, 0, sizeof(s_ctx.learn_opts));
    s_ctx.learn_opts_present = false;
}

static esp_err_t ir_do_learn_start(const ir_cmd_msg_t *cmd)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        ESP_LOGE(IR_CTRL_TAG, "learn_start: not inited");
        return ESP_ERR_INVALID_STATE;
    }
    if (ir_busy_nolock()) {
        ir_unlock();
        ESP_LOGE(IR_CTRL_TAG, "learn_start: busy");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.cfg.store_frame_func == NULL) {
        ir_unlock();
        ESP_LOGE(IR_CTRL_TAG, "learn_start: store_frame_func is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.state = IR_STATE_LEARNING;
    s_ctx.learn_slot = cmd->u.learn_start.slot;
    ESP_LOGI(IR_CTRL_TAG, "DO_learn_start: slot=%u", (unsigned)s_ctx.learn_slot);

    s_ctx.learn_opts_present = cmd->u.learn_start.opts_present;
    if (cmd->u.learn_start.opts_present) {
        s_ctx.learn_opts = cmd->u.learn_start.opts;
    } else {
        memset(&s_ctx.learn_opts, 0, sizeof(s_ctx.learn_opts));
    }

    uint16_t cap = IR_CTRL_MAX_FRAME_SIZE;
    if (s_ctx.learn_opts_present && s_ctx.learn_opts.max_symbols != 0 && s_ctx.learn_opts.max_symbols < cap) {
        cap = s_ctx.learn_opts.max_symbols;
    }

    esp_err_t err = ir_rx_start_locked(cap);
    if (err != ESP_OK) {
        ir_finish_to_idle_locked();
        ESP_LOGE(IR_CTRL_TAG, "learn_start: rmt_receive failed: %s", esp_err_to_name(err));
    }
    s_ctx.learn_gen++;
    s_ctx.active_learn_gen = s_ctx.learn_gen;
    ir_unlock();
    return err;
}

static esp_err_t ir_do_learn_cancel(void)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state != IR_STATE_LEARNING) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(IR_CTRL_TAG, "learn: cancel");
    s_ctx.active_learn_gen = 0;
    (void)rmt_disable(s_ctx.rx_chan);
    ir_finish_to_idle_locked();
    ir_unlock();
    return ESP_OK;
}

static esp_err_t ir_store_frame(uint16_t slot, const rmt_symbol_word_t *syms, uint16_t n)
{
    if (n == 0 || n > IR_CTRL_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }

    rmt_frame_obj_t obj;
    memset(&obj, 0, sizeof(obj));
    memcpy(obj.rmt_frame_data, syms, (size_t)n * sizeof(rmt_symbol_word_t));
    obj.num_syms = n;

    bool ok = s_ctx.cfg.store_frame_func(slot, &obj, (uint16_t)sizeof(obj));
    return ok ? ESP_OK : ESP_FAIL;
}

static void ir_handle_rx_done(const rmt_rx_done_event_data_t *rx)
{
    ESP_LOGI(IR_CTRL_TAG, "learn: RX done, symbols=%u", (unsigned)rx->num_symbols);

    uint16_t slot;
    ir_learn_opts_t opts;
    bool opts_present;
    ir_state_t st;

    ir_lock();
    st = s_ctx.state;
    slot = s_ctx.learn_slot;
    opts_present = s_ctx.learn_opts_present;
    opts = s_ctx.learn_opts;
    ir_unlock();

    if (st != IR_STATE_LEARNING) {
        return;
    }

    if (s_ctx.active_learn_gen != s_ctx.learn_gen) {
        ESP_LOGW(IR_CTRL_TAG, "learn: stale RX done, ignoring");
        return;
    }

    uint16_t n = (uint16_t)rx->num_symbols;
    if (n > IR_CTRL_MAX_FRAME_SIZE) {
        ESP_LOGW(IR_CTRL_TAG, "learn: drop too large: %u", (unsigned)n);
        (void)ir_do_learn_cancel();
        return;
    }

    if (opts_present) {
        if (opts.min_symbols && n < opts.min_symbols) {
            ESP_LOGW(IR_CTRL_TAG, "learn: drop short: %u < %u", (unsigned)n, (unsigned)opts.min_symbols);
            (void)ir_do_learn_cancel();
            return;
        }
        if (opts.max_symbols && n > opts.max_symbols) {
            ESP_LOGW(IR_CTRL_TAG, "learn: drop long: %u > %u", (unsigned)n, (unsigned)opts.max_symbols);
            (void)ir_do_learn_cancel();
            return;
        }
    }

    const rmt_symbol_word_t *src = rx->received_symbols;
    rmt_symbol_word_t *tmp = s_ctx.scratch;
    uint16_t tmp_n = n;

    if (opts_present && opts.invert_levels) {
        ESP_LOGI(IR_CTRL_TAG, "learn: invert");
        ir_apply_invert(src, tmp, n);
        src = tmp;
    }

    const rmt_symbol_word_t *final_syms = src;
    uint16_t final_n = tmp_n;

    if (opts_present && opts.normalize && opts.postprocess) {
        rmt_symbol_word_t *out = (src == tmp) ? s_ctx.rx_raw : s_ctx.scratch;
        uint16_t out_n = IR_CTRL_MAX_FRAME_SIZE;

        opts.postprocess(src, tmp_n, out, &out_n, opts.user_ctx);

        if (out_n == 0 || out_n > IR_CTRL_MAX_FRAME_SIZE) {
            ESP_LOGW(IR_CTRL_TAG, "learn: postprocess invalid len=%u", (unsigned)out_n);
            (void)ir_do_learn_cancel();
            return;
        }
        final_syms = out;
        final_n = out_n;
    }

    ESP_LOGI(IR_CTRL_TAG, "learn: final symbols=%u", (unsigned)final_n);

    esp_err_t st_err = ir_store_frame(slot, final_syms, final_n);
    if (st_err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "learn: store failed: %s", esp_err_to_name(st_err));
        (void)ir_do_learn_cancel();
        return;
    }

    ESP_LOGI(IR_CTRL_TAG, "learn: stored slot %u", (unsigned)slot);

    ir_lock();
    (void)rmt_disable(s_ctx.rx_chan);
    ir_finish_to_idle_locked();
    ir_unlock();
}

static esp_err_t ir_load_frame(uint16_t slot, rmt_frame_obj_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.cfg.load_frame_func == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t got = 0;
    bool ok = s_ctx.cfg.load_frame_func(slot, out, (uint16_t)sizeof(*out), &got);
    if (!ok) {
        return ESP_ERR_NOT_FOUND;
    }
    if (got != sizeof(*out)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->num_syms == 0 || out->num_syms > IR_CTRL_MAX_FRAME_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t ir_do_send(const ir_cmd_msg_t *cmd)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (ir_busy_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.cfg.load_frame_func == NULL) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.state = IR_STATE_SENDING;
    ir_unlock();

    esp_err_t err;

    rmt_frame_obj_t obj;
    memset(&obj, 0, sizeof(obj));

    err = ir_load_frame(cmd->u.send.slot, &obj);
    if (err != ESP_OK) {
        goto out;
    }

    err = rmt_ensure_tx_channel_enabled();
    if (err != ESP_OK) {
        ESP_LOGW(IR_CTRL_TAG, "rmt_ensure_tx_channel_enabled failed: %s", esp_err_to_name(err));
    }

    err = ir_apply_carrier_if_needed(&cmd->u.send.opts);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "apply_carrier failed: %s", esp_err_to_name(err));
        goto out;
    }

    rmt_transmit_config_t txcfg = {
        .loop_count = 0,
    };

    uint32_t total = 1u + cmd->u.send.opts.repeat;

    for (uint32_t i = 0; i < total; i++) {
        ESP_LOGI(IR_CTRL_TAG, "send: slot=%u symbols=%u (repeat %u/%u)",
                 (unsigned)cmd->u.send.slot,
                 (unsigned)obj.num_syms,
                 (unsigned)i,
                 (unsigned)(total - 1u));

        err = rmt_transmit(s_ctx.tx_chan, s_ctx.copy_enc,
                           obj.rmt_frame_data,
                           (size_t)obj.num_syms * sizeof(rmt_symbol_word_t),
                           &txcfg);
        if (err != ESP_OK) {
            ESP_LOGE(IR_CTRL_TAG, "rmt_transmit failed: %s", esp_err_to_name(err));
            goto out;
        }

        if (cmd->u.send.opts.blocking) {
            err = rmt_tx_wait_all_done(s_ctx.tx_chan, -1);
            if (err != ESP_OK) {
                ESP_LOGE(IR_CTRL_TAG, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(err));
                goto out;
            }
        }

        if (cmd->u.send.opts.gap_us && (i + 1u < total)) {
            vTaskDelay(pdMS_TO_TICKS((cmd->u.send.opts.gap_us + 999u) / 1000u));
        }
    }

    err = ESP_OK;

out:
    ir_lock();
    s_ctx.state = IR_STATE_IDLE;
    ir_unlock();
    return err;
}

// ---------- Worker task ----------
static void ir_ctrl_task(void *arg)
{
    (void)arg;

    for (;;) {
        rmt_rx_done_event_data_t rx;
        while (xQueueReceive(s_ctx.rx_evt_q, &rx, 0) == pdPASS) {
            ir_handle_rx_done(&rx);
        }

        ir_cmd_msg_t cmd;
        if (xQueueReceive(s_ctx.cmd_q, &cmd, pdMS_TO_TICKS(50)) == pdPASS) {
            ESP_LOGI(IR_CTRL_TAG, "ir_ctrl_task: got cmd %d", (int)cmd.type);
            esp_err_t err = ESP_OK;

            switch (cmd.type) {
            case IR_CMD_LEARN_START:
                err = ir_do_learn_start(&cmd);
                break;
            case IR_CMD_LEARN_CANCEL:
                err = ir_do_learn_cancel();
                break;
            case IR_CMD_SEND:
                err = ir_do_send(&cmd);
                break;
            case IR_CMD_TASK_STOP:
                ESP_LOGI(IR_CTRL_TAG, "ir_ctrl_task: stopping task");
                vTaskDelete(NULL);
                break;
            default:
                err = ESP_ERR_INVALID_ARG;
                break;
            }

            if (err != ESP_OK) {
                ESP_LOGW(IR_CTRL_TAG, "cmd %d failed: %s", (int)cmd.type, esp_err_to_name(err));
            }
        }
    }
}

// ---------- Public API ----------
esp_err_t ir_ctrl_init(const ir_ctrl_cfg_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->resolution == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.cfg = *cfg;
    s_ctx.state = IR_STATE_UNINIT;

    s_ctx.lock = xSemaphoreCreateMutex();
    if (!s_ctx.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_ctx.rx_evt_q = xQueueCreate(IR_CTRL_RX_QUEUE_DEPTH, sizeof(rmt_rx_done_event_data_t));
    s_ctx.cmd_q    = xQueueCreate(IR_CTRL_CMD_QUEUE_DEPTH, sizeof(ir_cmd_msg_t));
    if (!s_ctx.rx_evt_q || !s_ctx.cmd_q) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;

    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = (gpio_num_t)s_ctx.cfg.rx_gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = s_ctx.cfg.resolution,
        .mem_block_symbols = IR_CTRL_MAX_FRAME_SIZE,
    };

    err = rmt_new_rx_channel(&rx_cfg, &s_ctx.rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_rx_event_callbacks_t rxcbs = {
        .on_recv_done = ir_rx_done_isr,
    };
    err = rmt_rx_register_event_callbacks(s_ctx.rx_chan, &rxcbs, s_ctx.rx_evt_q);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_rx_register_event_callbacks failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = (gpio_num_t)s_ctx.cfg.tx_gpio_num,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = s_ctx.cfg.resolution,
        .mem_block_symbols = IR_CTRL_MAX_FRAME_SIZE,
        .trans_queue_depth = 4,
    };

    err = rmt_new_tx_channel(&tx_cfg, &s_ctx.tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_carrier_config_t default_carrier = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33f,
    };
    err = rmt_apply_carrier(s_ctx.tx_chan, &default_carrier);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_apply_carrier failed: %s", esp_err_to_name(err));
        return err;
    }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_ctx.copy_enc);
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_ensure_tx_channel_enabled();
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_ensure_tx_channel_enabled failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rmt_ensure_rx_channel_enabled();
    if (err != ESP_OK) {
        ESP_LOGE(IR_CTRL_TAG, "rmt_ensure_rx_channel_enabled failed: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(ir_ctrl_task, "ir_ctrl", IR_CTRL_TASK_STACK_WORDS, NULL, IR_CTRL_TASK_PRIO, &s_ctx.task);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ir_lock();
    s_ctx.state = IR_STATE_IDLE;
    ir_unlock();

    return ESP_OK;
}

esp_err_t ir_ctrl_learn_start(uint16_t slot, const ir_learn_opts_t *opts)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (ir_busy_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ir_unlock();

    ir_cmd_msg_t cmd = {0};
    cmd.type = IR_CMD_LEARN_START;
    cmd.u.learn_start.slot = slot;
    ESP_LOGI(IR_CTRL_TAG, "CTRL_learn_start: slot=%u", (unsigned)slot);

    if (opts) {
        cmd.u.learn_start.opts_present = true;
        cmd.u.learn_start.opts = *opts;

        if (cmd.u.learn_start.opts.max_symbols == 0 || cmd.u.learn_start.opts.max_symbols > IR_CTRL_MAX_FRAME_SIZE) {
            cmd.u.learn_start.opts.max_symbols = IR_CTRL_MAX_FRAME_SIZE;
        }
        if (cmd.u.learn_start.opts.min_symbols > IR_CTRL_MAX_FRAME_SIZE) {
            cmd.u.learn_start.opts.min_symbols = IR_CTRL_MAX_FRAME_SIZE;
        }
    } else {
        cmd.u.learn_start.opts_present = false;
        memset(&cmd.u.learn_start.opts, 0, sizeof(cmd.u.learn_start.opts));
    }

    if (xQueueSend(s_ctx.cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ir_ctrl_learn_cancel(void)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ir_unlock();

    ir_cmd_msg_t cmd = {0};
    cmd.type = IR_CMD_LEARN_CANCEL;
    if (xQueueSend(s_ctx.cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ir_ctrl_send(uint16_t slot, ir_send_opts_t opts)
{
    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.state == IR_STATE_SENDING) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ir_unlock();

    ir_cmd_msg_t cmd = {0};
    cmd.type = IR_CMD_SEND;
    cmd.u.send.slot = slot;
    cmd.u.send.opts = opts;

    if (xQueueSend(s_ctx.cmd_q, &cmd, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    if (opts.blocking) {
        for (;;) {
            if (!ir_ctrl_is_busy()) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    return ESP_OK;
}

esp_err_t ir_ctrl_get_slot_info(uint16_t slot, rmt_frame_obj_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }

    ir_lock();
    if (!ir_is_inited_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (ir_busy_nolock()) {
        ir_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    ir_unlock();

    return ir_load_frame(slot, info);
}

esp_err_t print_ir_ctrl_rx_isr_stats(void)
{
    uint32_t count = s_rx_isr_count;
    uint32_t drop = s_rx_isr_drop;

    ESP_LOGI(IR_CTRL_TAG, "IR RX ISR stats: total calls=%lu, drops=%lu", count, drop);
    return ESP_OK;
}

esp_err_t ir_ctrl_deinit(void)
{
    if (s_ctx.lock) {
        xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
        s_ctx.state = IR_STATE_UNINIT;
        xSemaphoreGive(s_ctx.lock);
    }

    ir_ctrl_stop_task();

    if (s_ctx.tx_chan) {
        (void)rmt_disable(s_ctx.tx_chan);
        (void)rmt_del_channel(s_ctx.tx_chan);
        s_ctx.tx_chan = NULL;
    }
    if (s_ctx.rx_chan) {
        (void)rmt_disable(s_ctx.rx_chan);
        (void)rmt_del_channel(s_ctx.rx_chan);
        s_ctx.rx_chan = NULL;
    }
    if (s_ctx.copy_enc) {
        (void)rmt_del_encoder(s_ctx.copy_enc);
        s_ctx.copy_enc = NULL;
    }

    if (s_ctx.rx_evt_q) {
        vQueueDelete(s_ctx.rx_evt_q);
        s_ctx.rx_evt_q = NULL;
    }
    if (s_ctx.cmd_q)    {
        vQueueDelete(s_ctx.cmd_q);
        s_ctx.cmd_q = NULL;
    }
    if (s_ctx.lock)     {
        vSemaphoreDelete(s_ctx.lock);
        s_ctx.lock = NULL;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    return ESP_OK;
}
