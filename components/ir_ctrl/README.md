# ir_ctrl — IR learn + replay (ESP-IDF RMT)

Protocol-agnostic IR controller that **learns** and **replays** raw `rmt_symbol_word_t` sequences using ESP-IDF RMT.
It **does not** decode/encode NEC/etc; any protocol logic lives outside via optional postprocess.

---

## Public API (what you call)

- `ir_ctrl_init(cfg)`
  Creates RMT RX/TX channels + copy encoder and starts the worker task.

- `ir_ctrl_learn_start(slot, opts)` / `ir_ctrl_learn_cancel()`
  Start/cancel a learn operation. On RX completion, stores the learned frame via `store_frame_func`.

- `ir_ctrl_send(slot, opts)`
  Loads a stored frame via `load_frame_func` and transmits it (optionally repeat/gap, optional carrier override).

- `ir_ctrl_get_slot_info(slot, info)`
  Loads the stored frame into `info` (must be `rmt_frame_obj_t` in this MVP).

- `ir_ctrl_is_busy()`
  `true` when `LEARNING` or `SENDING`.

- `print_ir_ctrl_rx_isr_stats()`
  Dumps RX ISR enqueue stats (calls vs drops).

- `ir_ctrl_deinit()`
  Stops task and releases RMT + RTOS resources.

---

## Data model

- `IR_CTRL_MAX_FRAME_SIZE = 64` symbols (MVP hard cap).
- Stored waveform blob is `rmt_frame_obj_t`:
  - `rmt_frame_data[64]` symbol buffer
  - `num_syms` valid count
- Slots are **external**: this component does not define slot count or persistence format beyond “store/load this blob”.

> Note: persisting `rmt_frame_obj_t` directly ties storage to ESP-IDF’s `rmt_symbol_word_t` layout.

---

## Configuration & dependencies

### `ir_ctrl_cfg_t` (must-haves)

- `resolution` (Hz): RMT tick rate.
- `tx_gpio_num`, `rx_gpio_num`
- `store_frame_func(slot, data, len)` (required for learn)
- `load_frame_func(slot, out, max_len, out_len)` (required for send/get_slot_info)

### Internal resources created

- RMT: `rx_chan`, `tx_chan`, `copy_enc`
- FreeRTOS: worker task, command queue, RX event queue, mutex

---

## Operation flows

### Learn flow

1. `ir_ctrl_learn_start(slot, opts)` enqueues a command
2. Worker transitions to `LEARNING`, calls `rmt_receive()` into `rx_raw`
3. RX done ISR (`on_recv_done`) pushes event to `rx_evt_q`
4. Worker consumes RX event and validates:
   - `num_symbols <= IR_CTRL_MAX_FRAME_SIZE`
   - optional `min_symbols/max_symbols`
5. Optional transforms:
   - `invert_levels`: flips `level0/level1`
   - `normalize + postprocess`: rewrites symbols to final stream
6. Stores final stream as `rmt_frame_obj_t` via `store_frame_func`
7. Disables RX channel and returns to `IDLE`

### Send flow

1. `ir_ctrl_send(slot, opts)` enqueues a command
2. Worker transitions to `SENDING`
3. Loads `rmt_frame_obj_t` via `load_frame_func` (expects exact `sizeof(rmt_frame_obj_t)`)
4. Optional carrier override: `rmt_apply_carrier()` if `override_carrier`
5. Transmits via `rmt_transmit(copy_enc, obj.rmt_frame_data, obj.num_syms * sizeof(symbol))`
6. Optional blocking wait: `rmt_tx_wait_all_done()`
7. Optional repeat + gap
8. Returns to `IDLE`

---

## Concurrency / threading contract

- Public APIs enqueue commands; a **single worker task** serializes operations.
- RX done callback runs in ISR context and only enqueues to `rx_evt_q`.
- Busy states:
  - `IDLE`: accepts learn/send/get_slot_info
  - `LEARNING`: rejects new learn/send/get_slot_info
  - `SENDING`: rejects learn/send/get_slot_info (send checks `state == SENDING`)
- `ir_ctrl_send(...blocking=true)` busy-waits by polling `ir_ctrl_is_busy()` (10ms sleep).

---

## Error behavior (common cases)

### Init

- `ESP_ERR_INVALID_ARG`: `cfg==NULL` or `resolution==0`
- Propagates driver failures from `rmt_new_*`, `rmt_apply_carrier`, `rmt_enable`, etc.

### Learn

- `ESP_ERR_INVALID_STATE`: not inited / busy / missing `store_frame_func`
- Learn completion rejects and cancels if:
  - too many symbols (`>64`)
  - below `min_symbols` / above `max_symbols`
  - postprocess produces `out_len==0` or `>64`
  - store callback fails

### Send / slot info

- `ESP_ERR_INVALID_STATE`: not inited / busy / missing `load_frame_func`
- `ESP_ERR_NOT_FOUND`: slot missing (storage says “no”)
- `ESP_ERR_INVALID_SIZE`: stored blob size mismatch or `num_syms` out of bounds
- Carrier override invalid if:
  - `carrier_hz==0`
  - `duty_cycle` not in `(0,1)`

---

## Storage callback expectations (MVP)

This implementation stores/loads **exactly** `sizeof(rmt_frame_obj_t)` bytes.

- `store_frame_func(slot, &obj, sizeof(obj))` where `obj.num_syms` is set
- `load_frame_func(slot, out, sizeof(*out), &got)` must return:
  - `ok=true`
  - `got == sizeof(rmt_frame_obj_t)`
  - `out->num_syms in [1..64]`

If you want a stable on-flash encoding, add a versioned header + symbol packing later.

---

## Debug

- Tag: `"ir_ctrl"`
- RX ISR stats:
  - `total calls`: ISR callback invocations
  - `drops`: enqueue failures (RX event queue full) → implies missed frames or overload

Call:

- `print_ir_ctrl_rx_isr_stats()`
