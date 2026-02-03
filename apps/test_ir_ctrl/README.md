# IR Controller Hardware Integration Test

## Supported Targets

- ESP32
- ESP32-S2
- ESP32-S3

> Tested primarily on ESP32-S3. Other targets require RMT support and compatible GPIO routing.

---

## Hardware Required

- ESP32 development board (one of the supported targets above)
- **IR transmitter** (IR LED + current-limiting resistor or driver)
- **IR receiver module** (compatible with ~38 kHz carrier)
- Jumper wires
- USB cable for flashing and monitoring

---

## How to run (project root)

```bash
idf.py -DAPP_NAME=test_ir_ctrl -B build_test_ir_ctrl build flash monitor
```

---

## Hardware setup

### Required hardware

- **IR transmitter (LED + driver)** connected to `IR_CTRL_TX_GPIO`
- **IR receiver module** (demodulated or raw, as appropriate) connected to `IR_CTRL_RX_GPIO`

> ⚠️ These tests use **real IR hardware**, not a GPIO loopback.

### Wiring

- Connect the **IR transmitter data input** to:
  - `IR_CTRL_TX_GPIO`
- Connect the **IR receiver data output** to:
  - `IR_CTRL_RX_GPIO`
- Ensure **common ground** between the ESP32 and both IR devices.

### Notes

- The IR transmitter and receiver must be **physically facing each other** with a clear line of sight.
- Carrier configuration (38 kHz by default) must match the IR receiver’s expected carrier.
- Dummy GPIOs (`IR_CTRL_DUT_DUMMY_TX_GPIO`, `IR_CTRL_DUT_DUMMY_RX_GPIO`) must remain **unconnected** and are used only to disable the unused DUT direction per test.

---

## Pin configuration (compile-time macros)

These tests use preprocessor macros (defaults are in `test_ir_ctrl_main.c`).
They are **not Kconfig options**.

### Core pins

- `IR_CTRL_TX_GPIO` (default: `18`)
  Physical TX pin used by:

  - external stimulator during learn tests
  - DUT TX during send tests

- `IR_CTRL_RX_GPIO` (default: `17`)
  Physical RX pin used by:

  - DUT RX during learn tests
  - external sniffer during send tests

### Dummy pins (intentional no-connect)

- `IR_CTRL_DUT_DUMMY_TX_GPIO` (default: `1`)
- `IR_CTRL_DUT_DUMMY_RX_GPIO` (default: `1`)

Dummy pins are used to disable the unused direction on the DUT:

- **Learn test**: DUT RX = real, DUT TX = dummy
- **Send test**: DUT TX = real, DUT RX = dummy

Set these to any safe, unconnected GPIO for your board.

### Overriding pins

Override macros at compile time (example via CMake or injected CFLAGS):

```
-DIR_CTRL_TX_GPIO=18
-DIR_CTRL_RX_GPIO=17
-DIR_CTRL_DUT_DUMMY_TX_GPIO=1
-DIR_CTRL_DUT_DUMMY_RX_GPIO=1
```

---

## Test topology

Common components:

- **Storage shim**: RAM-backed slots using `test_store()` / `test_load()`
- **Golden frame**: `nec_golden_frame` (raw RMT symbols)

### Learn path

```
Stim (TX real) → DUT (learn RX real) → storage slot
```

- External RMT TX sends the golden frame
- DUT captures via `ir_ctrl_learn_start()`
- Optional invert + normalize (`ir_post_nec_canon`)
- Stored as `rmt_frame_obj_t`

### Send path

```
Storage slot → DUT (TX real) → Sniffer (RX real)
```

- Slot is preloaded with golden frame
- DUT sends via `ir_ctrl_send()`
- External RMT RX sniffer captures symbols
- Same postprocess logic applied before comparison

---

## Test cases

- **Learn**: external stim → DUT learn → stored frame matches golden
- **Send**: DUT send → sniffer capture → frame matches golden
- **State machine**:
  - `learn_cancel` clears busy state
  - `get_slot_info` rejects while busy

Frame comparison is strict except for the final trailer symbol.

---

## Troubleshooting

- **RX timeout / busy never clears**

  - Check loopback wiring
  - Verify TX/RX GPIO macro values

- **Unexpected `ESP_ERR_INVALID_STATE`**

  - Previous test did not deinit cleanly

- **RX ISR drops**

  - RX queue overflow or task starvation
  - Reduce logging or adjust priorities
