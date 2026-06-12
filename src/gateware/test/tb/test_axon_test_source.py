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


# Per-channel delay, in samples — must match DELAY in axon_test_source_peripheral.sv.
DELAY_SAMPLES = 8


def _sample16(word: int) -> int:
    """Interpret a payload word's low 16 bits as a signed sample."""
    s = word & 0xFFFF
    return s - 0x10000 if s & 0x8000 else s


@cocotb.test()
async def test_configure_and_stream(dut) -> None:
    """Configure 4 channels, start, and check we get well-formed, varying frames."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    channel_count = 4
    await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 20))
    await _send(source, MSG_START_STREAM, b"")

    ch0_samples = []
    for _ in range(40):
        frame = await sink.recv()
        msg_type, payload = parse_packet(list(frame.tdata))
        assert msg_type == MSG_DATA_FRAME, f"unexpected msg_type {msg_type:#06x}"

        words = _data_words(payload)
        assert len(words) == channel_count, (
            f"expected {channel_count} words, got {len(words)}"
        )
        ch0_samples.append(_sample16(words[0]))

    # The signal is a real LFP+spike synth, not a constant — channel 0 must move.
    assert len(set(ch0_samples)) > 1, f"channel 0 never varied: {ch0_samples}"


@cocotb.test()
async def test_per_channel_delay(dut) -> None:
    """Each channel is the same master signal delayed by DELAY_SAMPLES per channel.

    The synth is deterministic and one master sample is produced per frame, so
    channel 1 at frame f equals channel 0 at frame (f - DELAY_SAMPLES).
    """
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    channel_count = 4
    await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 20))
    await _send(source, MSG_START_STREAM, b"")

    frames = []
    for _ in range(60):
        frame = await sink.recv()
        _, payload = parse_packet(list(frame.tdata))
        frames.append([_sample16(w) for w in _data_words(payload)])

    # ch1[f] should equal ch0[f - DELAY_SAMPLES] once the delay line has filled.
    checked = 0
    for f in range(DELAY_SAMPLES, len(frames)):
        assert frames[f][1] == frames[f - DELAY_SAMPLES][0], (
            f"delay mismatch at frame {f}: ch1={frames[f][1]} != "
            f"ch0[f-{DELAY_SAMPLES}]={frames[f - DELAY_SAMPLES][0]}"
        )
        checked += 1
    assert checked > 0


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
    [
        "test_configure_and_stream",
        "test_per_channel_delay",
        "test_stop_halts_stream",
        "test_no_stream_before_start",
    ],
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
