"""cocotb tests for the axon_test_source peripheral.

Day-1 loopback verification — the template DUT echoes every word it
receives. Replace the assertions in ``test_loopback`` and ``test_random``
with peripheral-specific checks as you build out your RTL.

# CUSTOMIZE: this file is a starter, NOT auto-regenerated. Hand-edits are
# expected and preserved by ``axon-peripheral-sdk generate``.
"""
from __future__ import annotations

import os

import cocotb
import pytest
import vsc
from cocotb.clock import Clock
from cocotb.triggers import Timer
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


@cocotb.test()
async def test_loopback(dut) -> None:
    """Day-1 loopback: send one frame, expect the same frame back."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    # CUSTOMIZE: replace with the request the real peripheral expects.
    payload = b"\xDE\xAD\xBE\xEF"
    beats = generate_packet(msg_type=0x0001, payload=payload)
    await source.send(AxiStreamFrame(tdata=beats))

    reply = await sink.recv()
    msg_type, reply_payload = parse_packet(list(reply.tdata))

    # Day-1 loopback DUT does not rewrite msg_type — it echoes the request
    # word-for-word.
    assert msg_type == 0x0001, f"unexpected msg_type {msg_type:#06x}"
    assert reply_payload == b"\xDE\xAD\xBE\xEF", (
        f"payload mismatch: got {reply_payload!r}"
    )


@vsc.randobj
class _RandPayload:
    def __init__(self):
        super().__init__()
        self.p0 = vsc.rand_bit_t(32)


@cocotb.test()
async def test_random(dut) -> None:
    """Smoke-fuzz the loopback with pyvsc-randomised payloads."""
    cocotb.start_soon(Clock(dut.clk, CLK_PERIOD_NS, unit="ns").start())
    await _reset(dut)

    source = _make_source(dut)
    sink = _make_sink(dut)

    rng = _RandPayload()
    for _ in range(8):
        rng.randomize()
        payload = int(rng.p0).to_bytes(4, "little")
        beats = generate_packet(msg_type=0x0001, payload=payload)
        await source.send(AxiStreamFrame(tdata=beats))

        reply = await sink.recv()
        msg_type, reply_payload = parse_packet(list(reply.tdata))

        assert msg_type == 0x0001, f"unexpected msg_type {msg_type:#06x}"
        assert reply_payload == payload, (
            f"payload mismatch: sent {payload!r}, got {reply_payload!r}"
        )


_PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


@pytest.mark.parametrize("testcase", ["test_loopback", "test_random"])
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
