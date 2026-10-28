"""cocotb tests for the axon_test_source peripheral.

START_STREAM and STOP_STREAM carry the recording nonce. DATA_FRAME carries
either zero-extended 16-bit samples or an exact three-word BEGIN/END marker.
These tests retain admitted data, check the ordered fences, and exercise the
same producer through the installed SDK encapsulator as well as its raw lane.

# CUSTOMIZE: this file is a starter, NOT auto-regenerated. Hand-edits are
# expected and preserved by ``axon-peripheral-sdk generate``.
"""
from __future__ import annotations

import os
import struct

import cocotb
import pytest
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, FallingEdge, ReadOnly, RisingEdge, with_timeout
from cocotb.utils import get_sim_time
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


# 80 MHz — nerv512u-devkit's clkmc. via-devkit runs clkmc at 40 MHz, but the
# peripheral is fully synchronous and every assertion below counts cycles
# rather than nanoseconds, so this value is cosmetic and the suite is shared
# unchanged across both target profiles.
CLK_PERIOD_NS = 12.5

# Message-type opcodes — must match axon_test_source_peripheral.sv.
MSG_CONFIGURE = 0x0052
MSG_START_STREAM = 0x0054
MSG_STOP_STREAM = 0x0055
MSG_DATA_FRAME = 0x0056

START_MAGIC = 0xA57A0001
STOP_MAGIC = 0xA57A0002
NONCE_A = 0x0123456789ABCDEF
NONCE_B = 0xFEDCBA9876543210
WIRE_SOURCE_ADDR = 0x12340100


async def _reset(dut) -> None:
    """Apply active-high reset away from the sampling edge."""
    await FallingEdge(dut.clk)
    dut.rst.value = 1
    await ClockCycles(dut.clk, 4)
    await FallingEdge(dut.clk)
    dut.rst.value = 0


async def _start_clock_and_reset(dut) -> None:
    dut.rst.value = 1
    dut.rx_tdata.value = 0
    dut.rx_tvalid.value = 0
    dut.rx_tlast.value = 0
    dut.tx_tready.value = 0
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)


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


def _nonce_payload(nonce: int) -> bytes:
    """Exactly two little-endian words, low word first."""
    return struct.pack("<Q", nonce)


def _foreign_nonces(nonce: int) -> tuple[int, int]:
    return nonce ^ (1 << 32), nonce ^ 1


def _long_stall_cycles(sample_period: int) -> int:
    """Public P+1 pacing-derived stress duration, not a private tick count."""
    return 2048 * (sample_period + 1)


def _malformed_controls(msg_type: int, nonce: int) -> list[list[int]]:
    """Wrong byte lengths and early/late TLAST boundaries for the current nonce."""
    low, high = nonce & 0xFFFFFFFF, nonce >> 32
    return [
        [msg_type << 16],
        [(msg_type << 16) | 4, low],
        [(msg_type << 16) | 8],
        [(msg_type << 16) | 8, low],
        [(msg_type << 16) | 8, low, high, 0x12345678],
        [(msg_type << 16) | 12, low, high, 0x12345678],
        [(msg_type << 16) | 7, low, high],
    ]


async def _send(source: AxiStreamSource, msg_type: int, payload: bytes) -> None:
    beats = generate_packet(msg_type=msg_type, payload=payload)
    await _send_beats(source, beats)


async def _send_beats(source: AxiStreamSource, beats: list[int]) -> None:
    """Send supplied command words, with TLAST on the final beat."""
    await source.send(AxiStreamFrame(tdata=beats))
    await with_timeout(source.wait(), 100, "us")


async def _recv_frame(sink: AxiStreamSink) -> AxiStreamFrame:
    return await with_timeout(sink.recv(), 100, "us")


async def _recv_packet(sink: AxiStreamSink) -> tuple[int, bytes]:
    frame = await _recv_frame(sink)
    return parse_packet(list(frame.tdata))


async def _data_cadence(sink, channel_count: int, after_time=None) -> tuple[int, int]:
    """Use actual handshake timestamps; validate, but do not measure, older queued DATA."""
    arrivals = []
    for _ in range(1024):
        frame = await _recv_frame(sink)
        _assert_data(parse_packet(list(frame.tdata)), channel_count)
        assert frame.sim_time_start is not None and frame.sim_time_end is not None
        if after_time is None or frame.sim_time_start > after_time:
            arrivals.append(frame.sim_time_end)
            if len(arrivals) == 4:
                return arrivals[2] - arrivals[1], arrivals[3] - arrivals[2]
    raise AssertionError("four post-command DATA frames did not arrive")


