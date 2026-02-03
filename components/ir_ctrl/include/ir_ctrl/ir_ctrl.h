#ifndef IR_CTRL_H
#define IR_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"

/**
 * @def IR_CTRL_MAX_FRAME_SIZE
 * @brief Maximum number of RMT symbols supported per learned/sent frame in this MVP.
 *
 * A symbol is one @ref rmt_symbol_word_t element (level0/duration0 + level1/duration1).
 * If a received frame exceeds this limit, learning must fail.
 */
#define IR_CTRL_MAX_FRAME_SIZE 64

/**
 * @typedef store_func_t
 * @brief Storage callback to persist a learned IR waveform for a given slot.
 *
 * The IR controller calls this after learning completes (and after any optional processing).
 *
 * @param slot Slot index to store into.
 * @param data Pointer to the blob to store. For this MVP, the blob is expected to be a
 *             serialized @ref rmt_frame_obj_t (or equivalent raw symbol payload), as defined
 *             by the IR controller implementation.
 * @param len  Length of @p data in bytes.
 *
 * @return true on success, false on failure.
 */
typedef bool(store_func_t)(uint16_t slot, const void *data, uint16_t len);

/**
 * @typedef load_func_t
 * @brief Storage callback to load a previously stored IR waveform for a given slot.
 *
 * The IR controller calls this when sending a command.
 *
 * @param slot    Slot index to load from.
 * @param out     Output buffer to fill with the stored blob.
 * @param max_len Capacity of @p out in bytes.
 * @param out_len Output: number of bytes written into @p out on success.
 *
 * @return true on success, false on failure (e.g., slot not found, storage error).
 */
typedef bool(load_func_t)(uint16_t slot, void *out, uint16_t max_len, uint16_t *out_len);

/**
 * @brief IR controller configuration.
 *
 * This component is protocol-agnostic: it learns and sends raw RMT symbols.
 * Storage is injected via callbacks to keep the component reusable and mockable.
 */
typedef struct __attribute__((packed))
{
    uint32_t   resolution;        /**< RMT channel resolution in Hz (ticks per second). */
    uint8_t    tx_gpio_num;        /**< RMT TX GPIO. */
    uint8_t    rx_gpio_num;        /**< RMT RX GPIO. */

    /**
     * @brief Tolerance/margin for higher-level protocol parsing (if used by the application).
     *
     * @note The core IR controller does not require this for raw capture/replay, but it can be
     *       carried here for applications that also decode frames (e.g., NEC demo).
     */
    uint16_t   decode_margin;

    store_func_t *store_frame_func; /**< Persist learned frame for a slot. Must not be NULL to use learning. */
    load_func_t  *load_frame_func;  /**< Load stored frame for a slot. Must not be NULL to use sending-from-slot. */
}
ir_ctrl_cfg_t;

/**
 * @brief Options controlling a "learn" operation.
 *
 * These options define acceptance criteria and optional processing applied to the captured
 * symbol stream before it is stored via @ref ir_ctrl_cfg_t::store_frame_func.
 */
typedef struct {
    uint32_t timeout_ms;     /**< Max time to wait for a frame to be received (0 = implementation-defined). */
    uint16_t min_symbols;    /**< Reject captures shorter than this (0 = no minimum). */
    uint16_t max_symbols;    /**< Hard cap for capture (0 = use IR_CTRL_MAX_FRAME_SIZE or implementation-defined). */
    bool     invert_levels;  /**< If true, invert level0/level1 on all symbols prior to storing. */

    /**
     * @brief Apply optional normalization hook.
     *
     * If true and @ref postprocess is non-NULL, the controller calls the hook to transform
     * captured symbols into the final symbols that will be stored.
     *
     * @note "Normalization" is intentionally application-defined (protocol-specific logic should
     *       live outside the core component).
     */
    bool     normalize;

    /**
     * @brief Optional post-processing hook.
     *
     * Called after capture (and after optional inversion). The hook writes an output symbol stream
     * into @p out and updates @p out_len with the number of produced symbols.
     *
     * @param in       Input symbols (captured).
     * @param in_len   Number of input symbols.
     * @param out      Output buffer provided by the IR controller.
     * @param out_len  In: capacity in symbols of @p out. Out: number of produced symbols.
     * @param user_ctx Opaque pointer forwarded from @ref user_ctx.
     */
    void (*postprocess)(
        const rmt_symbol_word_t *in,
        uint16_t in_len,
        rmt_symbol_word_t *out,
        uint16_t *out_len,
        void *user_ctx
    );

    void *user_ctx;          /**< Passed to @ref postprocess as-is. */
} ir_learn_opts_t;

/**
 * @brief Options controlling a "send" operation.
 *
 * Defines runtime transmission behavior (repeat/gap and optional carrier override).
 */
