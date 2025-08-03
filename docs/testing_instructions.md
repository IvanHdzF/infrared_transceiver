# ESP32 IR Transceiver Test Procedure

## 1. Environment Setup

### Install dependencies
```bash
sudo apt update
sudo apt install python3-venv python3-pip minicom
```

### Create and activate virtual environment
```bash
python3 -m venv venv
source venv/bin/activate
```

### Install Python packages
```bash
pip install -U pip
pip install pytest pytest-embedded pytest-embedded-serial
```

---

## 2. Connecting the Device

- Connect the ESP32 device via USB.
- Verify the serial port:
```bash
ls /dev/ttyACM*
```
Example output:
```
/dev/ttyACM0
```

- Make sure no other program (e.g., `minicom`) is using the port before running tests.

---

## 3. Resetting the Device

### Manual Reset
- Press the physical reset button on the board.

### Via OpenOCD
If OpenOCD is running:
```bash
echo "reset" | telnet localhost 4444
```

---

## 4. Running the Tests

From project root:

```bash
pytest pytest_ir_nec.py --embedded-services serial --port /dev/ttyACM0 -v
```

Notes:
- Change `/dev/ttyACM0` to the actual device port if different.
- Use `--maxfail=1` to stop after the first failure.

---

## 5. Adjusting Test to Match Firmware Logs

If the firmware log prefix changes, update the test expectations in `pytest_ir_nec.py`.  
Example:
```python
dut.expect('IR_main: create RMT RX channel')
```
instead of:
```python
dut.expect('example: create RMT RX channel')
```

---

## 6. Debugging Common Issues

### Port Busy
If you used `minicom`, exit with:
```
Ctrl + A, then X
```
or:
```bash
sudo pkill minicom
```

### Test Fails on `expect_exact`
- Check that the expected string matches the firmware log output exactly.
- Use `dut.expect('<substring>')` for partial matches.

---

## 7. Project Notes
- Firmware currently does not accept UART reset commands.
- For automated reset, integrate OpenOCD reset command in pytest fixture.
- Custom pytest marks (`@pytest.mark.esp32`, `@pytest.mark.ir_transceiver`) should be registered in `pytest.ini`:
```ini
[pytest]
markers =
    esp32: mark test for ESP32 devices
    ir_transceiver: mark test for IR transceiver functionality
```