def _assert_marker(packet: tuple[int, bytes], magic: int, nonce: int) -> None:
    msg_type, payload = packet
    assert msg_type == MSG_DATA_FRAME, f"unexpected marker type {msg_type:#06x}"
    assert payload == struct.pack("<IQ", magic, nonce), (
        f"wrong marker: {payload.hex()}, expected magic {magic:#010x}, nonce {nonce:#018x}"
    )


def _assert_data(packet: tuple[int, bytes], channel_count: int) -> list[int]:
    msg_type, payload = packet
    assert msg_type == MSG_DATA_FRAME, f"unexpected DATA type {msg_type:#06x}"
    words = _data_words(payload)
    assert len(words) == channel_count, (
        f"expected {channel_count} sample words, got {len(words)}"
    )
    assert all(0 <= word <= 0xFFFF for word in words), (
        f"marker or non-zero-extended sample leaked into DATA: {words}"
    )
    return words


async def _begin(source, sink, nonce: int) -> None:
    await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
    _assert_marker(await _recv_packet(sink), START_MAGIC, nonce)


async def _finish(sink, nonce: int, channel_count: int) -> int:
    """Retain and validate every queued DATA frame before the matching END."""
    data_count = 0
    for _ in range(1024):
        packet = await _recv_packet(sink)
        if packet[1] == struct.pack("<IQ", STOP_MAGIC, nonce):
            _assert_marker(packet, STOP_MAGIC, nonce)
            return data_count
        _assert_data(packet, channel_count)
        data_count += 1
    raise AssertionError("matching END did not arrive within the packet bound")


async def _assert_no_output(dut, sink=None, cycles: int = 500) -> None:
    """Supplement a consumed END with an absence check; never use quiet as END."""
    if sink is not None:
        assert sink.empty(), "queued output remained after END"
    for _ in range(cycles):
        await FallingEdge(dut.clk)
        await ReadOnly()
        assert not int(dut.tx_tvalid.value), "output offered after the terminal boundary"
    if sink is not None:
        assert sink.empty(), "output arrived after END"


def _raw_beat(dut) -> tuple[int, ...]:
    return (
        int(dut.raw_tdata.value),
        int(dut.raw_tlast.value),
        int(dut.raw_tkeep.value),
        int(dut.raw_tid.value),
        int(dut.raw_tdest.value),
        int(dut.raw_tuser.value),
    )


async def _wait_offered(dut) -> tuple[int, ...]:
    for _ in range(10000):
        await FallingEdge(dut.clk)
        await ReadOnly()
        if int(dut.raw_tvalid.value):
            assert not int(dut.raw_tready.value), "the manual sink must own a stalled READY"
            return _raw_beat(dut)
    raise AssertionError("no producer beat offered within the cycle bound")


def _assert_held(dut, expected: tuple[int, ...]) -> None:
    assert int(dut.raw_tvalid.value), "VALID withdrawn before handshake"
    assert not int(dut.raw_tready.value), "unexpected handshake during the stall"
    assert _raw_beat(dut) == expected, "offered DATA/marker beat or sideband changed while stalled"


async def _hold_beat(dut, expected: tuple[int, ...], cycles: int) -> None:
    for _ in range(cycles):
        await FallingEdge(dut.clk)
        await ReadOnly()
        _assert_held(dut, expected)


async def _take_beat(dut, expected: tuple[int, ...] | None = None) -> tuple[int, int]:
    """Manual READY owner, used only by raw-lane directed stall cases."""
    for _ in range(10000):
        await FallingEdge(dut.clk)
        dut.tx_tready.value = 1
        await ReadOnly()
        if int(dut.tx_tvalid.value):
            if expected is not None:
                assert _raw_beat(dut) == expected, "stalled beat changed at its accepting handshake"
            offered = int(dut.tx_tdata.value), int(dut.tx_tlast.value)
            await RisingEdge(dut.clk)
            await FallingEdge(dut.clk)
            dut.tx_tready.value = 0
            return offered
    raise AssertionError("no accepted beat within the cycle bound")


async def _take_packet(dut, prefix=None, first_expected=None) -> tuple[int, bytes]:
    beats = list(prefix) if prefix is not None else []
    for index in range(1024):
        data, last = await _take_beat(dut, first_expected if index == 0 else None)
        beats.append(data)
        if last:
            return parse_packet(beats)
    raise AssertionError("producer packet did not terminate")


async def _drive_rx_beat(dut, data: int, last: bool) -> None:
    """Manual RX owner for incomplete commands and clock-phased STOP cases."""
    for _ in range(10000):
        await FallingEdge(dut.clk)
        dut.rx_tdata.value = data
        dut.rx_tlast.value = int(last)
        dut.rx_tvalid.value = 1
        await ReadOnly()
        if int(dut.rx_tready.value):
            await RisingEdge(dut.clk)
            await FallingEdge(dut.clk)
            dut.rx_tvalid.value = 0
            dut.rx_tlast.value = 0
            return
    raise AssertionError("command beat was not accepted")


