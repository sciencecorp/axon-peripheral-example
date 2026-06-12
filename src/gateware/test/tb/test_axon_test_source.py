"""cocotb tests for the axon_test_source peripheral.

The DUT is configured with a channel count and a per-frame sample period, then
streams DATA_FRAME packets of incrementing-counter payload — one frame per
period — until STOP_STREAM. These tests drive the command frames and check the
emitted data ramp.

# CUSTOMIZE: this file is a starter, NOT auto-regenerated. Hand-edits are
# expected and preserved by ``axon-peripheral-sdk generate``.
"""
from __future__ import annotations

import os
import struct

import cocotb
import pytest
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, Timer
from cocotbext.axi import (
    AxiStreamBus,
    AxiStreamFrame,
    AxiStreamSink,
    AxiStreamSource,
)

from axon_peripheral_sdk.profiles.paths import (
    framework_sv_sources,
    resolve_install_path,
)
from axon_peripheral_sdk.sim.cocotb_runner import cocotb_pytest_runner
from axon_peripheral_sdk.sim.frames import generate_packet, parse_packet


CLK_PERIOD_NS = 12.5

# Message-type opcodes — must match axon_test_source_peripheral.sv.
MSG_CONFIGURE = 0x0052
MSG_START_STREAM = 0x0054
MSG_STOP_STREAM = 0x0055
MSG_DATA_FRAME = 0x0056


async def _reset(dut) -> None:
    """Apply an active-high reset pulse to the DUT."""
    dut.rst.value = 1
    await Timer(50, unit="ns")
    dut.rst.value = 0


def _make_source(dut) -> AxiStreamSource:
    return AxiStreamSource(
        AxiStreamBus.from_prefix(dut, "rx"),
        dut.clk,
        dut.rst,
        reset_active_level=True,
        byte_size=32,
    )


def _make_sink(dut) -> AxiStreamSink:
    return AxiStreamSink(
        AxiStreamBus.from_prefix(dut, "tx"),
        dut.clk,
        dut.rst,
        reset_active_level=True,
        byte_size=32,
    )


def _configure_payload(channel_count: int, sample_period: int) -> bytes:
    """CONFIGURE payload: word0 = channel_count, word1 = sample_period (LE)."""
    return struct.pack("<II", channel_count, sample_period)


async def _send(source: AxiStreamSource, msg_type: int, payload: bytes) -> None:
    beats = generate_packet(msg_type=msg_type, payload=payload)
    await source.send(AxiStreamFrame(tdata=beats))


def _data_words(payload: bytes) -> list[int]:
    """Unpack a DATA_FRAME payload into its 32-bit words."""
    assert len(payload) % 4 == 0, f"payload not word-aligned: {len(payload)} bytes"
    return list(struct.unpack(f"<{len(payload) // 4}I", payload))


@cocotb.test()
async def test_configure_and_stream(dut) -> None:
    """Configure 4 channels, start, and check the counter ramp across frames."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    channel_count = 4
    await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 20))
    await _send(source, MSG_START_STREAM, b"")

    expected = None
    for _ in range(5):
        frame = await sink.recv()
        msg_type, payload = parse_packet(list(frame.tdata))
        assert msg_type == MSG_DATA_FRAME, f"unexpected msg_type {msg_type:#06x}"

        words = _data_words(payload)
        assert len(words) == channel_count, (
            f"expected {channel_count} words, got {len(words)}"
        )

        # The payload is a single free-running counter: contiguous and monotonic
        # within a frame and across frames (low 16 bits are the per-channel sample).
        if expected is None:
            expected = words[0] & 0xFFFF
        for w in words:
            assert (w & 0xFFFF) == expected, (
                f"ramp break: got {w & 0xFFFF:#06x}, expected {expected:#06x}"
            )
            expected = (expected + 1) & 0xFFFF


@cocotb.test()
async def test_stop_halts_stream(dut) -> None:
    """After STOP_STREAM, no further DATA_FRAME should arrive."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    await _send(source, MSG_CONFIGURE, _configure_payload(2, 20))
    await _send(source, MSG_START_STREAM, b"")

    # Receive at least one frame so we know streaming is live.
    await sink.recv()

    await _send(source, MSG_STOP_STREAM, b"")

    # Drain whatever was already queued/in-flight, then assert quiet.
    await ClockCycles(dut.clk, 200)
    sink.clear()
    await ClockCycles(dut.clk, 500)
    assert sink.empty(), "DATA_FRAME received after STOP_STREAM"


@cocotb.test()
async def test_no_stream_before_start(dut) -> None:
    """CONFIGURE alone (no START_STREAM) must not produce any data."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    await _send(source, MSG_CONFIGURE, _configure_payload(8, 20))
    await ClockCycles(dut.clk, 500)
    assert sink.empty(), "DATA_FRAME received before START_STREAM"


_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


@pytest.mark.parametrize(
    "testcase",
    ["test_configure_and_stream", "test_stop_halts_stream", "test_no_stream_before_start"],
)
def test_runner(testcase: str) -> None:
    """pytest entrypoint — runs the cocotb suite under Questa/Verilator.

    # CUSTOMIZE: edit ``sources`` to add additional SV files. The list below
    # is seeded from ``peripheral.yaml`` at codegen time, but hand-edits here
    # are preserved across ``axon-peripheral-sdk generate``.
    """
    sources = [
        # SDK framework SV (axi4_stream_interface) the peripheral + tb ports
        # bind to — resolved from the repo (dev) or the staged .deb assets.
        *framework_sv_sources(),
        os.path.join(_PROJECT_ROOT, "src/axon_test_source_peripheral.sv"),
        os.path.join(_PROJECT_ROOT, "test", "tb", "axon_test_source_tb.sv"),
    ]
    cocotb_pytest_runner(
        sources=sources,
        toplevel="axon_test_source_tb",
        test_module=__name__,
        testcase=[testcase],
    )
