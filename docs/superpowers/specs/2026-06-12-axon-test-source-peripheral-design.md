# axon_test_source — design

**Date:** 2026-06-12
**Status:** approved-pending-review

## Goal

Turn the `axon-peripheral-example` (currently an Intan RHD2132 recording front-end
plus a day-1 loopback gateware stub) into a **dummy data source** peripheral named
`axon_test_source`: configure it with a channel count and a sample rate, and it
streams frames of synthetic incrementing-counter data, one frame per sample period.

This is the *throughput-tester idea* (`axon_source.sv` in the electronics repo:
a gateware-side packet generator paced by a timer) rebuilt to the Axon Peripheral
SDK contract, reusing the example's `RecordPlugin` software shape so it slots into
the normal recording pipeline.

Inspiration: `axon_source.sv` (the gateware generator) and headstage's
`src/axon/axon_source_peripheral.{h,cpp}` (`AxonTestPeripheral` — the existing
host-side driver for that gateware). We do **not** depend on the SDK internals or
the Axon controller; we port the *behavior* of those references onto the example's
SDK `RecordPlugin` framework.

### What the headstage reference confirms / contributes

`AxonTestPeripheral` (headstage) is an in-server `axon::RecordPeripheral` that talks
raw ZMQ + Axon protocol. We're building an SDK `RecordPluginWithLimits` plugin (the
example's framework), so we borrow its *semantics*, not its API:

