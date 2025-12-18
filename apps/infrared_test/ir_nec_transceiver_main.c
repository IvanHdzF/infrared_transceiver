/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "ir_nec_encoder.h"

#include <string.h>
#include <stdlib.h>

#define EXAMPLE_IR_RESOLUTION_HZ     1000000 // 1MHz resolution, 1 tick = 1us
#define EXAMPLE_IR_TX_GPIO_NUM       18
#define EXAMPLE_IR_RX_GPIO_NUM       17
#define EXAMPLE_IR_NEC_DECODE_MARGIN 300     // Tolerance for parsing RMT symbols into bit stream

/**
 * @brief NEC timing spec
 */
#define NEC_LEADING_CODE_DURATION_0  9000
#define NEC_LEADING_CODE_DURATION_1  4500
#define NEC_PAYLOAD_ZERO_DURATION_0  560
#define NEC_PAYLOAD_ZERO_DURATION_1  560
#define NEC_PAYLOAD_ONE_DURATION_0   560
#define NEC_PAYLOAD_ONE_DURATION_1   1690
#define NEC_REPEAT_CODE_DURATION_0   9000
#define NEC_REPEAT_CODE_DURATION_1   2250

static const char *TAG = "IR_main";

/**
 * @brief Saving NEC decode results
 */
static uint16_t s_nec_code_address;
static uint16_t s_nec_code_command;

/**
 * @brief Check whether a duration is within expected range
 */
static inline bool nec_check_in_range(uint32_t signal_duration, uint32_t spec_duration)
{
    return (signal_duration < (spec_duration + EXAMPLE_IR_NEC_DECODE_MARGIN)) &&
           (signal_duration > (spec_duration - EXAMPLE_IR_NEC_DECODE_MARGIN));
}

/**
 * @brief Check whether a RMT symbol represents NEC logic zero
 */
static bool nec_parse_logic0(rmt_symbol_word_t *rmt_nec_symbols)
{
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ZERO_DURATION_0) &&
           nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ZERO_DURATION_1);
}

/**
 * @brief Check whether a RMT symbol represents NEC logic one
 */
static bool nec_parse_logic1(rmt_symbol_word_t *rmt_nec_symbols)
{
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ONE_DURATION_0) &&
           nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ONE_DURATION_1);
}

/**
 * @brief Decode RMT symbols into NEC address and command
 */
static bool nec_parse_frame(rmt_symbol_word_t *rmt_nec_symbols)
{
    rmt_symbol_word_t *cur = rmt_nec_symbols;
    uint16_t address = 0;
    uint16_t command = 0;
    bool valid_leading_code = nec_check_in_range(cur->duration0, NEC_LEADING_CODE_DURATION_0) &&
                              nec_check_in_range(cur->duration1, NEC_LEADING_CODE_DURATION_1);
    if (!valid_leading_code) {
        return false;
    }
    cur++;
    for (int i = 0; i < 16; i++) {
        if (nec_parse_logic1(cur)) {
            address |= 1 << i;
        } else if (nec_parse_logic0(cur)) {
            address &= ~(1 << i);
        } else {
            return false;
        }
        cur++;
    }
    for (int i = 0; i < 16; i++) {
        if (nec_parse_logic1(cur)) {
            command |= 1 << i;
        } else if (nec_parse_logic0(cur)) {
            command &= ~(1 << i);
        } else {
            return false;
        }
        cur++;
    }
    // save address and command
    s_nec_code_address = address;
    s_nec_code_command = command;
    return true;
}

/**
 * @brief Check whether the RMT symbols represent NEC repeat code
 */
static bool nec_parse_frame_repeat(rmt_symbol_word_t *rmt_nec_symbols)
{
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_REPEAT_CODE_DURATION_0) &&
           nec_check_in_range(rmt_nec_symbols->duration1, NEC_REPEAT_CODE_DURATION_1);
}

/**
 * @brief Decode RMT symbols into NEC scan code and print the result
 */
static void example_parse_nec_frame(rmt_symbol_word_t *rmt_nec_symbols, size_t symbol_num)
{
    printf("NEC frame start---\r\n");
    for (size_t i = 0; i < symbol_num; i++) {
        printf("{%d:%d},{%d:%d}\r\n", rmt_nec_symbols[i].level0, rmt_nec_symbols[i].duration0,
               rmt_nec_symbols[i].level1, rmt_nec_symbols[i].duration1);
    }
    printf("---NEC frame end: ");
    // decode RMT symbols
    switch (symbol_num) {
    case 34: // NEC normal frame
        if (nec_parse_frame(rmt_nec_symbols)) {
            printf("Address=%04X, Command=%04X\r\n\r\n", s_nec_code_address, s_nec_code_command);
        }
        break;
    case 2: // NEC repeat frame
        if (nec_parse_frame_repeat(rmt_nec_symbols)) {
            printf("Address=%04X, Command=%04X, repeat\r\n\r\n", s_nec_code_address, s_nec_code_command);
        }
        break;
    default:
        printf("Unknown NEC frame\r\n\r\n");
        break;
    }
}