async def _drive_packet(dut, msg_type: int, payload: bytes) -> None:
    beats = generate_packet(msg_type=msg_type, payload=payload)
    for index, beat in enumerate(beats):
        await _drive_rx_beat(dut, beat, index == len(beats) - 1)


async def _tap_frames(dut, frames: list[list[int]]) -> None:
    """Read-only producer monitor; it never drives either READY signal."""
    partial = []
    held = None
    while True:
        await FallingEdge(dut.clk)
        await ReadOnly()
        reset = int(dut.rst.value)
        valid = int(dut.raw_tvalid.value) if not reset else 0
        ready = int(dut.raw_tready.value) if not reset else 0
        beat = _raw_beat(dut) if valid else None
        if held is not None and not reset:
            assert valid and beat == held, "producer beat changed before the encap handshake"
        held = beat if valid and not ready else None
        await RisingEdge(dut.clk)
        if reset:
            partial = []
        elif valid and ready:
            assert beat is not None
            partial.append(beat[0])
            if beat[1]:
                frames.append(partial)
                partial = []


def _parse_wire(beats: list[int]) -> tuple[int, tuple[int, bytes]]:
    """Decode actual encap output: five Axon words, type/length, payload, trailer."""
    assert len(beats) >= 7, f"short full Axon packet: {beats}"
    assert beats[0] == 0xC0FFEE00, "wrong Axon sentinel"
    assert beats[1] == WIRE_SOURCE_ADDR, "wrong original Axon source address"
    assert beats[2] == 0, "wrong Axon destination address"
    assert beats[4] == 0, "nonzero reserved Axon header word"
    # The trailer is present, but this test does not claim CRC validation.
    packet = parse_packet(beats[5:-1])
    return beats[3], packet


def _data_words(payload: bytes) -> list[int]:
    """Unpack a DATA_FRAME payload into its 32-bit words."""
    assert len(payload) % 4 == 0, f"payload not word-aligned: {len(payload)} bytes"
    return list(struct.unpack(f"<{len(payload) // 4}I", payload))


# Existing observable per-channel delay expectations, in samples. Channel 0
# is the live reference; these are not assertions about a RAM/LUT implementation.
DELAY_LUT = [0, 107, 188, 3, 210, 212, 206, 77]


