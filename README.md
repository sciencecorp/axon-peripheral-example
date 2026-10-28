# axon-peripheral-example

A working peripheral for the AxonProbe devkits: an FPGA module plus the driver
that talks to it. Clone it, swap in your own peripheral, build, deploy.

The example is `axon_test_source`, a synthetic data source. You configure it
with a channel count and a sample rate, and it streams generated neural-looking
data. Use it to check your setup works before you have real hardware to read
from, or as the starting point for your own peripheral.

## Quick start

```bash
git submodule update --init --recursive

synapsectl peripherals build both . --profile nerv512u-devkit
synapsectl -u "your-device-identifier" peripherals deploy both . --profile nerv512u-devkit
```

`build` cross-compiles the driver and the FPGA bitstream and packages them into
`dist/`. `deploy` installs them on the device. Building the bitstream runs
Lattice Radiant, so you need `LM_LICENSE_FILE` set; the driver half doesn't.

You can build one half by using `driver` or `gateware` in place of `both`.
For recording, deploy a matching driver and bitstream from the same revision
and target profile. Example 0.2.0 uses nonce-bearing START/STOP and is incompatible
with the 0.1.0 zero-payload protocol; updating only one half does not make a
matched pair.

## Target profiles

Each devkit is a separate build target. Pick one per build:

|                   | `via-devkit`               | `nerv512u-devkit`               |
| ----------------- | -------------------------- | ------------------------------- |
| Board             | Via devkit                 | NeRV512U devkit                 |
| FPGA              | `LIFCL-17-9SG72C`          | `LIFCL-33U-9CTG104C`            |
| Logic / block RAM | 16,640 LUT / 432 KB        | 33,000 LUT / 1,008 KB           |
| Peripheral clock  | 40 MHz                     | 80 MHz                          |
| Gateware project  | `src/gateware/via-devkit/` | `src/gateware/nerv512u-devkit/` |

Both boards use the same peripheral interface, so the RTL in
`src/gateware/axon_test_source_peripheral.sv` and the tests in
`src/gateware/test/` are shared. Only the generated files differ, and those
live in the per-profile directories.

`--profile` is required, because this repo supports two. It picks the gateware
project to build and tells the driver which board it is compiling for. Those
have to match: the peripheral counts clock ticks to pace its output, and the
two boards clock at different rates, so a driver built for the wrong profile
produces the wrong sample rate.

You can set `SYNAPSE_GATEWARE_PROFILE` instead of passing the flag each time.

## Recording boundaries

Each Start generates a fresh, nonzero 64-bit nonce from OS entropy; entropy
failure fails Start rather than reusing a nonce. Start configures the source,
subscribes to its existing DATA channel, and submits START with that nonce. Its
OK result means command submission, not receipt of the hardware BEGIN marker.
It performs no receive or queue drain on the control thread.

The recording reader consumes BEGIN before delivering this epoch's samples.
BEGIN and END use the same DATA packet lane as samples, with reserved first
words outside the zero-extended 16-bit sample range. They never become samples,
including with three channels, where marker and sample payload lengths match.
The full wire contract is in [the gateware README](src/gateware/README.md#test-source-recording-boundaries).

Stop sends the same nonce and consumes the existing DATA FIFO through the
matching END from the hardware source address. END follows all admitted source
frames, including a frame stalled when STOP arrived. Silence, reset, disconnect,
and an SDK Idle no-op Stop are not evidence of completion.

BEGIN has one absolute five-second deadline from START submission, checked even
under continuous old DATA and again when Stop enters without parser callbacks.
A protocol failure releases the owned subscription on the reader and latches
the first error for Stop; cleanup/send errors do not replace that error. Failed
subscription cleanup does not establish a bounded reader join.

Stop sets one five-second receipt deadline on entry, never extended by DATA.
A marker processed after that deadline is rejected. The existing 500 ms blocking
receive quantum permits the receive wait to return up to one quantum later
(a 5.5-second RX-bound return, excluding scheduling), not a strict five-second
whole-lifecycle/RPC bound. There is no quiet-gap completion or retry loop.

The socket must pass sequentially from control-thread Start to the recording
reader, which also owns Stop's receipt wait; a later Start follows reader join.
This does not make concurrent Start/Stop/read safe or fix host middleware
startup races or Stop-error propagation. The markers prove producer ordering,
not CRC validation, every sample's contents, or downstream sink drain.

## What's in here

- `src/driver/axon_test_source_peripheral.{h,cpp}` — the driver. Starts and
  stops the gateware's generator and unpacks the samples it sends back.
- `src/driver/axon_test_source_constants.h` — the message opcodes the driver
  and the FPGA agree on, the advertised limits, and the per-board clock rate.
- `src/driver/axon_test_source_plugin.cpp` — registration. Declares which
  peripheral IDs this plugin handles.
- `src/gateware/` — the FPGA side, one project per board. See
  `src/gateware/README.md`.
- `manifest.json` — name, version, and the peripheral IDs the plugin claims.
- `CMakeLists.txt` — builds the driver.

## Using it for your own peripheral

1. Replace `src/driver/axon_test_source_*`. Your class inherits
   `scifi::plugin::RecordPluginWithLimits<YourPeripheral>`, passing itself as
   the template argument, and implements its pure virtuals.
2. Replace the RTL in `src/gateware/` and the message opcodes in
   `axon_test_source_constants.h` to match.
3. Set your peripheral IDs in the plugin shim and in `manifest.json`. Contact
   Science if you need IDs assigned.
4. Update `name` and `version` in `manifest.json`, and `OUTPUT_NAME` in
   `CMakeLists.txt`.
5. If you only target one board, delete the other `src/gateware/<profile>/`
   directory and remove it from `AXON_KNOWN_PROFILES` in `CMakeLists.txt` and
   from the clock table in `axon_test_source_constants.h`. `--profile` then
   becomes optional.

## Building against a local SDK

If you are working on an SDK build that hasn't been published yet, drop the
`.deb` into `sdk/` and it will be used instead of the released version. There
are two, one per half:

```bash
cp path/to/scifi-peripheral-sdk_*.deb sdk/      # driver
cp path/to/axon-peripheral-sdk_*.deb sdk/       # gateware
```

While a `.deb` is sitting in `sdk/` the released version is never fetched, so
remove it to go back. `sdk/*.deb` is gitignored.