static bool example_rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t)user_data;
    // send the received RMT symbols to the parser task
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

//TODO: Add circular buffer here
#define MAX_FRAME_SIZE 64
typedef struct 
{
    rmt_symbol_word_t rmt_frame_data[MAX_FRAME_SIZE];
    size_t symbol_num;
}rmt_frame_obj_t;

rmt_frame_obj_t ir_cmd = {0};


static void invert_rmt_levels(const rmt_symbol_word_t *input, 
                              rmt_symbol_word_t *output, 
                              size_t symbol_num)
{
    for (size_t i = 0; i < symbol_num; i++) {
        output[i].level0 = !input[i].level0;
        output[i].level1 = !input[i].level1;
        output[i].duration0 = input[i].duration0;
        output[i].duration1 = input[i].duration1;
    }
}

static void normalize_rmt_durations(rmt_symbol_word_t *frame, size_t symbol_num)
{
    for (size_t i = 0; i < symbol_num; i++) {
        uint32_t d0 = frame[i].duration0;
        uint32_t d1 = frame[i].duration1;

        // Try to match NEC known pulse durations first
        if (abs((int)d0 - NEC_PAYLOAD_ZERO_DURATION_0) < 200 && abs((int)d1 - NEC_PAYLOAD_ZERO_DURATION_1) < 200) {
            frame[i].duration0 = NEC_PAYLOAD_ZERO_DURATION_0;
            frame[i].duration1 = NEC_PAYLOAD_ZERO_DURATION_1;
        } else if (abs((int)d0 - NEC_PAYLOAD_ONE_DURATION_0) < 200 && abs((int)d1 - NEC_PAYLOAD_ONE_DURATION_1) < 300) {
            frame[i].duration0 = NEC_PAYLOAD_ONE_DURATION_0;
            frame[i].duration1 = NEC_PAYLOAD_ONE_DURATION_1;
        } else if (abs((int)d0 - NEC_LEADING_CODE_DURATION_0) < 1000 && abs((int)d1 - NEC_LEADING_CODE_DURATION_1) < 1000) {
            frame[i].duration0 = NEC_LEADING_CODE_DURATION_0;
            frame[i].duration1 = NEC_LEADING_CODE_DURATION_1;
        } else if (abs((int)d0 - NEC_REPEAT_CODE_DURATION_0) < 1000 && abs((int)d1 - NEC_REPEAT_CODE_DURATION_1) < 1000) {
            frame[i].duration0 = NEC_REPEAT_CODE_DURATION_0;
            frame[i].duration1 = NEC_REPEAT_CODE_DURATION_1;
        } else {
            // Fallback: crude k-means with 2 clusters (short, long) on the fly
            static uint32_t short_avg = 560;
            static uint32_t long_avg = 1690;

            // Normalize duration0
            if (abs((int)d0 - (int)short_avg) < abs((int)d0 - (int)long_avg)) {
                frame[i].duration0 = short_avg;
            } else {
                frame[i].duration0 = long_avg;
            }

            // Normalize duration1
            if (abs((int)d1 - (int)short_avg) < abs((int)d1 - (int)long_avg)) {
                frame[i].duration1 = short_avg;
            } else {
                frame[i].duration1 = long_avg;
            }
        }
    }
}

static void normalize_rmt_frame(const rmt_symbol_word_t *input_frame, 
                                rmt_symbol_word_t *output_frame,
                                size_t symbol_num)
{
    invert_rmt_levels(input_frame, output_frame, symbol_num);
    normalize_rmt_durations(output_frame, symbol_num);
}


/**
 * @brief Store the rmt frame
 * 
 * @param rmt_nec_symbols 
 * @param symbol_num 
 */
static void store_rmt_frame(rmt_symbol_word_t *rmt_nec_symbols, size_t symbol_num)
{
    //TODO: Remove the static counter when button is implemented
    static uint8_t cnt = 0;
    if (cnt > 0 )
    {
        return;
    }

    // TODO: Add concurrency safety in the form of a circular buffer
    if (symbol_num > MAX_FRAME_SIZE)
    {
        ESP_LOGE(TAG, "Failure to store frame, symbol num (%d) > MAX_FRAME_SIZE (%d)", symbol_num, MAX_FRAME_SIZE);
        return;
    }
    

    memcpy(ir_cmd.rmt_frame_data, rmt_nec_symbols, symbol_num * sizeof(rmt_symbol_word_t));
    ir_cmd.symbol_num = symbol_num;


    cnt++;
}

