# SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded import Dut


@pytest.mark.esp32
@pytest.mark.ir_transceiver
def test_ir_nec_example(dut: Dut) -> None:
    dut.expect('IR_main: create RMT RX channel')
    dut.expect('IR_main: register RX done callback')
    dut.expect('IR_main: create RMT TX channel')
    dut.expect('IR_main: modulate carrier to TX channel')
    dut.expect('IR_main: install IR NEC encoder')
    dut.expect('IR_main: enable RMT TX and RX channels')
    dut.expect('NEC frame start---')