def _sample16(word: int) -> int:
    """Interpret a payload word's low 16 bits as a signed sample."""
    s = word & 0xFFFF
    return s - 0x10000 if s & 0x8000 else s


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_configure_and_stream(dut) -> None:
    """Configure 4 channels, start, and check we get well-formed, varying frames."""
    await _start_clock_and_reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    channel_count = 4
    await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 20))
    await _begin(source, sink, NONCE_A)

    ch0_samples = []
    for _ in range(40):
        words = _assert_data(await _recv_packet(sink), channel_count)
        ch0_samples.append(_sample16(words[0]))

    # The signal is a real LFP+spike synth, not a constant — channel 0 must move.
    assert len(set(ch0_samples)) > 1, f"channel 0 never varied: {ch0_samples}"
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _finish(sink, NONCE_A, channel_count)
    await _assert_no_output(dut, sink)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_per_channel_delay(dut) -> None:
    """Each channel is the same master signal delayed by its expected offset.

    The synth (LFP + spikes + noise) is deterministic and one master sample is
    produced per frame, so channel c at frame f equals channel 0 at frame
    (f - DELAY_LUT[c]). Offsets are pseudo-random, not linear in c.
    """
    await _start_clock_and_reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    channels = [1, 2, 3]                       # check these against channel 0
    channel_count = max(channels) + 1
    max_off = max(DELAY_LUT[c] for c in channels)

    await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 20))
    await _begin(source, sink, NONCE_A)

    frames = []
    for _ in range(max_off + 40):              # enough for the deepest offset to fill
        words = _assert_data(await _recv_packet(sink), channel_count)
        frames.append([_sample16(w) for w in words])

    checked = 0
    for c in channels:
        off = DELAY_LUT[c]
        for f in range(off, len(frames)):
            assert frames[f][c] == frames[f - off][0], (
                f"delay mismatch ch{c} (offset {off}) frame {f}: "
                f"{frames[f][c]} != ch0[f-{off}]={frames[f - off][0]}"
            )
            checked += 1
    assert checked > 0

    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _finish(sink, NONCE_A, channel_count)
    await _assert_no_output(dut, sink)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_stop_halts_stream(dut) -> None:
    """The already-accepted software sink queue is consumed before matching END."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    sink = _make_sink(dut)

    await _send(source, MSG_CONFIGURE, _configure_payload(2, 20))
    await _begin(source, sink, NONCE_A)
    _assert_data(await _recv_packet(sink), 2)

    await ClockCycles(dut.clk, 200)
    assert not sink.empty(), "test did not build a queued DATA tail"
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    assert await _finish(sink, NONCE_A, 2) > 0, "already-accepted sink DATA was not drained before END"
    await _assert_no_output(dut, sink)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_no_stream_before_start(dut) -> None:
    """CONFIGURE alone (no START_STREAM) must not produce any data."""
    await _start_clock_and_reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    await _send(source, MSG_CONFIGURE, _configure_payload(8, 20))
    await _assert_no_output(dut, sink)


async def _finish_manual(dut, nonce: int, channel_count: int) -> int:
    data_count = 0
    for _ in range(16):
        packet = await _take_packet(dut)
        if packet[1] == struct.pack("<IQ", STOP_MAGIC, nonce):
            _assert_marker(packet, STOP_MAGIC, nonce)
            return data_count
        _assert_data(packet, channel_count)
        data_count += 1
    raise AssertionError("matching END did not follow the admitted DATA")


async def _stop_monitor(monitor) -> None:
    if monitor.done():
        await monitor
    else:
        monitor.cancel()


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_begin_precedes_data(dut) -> None:
    """Even a stalled final BEGIN word must be accepted before any new DATA."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
    await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_A))

    prefix = []
    for _ in range(3):
        data, last = await _take_beat(dut)
        assert not last, "BEGIN terminated before its complete nonce"
        prefix.append(data)
    offered = await _wait_offered(dut)
    assert offered[:2] == (NONCE_A >> 32, 1), "BEGIN's final nonce word was not offered"
    await _hold_beat(dut, offered, 64)
    _assert_marker(await _take_packet(dut, prefix, offered), START_MAGIC, NONCE_A)
    _assert_data(await _take_packet(dut), 3)

    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _finish_manual(dut, NONCE_A, 3)
    await _assert_no_output(dut)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_stop_under_backpressure(dut) -> None:
    """Prolonged header, payload and TLAST stalls complete the admitted frame before END."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    channel_count = 8
    for stalled_index in (0, 1, channel_count):
        nonce = NONCE_A + stalled_index
        await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        _assert_marker(await _take_packet(dut), START_MAGIC, nonce)

        prefix = []
        for _ in range(stalled_index):
            data, last = await _take_beat(dut)
            assert not last, "admitted DATA terminated before the selected stall"
            prefix.append(data)
        offered = await _wait_offered(dut)
        assert offered[1] == int(stalled_index == channel_count)
        if stalled_index == 0:
            assert offered[0] == (MSG_DATA_FRAME << 16) | (4 * channel_count)
        else:
            assert offered[0] <= 0xFFFF, "stalled sample was a marker"

        long_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(1)))
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
        await long_hold

        command_hold = cocotb.start_soon(_hold_beat(dut, offered, 128))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await command_hold
        _assert_data(await _take_packet(dut, prefix, offered), channel_count)
        _assert_marker(await _take_packet(dut), STOP_MAGIC, nonce)
        await _assert_no_output(dut)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_stop_between_frames(dut) -> None:
    """A matching STOP in the inter-frame gap emits END without another DATA."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 1000))
    await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_A))
    _assert_marker(await _take_packet(dut), START_MAGIC, NONCE_A)
    _assert_data(await _take_packet(dut), 3)
    await _assert_no_output(dut, cycles=8)
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    _assert_marker(await _take_packet(dut), STOP_MAGIC, NONCE_A)
    await _assert_no_output(dut)