typedef struct {
    uint32_t repeat;            /**< 0 = send once, N = repeat N additional times (exact semantics implementation-defined). */
    uint32_t gap_us;            /**< Inter-frame gap in microseconds (0 = implementation-defined default). */
    bool     blocking;          /**< If true, ir_ctrl_send waits until TX is complete. */
    bool     override_carrier;  /**< If true, use carrier_hz/duty_cycle instead of slot/default config. */
    uint32_t carrier_hz;        /**< Carrier frequency in Hz (valid only if override_carrier is true). */
    float    duty_cycle;        /**< Carrier duty cycle (0..1) (valid only if override_carrier is true). */
} ir_send_opts_t;

/**
 * @brief In-memory frame object (symbols + count).
 *
 * This is the canonical container for a learned/sent waveform in this MVP.
 *
 * @note If you persist this struct directly, be mindful that @ref rmt_symbol_word_t layout may
 *       change across ESP-IDF versions. A stable on-flash encoding can be added later if needed.
 */
typedef struct {
    rmt_symbol_word_t rmt_frame_data[IR_CTRL_MAX_FRAME_SIZE]; /**< Symbol buffer. Only [0..num_syms-1] are valid. */
    uint16_t          num_syms;                       /**< Number of valid symbols in @ref rmt_frame_data. */
} rmt_frame_obj_t;

/**
 * @brief Initialize the IR controller.
 *
 * Creates/configures RMT RX and TX channels based on @p cfg and prepares internal resources.
 *
 * @param cfg Module configuration. Must not be NULL.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if cfg is invalid (NULL, missing callbacks, invalid GPIOs, etc.)
 * - ESP_ERR_NO_MEM or other ESP-IDF errors from underlying driver setup
 */
esp_err_t ir_ctrl_init(const ir_ctrl_cfg_t *cfg);

/**
 * @brief Start learning (capturing) a new IR command into the given slot.
 *
 * Transitions the module into LEARNING state and begins RX capture. When a complete frame is
 * received, the controller optionally applies processing based on @p opts and stores the result
 * via @ref ir_ctrl_cfg_t::store_frame_func.
 *
 * @param slot Slot index to learn into.
 * @param opts Learning options (may be NULL for defaults; defaults are implementation-defined).
 *
 * @return
 * - ESP_OK if learning started
 * - ESP_ERR_INVALID_STATE if already learning/sending
 * - ESP_ERR_INVALID_ARG on invalid parameters
 * - Other ESP-IDF/RMT errors if RX cannot be started
 */
esp_err_t ir_ctrl_learn_start(uint16_t slot, const ir_learn_opts_t *opts);

/**
 * @brief Cancel an in-progress learn operation.
 *
 * If the module is in LEARNING state, stop RX and transition back to IDLE.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if not currently learning (implementation-defined if treated as no-op)
 */
esp_err_t ir_ctrl_learn_cancel(void);

/**
 * @brief Send the IR command stored in the given slot.
 *
 * Loads the waveform for @p slot via @ref ir_ctrl_cfg_t::load_frame_func and transmits it using
 * a copy encoder. The module enters SENDING state while TX is active.
 *
 * @param slot Slot index to transmit.
 * @param opts Runtime send options.
 *
 * @return
 * - ESP_OK on successful start (and completion if opts.blocking is true)
 * - ESP_ERR_INVALID_STATE if busy learning/sending
 * - ESP_ERR_INVALID_ARG if arguments are invalid
 * - ESP_ERR_NOT_FOUND (or similar) if the slot is empty (implementation-defined)
 * - Other ESP-IDF/RMT errors if TX fails
 */
esp_err_t ir_ctrl_send(uint16_t slot, ir_send_opts_t opts);

/**
 * @brief Retrieve information about a stored slot waveform.
 *
 * Loads the slot data (implementation-defined; typically via @ref ir_ctrl_cfg_t::load_frame_func)
 * and writes it into @p info.
 *
 * @param slot Slot index to query.
 * @param info Output buffer to receive slot waveform info. Must not be NULL.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_ARG if info is NULL
 * - ESP_ERR_NOT_FOUND (or similar) if the slot is empty (implementation-defined)
 * - Other errors on storage/IO failure
 */
esp_err_t ir_ctrl_get_slot_info(uint16_t slot, rmt_frame_obj_t *info);

/**
 * @brief Check whether the IR controller is busy.
 *
 * @return true if the module is in LEARNING or SENDING state, false if IDLE.
 */
bool ir_ctrl_is_busy(void);

/**
 * @brief Print IR controller RX ISR statistics.
 *
 * Prints the total number of RX ISR calls and the number of dropped calls
 * (due to queue full) since initialization.
 *
 * @return
 * - ESP_OK on success
 */
esp_err_t print_ir_ctrl_rx_isr_stats(void);

/**
 * @brief Deinitialize the IR controller.
 *
 * Stops the worker task and releases all RMT channels and internal resources.
 *
 * @return
 * - ESP_OK on success
 * - ESP_ERR_INVALID_STATE if not initialized
 */
esp_err_t ir_ctrl_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // IR_CTRL_H