static void save_rmt_cmd(rmt_symbol_word_t *raw_symbols, size_t symbol_num)
{
    rmt_symbol_word_t normalized[MAX_FRAME_SIZE] = {0};
    normalize_rmt_frame(raw_symbols, normalized, symbol_num);
    store_rmt_frame(normalized, symbol_num);
}


void app_main(void)
{
    ESP_LOGI(TAG, "create RMT RX channel");
    rmt_rx_channel_config_t rx_channel_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = EXAMPLE_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64, // amount of RMT symbols that the channel can store at a time
        .gpio_num = EXAMPLE_IR_RX_GPIO_NUM,
    };
    rmt_channel_handle_t rx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

    ESP_LOGI(TAG, "register RX done callback");
    QueueHandle_t receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    assert(receive_queue);
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = example_rmt_rx_done_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

    // the following timing requirement is based on NEC protocol
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,     // the shortest duration for NEC signal is 560us, 1250ns < 560us, valid signal won't be treated as noise
        .signal_range_max_ns = 12000000, // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
    };

    ESP_LOGI(TAG, "create RMT TX channel");
    rmt_tx_channel_config_t tx_channel_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = EXAMPLE_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64, // amount of RMT symbols that the channel can store at a time
        .trans_queue_depth = 4,  // number of transactions that allowed to pending in the background, this example won't queue multiple transactions, so queue depth > 1 is sufficient
        .gpio_num = EXAMPLE_IR_TX_GPIO_NUM,
        //.flags.invert_out = true,        // <-- Enable output inversion
    };
    rmt_channel_handle_t tx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));

    ESP_LOGI(TAG, "modulate carrier to TX channel");
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = 0.33,
        .frequency_hz = 38000, // 38KHz
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_cfg));

    // this example won't send NEC frames in a loop
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // no loop
    };

    ESP_LOGI(TAG, "install IR NEC encoder");
    ir_nec_encoder_config_t nec_encoder_cfg = {
        .resolution = EXAMPLE_IR_RESOLUTION_HZ,
    };
    rmt_encoder_handle_t nec_encoder = NULL;
    ESP_ERROR_CHECK(rmt_new_ir_nec_encoder(&nec_encoder_cfg, &nec_encoder));

    rmt_encoder_handle_t copy_encoder;
    rmt_copy_encoder_config_t copy_enc_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_enc_config, &copy_encoder));


    ESP_LOGI(TAG, "enable RMT TX and RX channels");
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    // save the received RMT symbols
    rmt_symbol_word_t raw_symbols[64]; // 64 symbols should be sufficient for a standard NEC frame
    rmt_rx_done_event_data_t rx_data;
    // ready to receive
    ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));

    const ir_nec_scan_code_t scan_code = {
            .address = 0xFE01,
            .command = 0x748B,
    };
    ESP_ERROR_CHECK(rmt_transmit(tx_channel, nec_encoder, &scan_code, sizeof(scan_code), &transmit_config));
    while (1) {
        // wait for RX done signal
        if (xQueueReceive(receive_queue, &rx_data, pdMS_TO_TICKS(1000)) == pdPASS) {
            // parse the receive symbols and print the result
            save_rmt_cmd(rx_data.received_symbols, rx_data.num_symbols);
            example_parse_nec_frame(rx_data.received_symbols, rx_data.num_symbols);
            // start receive again
            ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
        } else {
            //timeout, transmit predefined IR NEC packets
            // const ir_nec_scan_code_t scan_code = {
            //     .address = 0xFE01,
            //     .command = 0x748B,
            // };
            // ESP_ERROR_CHECK(rmt_transmit(tx_channel, nec_encoder, &scan_code, sizeof(scan_code), &transmit_config));
            // continue;

            if (ir_cmd.symbol_num == 0)
            {
                continue;
            }

            /* Buffer data */
            rmt_frame_obj_t cmd;
            memcpy(&cmd.rmt_frame_data, ir_cmd.rmt_frame_data, ir_cmd.symbol_num * sizeof(rmt_symbol_word_t));
            cmd.symbol_num = ir_cmd.symbol_num;

            /* Replace the first element with the configured leading pulse */
            cmd.rmt_frame_data[0] = (rmt_symbol_word_t) {
                .level0 = 1,
                .duration0 = 9000ULL * EXAMPLE_IR_RESOLUTION_HZ / 1000000,
                .level1 = 0,
                .duration1 = 4500ULL * EXAMPLE_IR_RESOLUTION_HZ / 1000000,
            };

            ESP_LOGI(TAG, "Replaying stored NEC frame with %d symbols", cmd.symbol_num);

            /* rmt_transmit is blocking */
            esp_err_t tx_err = rmt_transmit(tx_channel, copy_encoder, cmd.rmt_frame_data, cmd.symbol_num * sizeof(rmt_symbol_word_t), &transmit_config);
            if (tx_err != ESP_OK)
            {
                ESP_LOGE(TAG,"TX Failed with %d", tx_err);
            }

        }
    }
}