@cocotb.test(timeout_time=2, timeout_unit="ms")
async def test_stop_across_sample_period_phases(dut) -> None:
    """Sweep complete STOP across a public sample-period window, retaining admitted DATA."""
    await _start_clock_and_reset(dut)
    sample_period = 31
    for phase in range(sample_period + 2):
        nonce = NONCE_A + phase
        await _drive_packet(dut, MSG_CONFIGURE, _configure_payload(3, sample_period))
        await _drive_packet(dut, MSG_START_STREAM, _nonce_payload(nonce))
        _assert_marker(await _take_packet(dut), START_MAGIC, nonce)
        _assert_data(await _take_packet(dut), 3)
        await ClockCycles(dut.clk, phase + 1)
        await _drive_packet(dut, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _finish_manual(dut, nonce, 3)
        await _assert_no_output(dut, cycles=64)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_control_waits_for_complete_nonce(dut) -> None:
    """Neither START nor STOP acts on a header or a partial nonce alone."""
    await _start_clock_and_reset(dut)
    await _drive_packet(dut, MSG_CONFIGURE, _configure_payload(3, 31))
    await _drive_rx_beat(dut, (MSG_START_STREAM << 16) | 8, False)
    await _drive_rx_beat(dut, NONCE_A & 0xFFFFFFFF, False)
    await _assert_no_output(dut)
    await _drive_rx_beat(dut, NONCE_A >> 32, True)
    _assert_marker(await _take_packet(dut), START_MAGIC, NONCE_A)
    _assert_data(await _take_packet(dut), 3)

    await _drive_rx_beat(dut, (MSG_STOP_STREAM << 16) | 8, False)
    await _drive_rx_beat(dut, NONCE_A & 0xFFFFFFFF, False)
    for _ in range(3):
        _assert_data(await _take_packet(dut), 3)
    await _drive_rx_beat(dut, NONCE_A >> 32, True)
    await _finish_manual(dut, NONCE_A, 3)
    await _assert_no_output(dut)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_malformed_control_cannot_complete_epoch(dut) -> None:
    """Empty, short, long and byte-misaligned controls cannot start/stop an epoch."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    sink = _make_sink(dut)
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))

    for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
        for beats in _malformed_controls(opcode, NONCE_A):
            await _send_beats(source, beats)
            await _assert_no_output(dut, sink, cycles=32)

    observed = []
    monitor = cocotb.start_soon(_tap_frames(dut, observed))
    await _begin(source, sink, NONCE_A)
    _assert_data(await _recv_packet(sink), 3)
    for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
        for beats in _malformed_controls(opcode, NONCE_A):
            before = len(observed)
            await _send_beats(source, beats)
            await ClockCycles(dut.clk, 32)
            await FallingEdge(dut.clk)
            await ReadOnly()
            assert len(observed) > before, "malformed control halted live DATA"
            for frame in observed[before:]:
                _assert_data(parse_packet(frame), 3)

    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _finish(sink, NONCE_A, 3)
    await _assert_no_output(dut, sink)
    await _stop_monitor(monitor)


@cocotb.test(timeout_time=1, timeout_unit="ms")
async def test_wrong_and_stale_nonce_are_ignored(dut) -> None:
    """Retired or foreign nonce commands cannot terminate or replace an active epoch."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _assert_no_output(dut, cycles=64)
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
    await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_A))
    _assert_marker(await _take_packet(dut), START_MAGIC, NONCE_A)
    _assert_data(await _take_packet(dut), 3)
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _finish_manual(dut, NONCE_A, 3)

    await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
    _assert_marker(await _take_packet(dut), START_MAGIC, NONCE_B)
    for opcode, payload in (
        (MSG_START_STREAM, _nonce_payload(NONCE_B)),
        (MSG_START_STREAM, _nonce_payload(NONCE_A)),
        (MSG_STOP_STREAM, _nonce_payload(NONCE_A)),
        (MSG_CONFIGURE, _configure_payload(256, 1)),
    ):
        await _send(source, opcode, payload)
        for _ in range(3):
            _assert_data(await _take_packet(dut), 3)
    for foreign_nonce in _foreign_nonces(NONCE_B):
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            await _send(source, opcode, _nonce_payload(foreign_nonce))
            for _ in range(3):
                _assert_data(await _take_packet(dut), 3)
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
    await _finish_manual(dut, NONCE_B, 3)
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
    await _assert_no_output(dut)