- Opcodes match this spec exactly: `CONFIGURE 0x52`, `START_STREAM 0x54`,
  `STOP_STREAM 0x55`, `DATA_FRAME 0x56` (also `SET_TDEST 0x51`, `CONFIGURE_RESP 0x53`
  exist there; we don't need them).
- Samples are 16-bit; a frame carries `nchannels` samples (`packet_length =
  nchannels * 2` bytes).
- `to_proto()` advertises `Peripheral_Type_kBroadbandSource`.
- Trivial overrides: `get_lsb → 1.0`, `get_impedance → UNSUPPORTED`,
  `validate_* → nullopt`, `configure_bit_width/configure_channels → OK`, `self_test`
  stub. We mirror this.
- Drop detection / timestamping in headstage lives in its own `read_frames`. In the
  SDK model `read_frames` is the SDK's job and we only implement
  `parse_frame_payload`, so we do **not** reimplement that — keeps us simpler.

**Deliberate divergence:** headstage's `set_config` sends `(throughput<<16 |
packet_length)` in one word and waits for `CONFIGURE_RESP`. We instead send
`(channel_count, sample_period)` fire-and-forget (no response frame). Rationale: we
write both the gateware and the software here, so they only need to agree with each
other, and dropping the handshake removes an FSM round-trip. The software still
derives everything from the user-facing `(sample_rate, channels)` the way headstage
derives throughput/packet_length from them.

## Why this is simpler than `axon_source.sv`

`axon_source.sv` is a standalone peripheral wired directly to the Axon switch, so it
re-implements the entire Axon packet protocol (magic header, src/dst addr, seq num,
CRC, the CONFIGURE handshake). In the SDK model the transport (`encap`/`decap` in
`via_top.sv`) does all of that. Our peripheral only sees the **SDK frame format**:

```
word 0:  {msg_type[15:0], len[15:0]}     <- header (len = payload bytes)
word 1..N: payload words                 <- little-endian, tlast on final word
```

So we keep the timer-paced generator idea and drop all the framing.

## Design decisions (locked)

- **Name:** `axon_test_source`. Module `axon_test_source_peripheral_top`.
- **Software base:** keep `RecordPluginWithLimits` (same override surface as the
  example), bodies gutted to dummy behavior. Integrates as a normal record source.
- **Data pattern:** a single free-running 32-bit counter incremented once per
  emitted payload word (monotonic across the whole stream). The low 16 bits are the
  per-channel sample value, matching the example's `0x0000_RRRR` 16-bit sample shape.
  A monotonic ramp lets the host verify continuity / detect dropped frames.
- **Pacing:** configurable sample rate. Software sends a `sample_period` in clk
  ticks; gateware emits one frame of `channel_count` words per period via a
  down-counter.
- **Sample width:** 16-bit only (same as the example).

## Message protocol (peripheral-local `msg_type` opcodes)

Mirrors `axon_source.sv`'s opcodes for continuity:

| Name           | Value    | Dir          | Payload                                  |
| -------------- | -------- | ------------ | ---------------------------------------- |
| `CONFIGURE`    | `0x0052` | host→periph  | word0 = `channel_count`, word1 = `sample_period` (clk ticks). len=8 |
| `START_STREAM` | `0x0054` | host→periph  | none (len=0)                             |
| `STOP_STREAM`  | `0x0055` | host→periph  | none (len=0)                             |
| `DATA_FRAME`   | `0x0056` | periph→host  | `channel_count` words of counter data    |

`CONFIGURE` is fire-and-forget (no response frame), like the example's
`SET_SAMPLE_PERIOD`. Defined once in `peripheral.yaml` `api.msg_types` and once in a
software constants header (`axon_test_source_constants.h`).

## Gateware: `src/gateware/src/axon_test_source_peripheral.sv`

Replaces the combinational loopback. Keeps the exact SDK port contract
(`clk`, `rst`, `periph_addr`, `rx_axis.secondary`, `tx_axis.main`).

**RX FSM** (consume command frames):
- Read header word → `msg_type`, `len`.
- `CONFIGURE` → read 2 payload words into `channel_count`, `sample_period`.
- `START_STREAM` → `stream_en <= 1`. `STOP_STREAM` → `stream_en <= 0`.
- Always assert `rx_axis.tready` enough to drain frames (commands are small).

**TX FSM** (produce data frames):
- A down-counter loaded from `sample_period`; when it hits 0 and `stream_en` and
  `channel_count > 0`, start a frame.
- Emit header `{DATA_FRAME, channel_count*4}`, then `channel_count` words, each =
  `counter` (counter increments every accepted word). `tlast` on the final word.
- Respect `tx_axis.tready` (stall if downstream not ready).
- Reload the period counter after each frame.

**Reset:** synchronous active-high `rst` clears `stream_en`, counters, FSM state.

Notes:
- `counter` is 32-bit, wraps naturally; host reads low 16 bits per sample.
- No external IO, no `axis_fifo`/`divider_timer` dependencies (those were needed by
  `axon_source` only because it owned the full protocol). Pure FSM + counters keeps
  it self-contained and synthesizable with just the SDK framework SV.

### Clock-frequency assumption

The peripheral is clocked by `clkmc` in `via_top.sv`, where
`CLKMC_FREQ = CLK0_FREQ/4 = 160 MHz/4 = 40 MHz`. (The peripheral.sv header comment in
the original stub says "80 MHz", which disagrees with the generated `via_top.sv`.)
We define `CLK_FREQ_HZ = 40_000_000` as a named constant in the software constants
header and compute `sample_period = round(CLK_FREQ_HZ / sample_rate)`. Flagged as an
assumption to confirm against the real board; it only affects rate accuracy, not
correctness of the data.

## Software: `src/driver/axon_test_source_*`

Rename `IntanRhd2132Peripheral` → `AxonTestSourcePeripheral`. Keep the same set of
`RecordPluginWithLimits` overrides the example demonstrates, with dummy bodies:

- `SCIFI_RECORD_PLUGIN_LIMITS(MAX_SAMPLE_RATE, MAX_BIT_WIDTH=16, MAX_GAIN=1,
  MAX_CHANNEL_COUNT)` — generic placeholder limits (e.g. 256 channels, 40 kHz).
- `to_proto()` → descriptor `{name="Axon Test Source", vendor="Science Corporation"}`,
  type `kBroadbandSource` (mirrors headstage `AxonTestPeripheral::to_proto`). If the
  example's `from_peripheral_descriptor` helper doesn't expose the type field, set it
  the way the example's `to_proto` does and note the limitation.
- `get_lsb()` → `1.0f`.
- `self_test()` → trivial pass.
- `validate_ephys_config()` → check sample_rate ≤ max, bit_width ∈ {0,16}.
- `get_impedance()` → return `INVALID_STATE` / not-supported (no electrodes).
- `configure_sample_rate()` → `actual = clamp(desired, ≤ MAX_SAMPLE_RATE)`.
- `configure_bit_width()` → accept only 16.
- `configure_channels()` → count channels, set `channels_enabled_`, populate
  `channel_ranges` (ELECTRODE, count = N). No amp masks / SPI.
- `set_fpga_clk_freq_hz()` → store; used in period calc.
- `start_recording_impl()`:
  1. `send_packet(CONFIGURE, {channels_enabled_, sample_period})`.
  2. `subscribe_persistent(DATA_FRAME, …)`, `drain_rx()`.
  3. `send_packet(START_STREAM, {})`.
  4. report `actual_sample_rate`.
- `stop_recording_impl()`: `send_packet(STOP_STREAM, {})`, `drain_rx()`,
  `unsubscribe_persistent(DATA_FRAME)`, clear channel state.
- `parse_frame_payload(payload_words)`: write low 16 bits of the first
  `channels_enabled_` words into `frame_buffer_`, return the span.

**Deleted:** `intan_rhd2132_registers.{h,cpp}`, `intan_rhd2132_constants.h`, and all
Intan/SPI/impedance/register logic. **Added:** `axon_test_source_constants.h`
(msg-type opcodes, `CLK_FREQ_HZ`, limits).

## Files touched

Rename + rewrite:
- `src/gateware/peripheral.yaml` — name, module, sources path, cocotb test path,
  `api.msg_types` (the 4 opcodes), keep `dev_peripheral_id: 0xF001`.
- `src/gateware/src/intan_rhd2132_peripheral.sv` → `axon_test_source_peripheral.sv`
  (new FSM body).
- `src/gateware/test/tb/intan_rhd2132_tb.sv` → `axon_test_source_tb.sv` (module +
  DUT instance rename only).
- `src/gateware/test/tb/test_intan_rhd2132.py` → `test_axon_test_source.py`
  (new cocotb tests; see below).
- `src/driver/intan_rhd2132_peripheral.{h,cpp}` → `axon_test_source_peripheral.{h,cpp}`.
- `src/driver/intan_rhd2132_plugin.cpp` → `axon_test_source_plugin.cpp`
  (`SCIFI_REGISTER_PERIPHERAL(AxonTestSourcePeripheral, "axon_test_source", …, 0xF001)`).
- `src/driver/intan_rhd2132_constants.h` → `axon_test_source_constants.h` (rewritten).
- Delete `src/driver/intan_rhd2132_registers.{h,cpp}`.

Hand-edit (can't run `synapsectl peripherals gateware generate` locally — SDK lives
in the build container; `sdk/.gitkeep` is empty):
- `src/gateware/src/via_top.sv` — rename all `intan_rhd2132*` identifiers, module
  instance, `USER_*_ID` localparam, interface names. (Checksum header goes stale;
  hand-edit is supported — next `generate` will just refuse to silently overwrite.)
- `src/gateware/src/scir_sdk.rdf` — the `<Source name="...sv">` reference.

Plain edits:
- `manifest.json` — `name`, `description`, `install.target`, `gateware_target`.
- `CMakeLists.txt` — `PLUGIN_SOURCES` (drop registers.cpp), target name,
  `OUTPUT_NAME`, comments.
- `README.md` and `src/gateware/README.md` — name/path references, peripheral list.

## Tests

Rewrite `test_axon_test_source.py` cocotb suite using the existing
`generate_packet`/`parse_packet` helpers and AXI source/sink harness:
- `test_configure_and_stream`: reset → send `CONFIGURE`(N=4, small period) →
  `START_STREAM` → receive a few `DATA_FRAME`s; assert each has 4 words, msg_type =
  `DATA_FRAME`, and the low-16 values form a contiguous monotonic ramp across frames.
- `test_stop`: after `STOP_STREAM`, assert no further `DATA_FRAME` arrives within a
  timeout window.
- `test_period`: optionally check inter-frame spacing ≈ `sample_period` clk ticks.

Keep the `# CUSTOMIZE` runner block; update `toplevel`, source paths, `test_module`.

## Verification limitation

The SDK is not installed in this workspace (builds/sim run in the SDK container via
`synapsectl peripherals gateware {generate,build,sim}` and `synapsectl peripherals
build`). I will write all RTL, software, and tests, but **cannot run sim/build
here**. Verification (cocotb sim, Radiant build, plugin compile) is done in your
container. I will clearly state what is unverified rather than claim it passes.

## Out of scope (YAGNI)

- No CONFIGURE response/handshake frame (fire-and-forget).
- No throughput-in-Mbps config or `divider_timer` — sample-rate/period is enough.
- No CRC, seq numbers, addressing (transport owns those).
- No real impedance, gain, or filter behavior.