@cocotb.test(timeout_time=2, timeout_unit="ms")
async def test_pending_begin_keeps_nonce_and_order(dut) -> None:
    """Pending BEGIN beats retain their nonce; STOP yields BEGIN then END, no DATA."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    for stalled_index in range(4):
        nonce = NONCE_A + stalled_index
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        prefix = []
        for _ in range(stalled_index):
            data, last = await _take_beat(dut)
            assert not last
            prefix.append(data)
        offered = await _wait_offered(dut)

        long_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(1)))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_CONFIGURE, _configure_payload(256, 999))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
        for foreign_nonce in _foreign_nonces(nonce):
            await _send(source, MSG_STOP_STREAM, _nonce_payload(foreign_nonce))
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            for beats in _malformed_controls(opcode, nonce):
                await _send_beats(source, beats)
        await long_hold

        command_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(1)))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_CONFIGURE, _configure_payload(1, 1))
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            for beats in _malformed_controls(opcode, nonce):
                await _send_beats(source, beats)
        await command_hold
        _assert_marker(await _take_packet(dut, prefix, offered), START_MAGIC, nonce)
        _assert_marker(await _take_packet(dut), STOP_MAGIC, nonce)
        await _assert_no_output(dut, cycles=64)

    # Compare free-flow pacing across rejected pending/active CONFIGURE commands.
    await FallingEdge(dut.clk)
    sink = _make_sink(dut)
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 31))
    await _begin(source, sink, NONCE_B)
    cadence = await _data_cadence(sink, 3)
    assert cadence[0] == cadence[1] and cadence[0] > 0, "baseline DATA cadence was not steady"
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
    await _finish(sink, NONCE_B, 3)
    await _assert_no_output(dut, sink, cycles=64)

    await FallingEdge(dut.clk)
    sink.pause = True
    nonce = NONCE_B + 1
    await _send(source, MSG_CONFIGURE, _configure_payload(3, 31))
    await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
    offered = await _wait_offered(dut)
    command_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(31)))
    await _send(source, MSG_CONFIGURE, _configure_payload(256, 999))
    await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_A))
    for foreign_nonce in _foreign_nonces(nonce):
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            await _send(source, opcode, _nonce_payload(foreign_nonce))
    for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
        for beats in _malformed_controls(opcode, nonce):
            await _send_beats(source, beats)
    await command_hold
    await FallingEdge(dut.clk)
    sink.pause = False
    _assert_marker(await _recv_packet(sink), START_MAGIC, nonce)
    assert await _data_cadence(sink, 3, after_time=get_sim_time()) == cadence, (
        "pending CONFIGURE changed active sample period"
    )

    await _send(source, MSG_CONFIGURE, _configure_payload(256, 998))
    assert await _data_cadence(sink, 3, after_time=get_sim_time()) == cadence, (
        "streaming CONFIGURE changed active sample period"
    )
    await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
    await _finish(sink, nonce, 3)
    await _assert_no_output(dut, sink)


@cocotb.test(timeout_time=2, timeout_unit="ms")
async def test_pending_end_keeps_nonce_and_frame(dut) -> None:
    """All pending END beats and the preceding admitted DATA survive conflicting controls."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    for stalled_index in range(4):
        nonce = NONCE_A + stalled_index
        await _send(source, MSG_CONFIGURE, _configure_payload(8, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        _assert_marker(await _take_packet(dut), START_MAGIC, nonce)
        header, last = await _take_beat(dut)
        assert not last
        offered = await _wait_offered(dut)

        command_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(1)))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 999))
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            for beats in _malformed_controls(opcode, nonce):
                await _send_beats(source, beats)
        await command_hold
        _assert_data(await _take_packet(dut, [header], offered), 8)

        prefix = []
        for _ in range(stalled_index):
            data, last = await _take_beat(dut)
            assert not last
            prefix.append(data)
        offered = await _wait_offered(dut)
        marker_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(1)))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        await _send(source, MSG_START_STREAM, _nonce_payload(NONCE_B))
        await _send(source, MSG_CONFIGURE, _configure_payload(256, 998))
        for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
            for beats in _malformed_controls(opcode, nonce):
                await _send_beats(source, beats)
        await marker_hold
        _assert_marker(await _take_packet(dut, prefix, offered), STOP_MAGIC, nonce)
        await _assert_no_output(dut, cycles=64)

    await FallingEdge(dut.clk)
    sink = _make_sink(dut)
    await _send(source, MSG_CONFIGURE, _configure_payload(8, 31))
    await _begin(source, sink, NONCE_B)
    cadence = await _data_cadence(sink, 8)
    assert cadence[0] == cadence[1] and cadence[0] > 0, "baseline DATA cadence was not steady"

    async def pause_on_end():
        for _ in range(10000):
            await FallingEdge(dut.clk)
            if int(dut.raw_tvalid.value) and int(dut.raw_tdata.value) == (MSG_DATA_FRAME << 16) | 12:
                sink.pause = True
                for _ in range(64):
                    await FallingEdge(dut.clk)
                    await ReadOnly()
                    if int(dut.raw_tvalid.value) and not int(dut.raw_tready.value):
                        return _raw_beat(dut)
                raise AssertionError("END completed before the sink pause took effect")
        raise AssertionError("END was not offered to the public sink")

    pause_task = cocotb.start_soon(pause_on_end())
    await _send(source, MSG_STOP_STREAM, _nonce_payload(NONCE_B))
    offered = await pause_task
    marker_hold = cocotb.start_soon(_hold_beat(dut, offered, _long_stall_cycles(31)))
    await _send(source, MSG_CONFIGURE, _configure_payload(256, 998))
    for foreign_nonce in _foreign_nonces(NONCE_B):
        await _send(source, MSG_STOP_STREAM, _nonce_payload(foreign_nonce))
    for opcode in (MSG_START_STREAM, MSG_STOP_STREAM):
        for beats in _malformed_controls(opcode, NONCE_B):
            await _send_beats(source, beats)
    await marker_hold
    await FallingEdge(dut.clk)
    sink.pause = False
    await _finish(sink, NONCE_B, 8)
    await _assert_no_output(dut, sink, cycles=64)

    nonce = NONCE_B + 1
    await _begin(source, sink, nonce)
    assert await _data_cadence(sink, 8, after_time=get_sim_time()) == cadence, (
        "pending END CONFIGURE changed the retained sample period"
    )
    await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
    await _finish(sink, nonce, 8)
    await _assert_no_output(dut, sink)


@cocotb.test(timeout_time=2, timeout_unit="ms")
async def test_reset_invalidates_epoch(dut) -> None:
    """Reset during partial BEGIN, DATA or END invalidates, rather than completing, the epoch."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    for index, phase in enumerate(("begin", "data", "end")):
        nonce = NONCE_A + index
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        if phase == "begin":
            _, last = await _take_beat(dut)
            assert not last
        else:
            _assert_marker(await _take_packet(dut), START_MAGIC, nonce)
            if phase == "data":
                _, last = await _take_beat(dut)
                assert not last
            else:
                _assert_data(await _take_packet(dut), 3)
                await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
                for _ in range(4):
                    header, last = await _take_beat(dut)
                    assert not last
                    first_word, last = await _take_beat(dut)
                    assert not last
                    if first_word == STOP_MAGIC:
                        break
                    _assert_data(await _take_packet(dut, [header, first_word]), 3)
                else:
                    raise AssertionError("pending END was never offered before reset")
        await _wait_offered(dut)
        await _reset(dut)
        await _assert_no_output(dut, cycles=64)
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _assert_no_output(dut, cycles=64)

        recovery_nonce = NONCE_B + index
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(recovery_nonce))
        _assert_marker(await _take_packet(dut), START_MAGIC, recovery_nonce)
        _assert_data(await _take_packet(dut), 3)
        await _send(source, MSG_STOP_STREAM, _nonce_payload(recovery_nonce))
        await _finish_manual(dut, recovery_nonce, 3)
        await _assert_no_output(dut, cycles=64)


@cocotb.test(timeout_time=10, timeout_unit="ms")
async def test_marker_data_separation(dut) -> None:
    """Every legal 1..256-channel DATA shape, including three words, stays sample-only."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    sink = _make_sink(dut)
    for channel_count in range(1, 257):
        nonce = (channel_count << 32) | (0 if channel_count == 3 else channel_count)
        await _send(source, MSG_CONFIGURE, _configure_payload(channel_count, 1))
        await _begin(source, sink, nonce)
        for _ in range(2):
            _assert_data(await _recv_packet(sink), channel_count)
        await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
        await _finish(sink, nonce, channel_count)
        await _assert_no_output(dut, sink, cycles=8)


@cocotb.test(timeout_time=2, timeout_unit="ms")
async def test_encapsulation_preserves_order_and_sequence(dut) -> None:
    """Actual installed encap/FIFO output preserves both fences, tails and original sequence."""
    await _start_clock_and_reset(dut)
    source = _make_source(dut)
    sink = _make_sink(dut)
    sink.pause = True
    raw_frames = []
    wire_frames = []
    monitor = cocotb.start_soon(_tap_frames(dut, raw_frames))
    previous_sequence = None

    for record_index, nonce in enumerate((NONCE_A, NONCE_B)):
        await _send(source, MSG_CONFIGURE, _configure_payload(3, 1))
        await _send(source, MSG_START_STREAM, _nonce_payload(nonce))
        stop_sent = False
        if record_index == 0:
            stalled_cycles = 0
            for _ in range(10000):
                await FallingEdge(dut.clk)
                await ReadOnly()
                if len(raw_frames) >= 2 and int(dut.raw_tvalid.value) and not int(dut.raw_tready.value):
                    stalled_cycles += 1
                    if stalled_cycles >= 64:
                        break
                else:
                    stalled_cycles = 0
            else:
                raise AssertionError("real encap FIFO did not exert sustained producer backpressure")
            _assert_marker(parse_packet(raw_frames[0]), START_MAGIC, nonce)
            offered = _raw_beat(dut)
            await _hold_beat(dut, offered, _long_stall_cycles(1))
            await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
            stop_sent = True
            assert sink.empty(), "wire sink accepted output while paused"
            await FallingEdge(dut.clk)
            sink.pause = False

        begin_seen = False
        data_count = 0
        for _ in range(1024):
            frame = await _recv_frame(sink)
            wire = list(frame.tdata)
            sequence, packet = _parse_wire(wire)
            if previous_sequence is not None:
                assert sequence == (previous_sequence + 1) & 0xFFFFFFFF, "original wire sequence gap/reordering"
            previous_sequence = sequence
            wire_frames.append(wire)
            if not begin_seen:
                _assert_marker(packet, START_MAGIC, nonce)
                begin_seen = True
            elif packet[1] == struct.pack("<IQ", STOP_MAGIC, nonce):
                _assert_marker(packet, STOP_MAGIC, nonce)
                assert stop_sent and data_count > 0, "terminal did not follow a live stopped recording"
                break
            else:
                _assert_data(packet, 3)
                data_count += 1
                if not stop_sent:
                    await _send(source, MSG_STOP_STREAM, _nonce_payload(nonce))
                    stop_sent = True
        else:
            raise AssertionError("matching END was absent from real encap output")
        await _assert_no_output(dut, sink, cycles=64)

    assert len(wire_frames) == len(raw_frames), "producer/encap packet loss or duplication"
    for raw, wire in zip(raw_frames, wire_frames):
        assert wire[5:-1] == raw, "real encap changed/reordered an accepted producer frame"
    await _stop_monitor(monitor)


# src/gateware/ — the shared root above the per-profile project dirs
# (via-devkit/, nerv512u-devkit/). The peripheral RTL and this testbench are
# profile-agnostic and live here once; only the generated top/seed/transport
# files differ per profile, and the sim never compiles those.
_GATEWARE_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


_RAW_CASES = [
    "test_configure_and_stream",
    "test_per_channel_delay",
    "test_stop_halts_stream",
    "test_no_stream_before_start",
    "test_begin_precedes_data",
    "test_stop_under_backpressure",
    "test_stop_between_frames",
    "test_stop_across_sample_period_phases",
    "test_control_waits_for_complete_nonce",
    "test_malformed_control_cannot_complete_epoch",
    "test_wrong_and_stale_nonce_are_ignored",
    "test_pending_begin_keeps_nonce_and_order",
    "test_pending_end_keeps_nonce_and_frame",
    "test_reset_invalidates_epoch",
    "test_marker_data_separation",
]
_WIRE_CASE = "test_encapsulation_preserves_order_and_sequence"
_WIRE_PROFILES = ("nerv512u-devkit", "via-devkit")


@pytest.mark.parametrize(
    "testcase,profile",
    [
        *[pytest.param(case, None, id=case) for case in _RAW_CASES],
        *[
            pytest.param(_WIRE_CASE, profile, id=f"{_WIRE_CASE}-{profile}")
            for profile in _WIRE_PROFILES
        ],
    ],
)
def test_runner(testcase: str, profile: str | None) -> None:
    """Explicit SIM selection; wire cases require real encrypted SDK sources.

    # CUSTOMIZE: the sources below are maintained with the existing wrapper,
    # not regenerated by ``axon-peripheral-sdk generate``.
    """
    sim = os.environ.get("SIM", "questa").lower().strip()
    if profile is not None and sim != "questa":
        raise ValueError("real encrypted encapsulation requires SIM=questa; no mock fallback")
    sources = [*framework_sv_sources()]
    if profile is not None:
        asset_root = os.environ.get(
            "AXON_PERIPHERAL_SDK_ASSET_ROOT",
            str(resolve_install_path(f"products/{profile}/build-driver/assets")),
        )
        sources.extend(
            [
                os.path.join(
                    asset_root,
                    "shared/gateware/cores/alexforencich/verilog-axis/rtl/axis_fifo.v",
                ),
                str(resolve_install_path(f"profiles/{profile}/encap.enc.v")),
            ]
        )
    sources.extend(
        [
            os.path.join(_GATEWARE_ROOT, "axon_test_source_peripheral.sv"),
            os.path.join(_GATEWARE_ROOT, "test", "tb", "axon_test_source_tb.sv"),
        ]
    )
    build_root = os.environ.get(
        "AXON_TEST_BUILD_ROOT", os.path.join(os.path.dirname(__file__), "sim_build")
    )
    cocotb_pytest_runner(
        sources=sources,
        sim=sim,
        build_dir=os.path.join(build_root, sim, profile or "raw", testcase),
        params={
            "ENCAPSULATE": "1" if profile is not None else "0",
            "PERIPH_ADDR": str(WIRE_SOURCE_ADDR if profile is not None else 0),
        },
        waves=os.environ.get("WAVES", "0"),
        toplevel="axon_test_source_tb",
        test_module=__name__,
        testcase=[testcase],
    )
