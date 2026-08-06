# axon-peripheral-example

A SciFi peripheral plugin built against [scifi-peripheral-sdk](../scifi-peripheral-sdk).
Fork this repo, replace `src/` with your peripheral, edit `manifest.json`, and you're done.

## tl;dr

```bash
git submodule update --init --recursive

synapsectl peripherals build both . --profile nerv512u-devkit
synapsectl -u "your-device-identifier" peripherals deploy both . --profile nerv512u-devkit
```

This cross-compiles the driver and gateware, packages them into a `.deb`
(staged under `dist/`), and deploys it; the driver installs to
`/usr/lib/scifi/plugins/axon_test_source.so` and the bitstream to
`/usr/lib/scifi/gateware/axon_test_source.bit` on the device. After deploy
reports success, restart `scifi-server` yourself — on startup it scans
`/usr/lib/scifi/plugins/`, dlopens each plugin, and dispatches matching
peripheral IDs to the plugin's factory.

## Target profiles

The example builds for **both** supported devkits. Pick one per build:

|                          | `via-devkit`                    | `nerv512u-devkit`               |
| ------------------------ | ------------------------------- | ------------------------------- |
| Board                    | Via Devkit (scIR)               | NeRV512U Devkit                 |
| Aliases                  | `via-devkit`, `sciop-devkit`    | `nerv512u-devkit`               |
| FPGA                     | `LIFCL-17-9SG72C` (QFN72)       | `LIFCL-33U-9CTG104C` (FCCSP104) |
| Logic / BRAM             | 16,640 LUT / 432 KB             | 33,000 LUT / 1,008 KB           |
| Peripheral `clk` (clkmc) | 40 MHz                          | 80 MHz                          |
| Host link                | scIR SerDes                     | USB 3 (Nucleus RISC-V SoC)      |
| USB PID                  | `0x000B`                        | `0x0001`                        |
| Gateware project         | `src/gateware/via-devkit/`      | `src/gateware/nerv512u-devkit/` |
| Top module               | `src/via_top.sv`                | `src/nerv_top.sv`               |
| Board seed               | `src/scir_sdk.{rdf,pdc,sdc}`    | `src/nerv512u_sdk.{rdf,pdc,sdc}`|
| Driver macro             | `AXON_PROFILE_VIA_DEVKIT`       | `AXON_PROFILE_NERV512U_DEVKIT`  |

Both devkits expose the **same peripheral contract** — identical port list,
identical frame format, same `dev_peripheral_id` window (`0xF001..0xFFFE`, 8
user peripherals max). That is what lets one
`src/gateware/axon_test_source_peripheral.sv` and one cocotb suite serve both;
only the generated top, board seed and encrypted transport bundle differ, and
those are what force a separate project directory per profile.

Run `synapsectl peripherals gateware list-profiles` to see what the SDK you
have installed actually ships.

`--profile` is **required** here, because the repo ships more than one and
synapsectl refuses to guess. It selects the gateware project under
`src/gateware/<profile>/` *and* is forwarded to CMake as
`-DAXON_TARGET_PROFILE`, which is what picks the driver's `CLK_FREQ_HZ`. The
two have to agree: the peripheral paces its output in gateware clock ticks, so
a driver built for the wrong profile gets every sample rate wrong by the ratio
of the two clocks — and nothing fails until it's on hardware.

You can set `SYNAPSE_GATEWARE_PROFILE` instead of passing the flag. That is the
only way to steer `synapsectl peripherals gateware <verb>`, whose whole tail is
forwarded verbatim to the SDK and so can't carry a synapsectl-side flag.

## What's in here

This example is `axon_test_source`: a **dummy data source** peripheral. Configure
it with a channel count and sample rate, and the gateware streams frames of
synthetic incrementing-counter data — handy for exercising the SDK data path
end-to-end without real hardware. It's the Axon throughput-tester idea
(`axon_source.sv`) rebuilt onto the SDK peripheral contract, which is identical
across the Via Devkit's LIFCL-17 and the NeRV512U Devkit's LIFCL-33U.

- `src/driver/axon_test_source_peripheral.{h,cpp}` — the driver. A
  `RecordPluginWithLimits` whose `start_recording` configures the gateware
  generator (`CONFIGURE`), starts the stream (`START_STREAM`), and whose
  `parse_frame_payload` unpacks each `DATA_FRAME` word's low 16 bits as a sample.
- `src/driver/axon_test_source_constants.h` — message-type opcodes (shared with
  the gateware) and the hardware-limits constants. Also holds the per-profile
  `CLK_FREQ_HZ`, selected by the `AXON_PROFILE_*` macro CMake defines; there is
  no default, so an unparameterised build fails to compile rather than
  defaulting to the wrong devkit's clock.
- `src/driver/axon_test_source_plugin.cpp` — the registration shim. Contains the
  `SCIFI_REGISTER_PERIPHERAL(...)` block that exports the plugin entry point and
  the factory function the host calls.
- `src/gateware/` — the matching FPGA peripheral. The RTL
  (`axon_test_source_peripheral.sv`) and cocotb tests (`test/`) are shared by
  both profiles; each profile's generated top wrapper, Radiant seed project and
  encrypted SDK bundles live in `src/gateware/<profile>/`. See
  `src/gateware/README.md`.
- `manifest.json` — plugin metadata. Used by `synapsectl peripherals deploy` for
  packaging and by the host's plugin loader for sanity-checking ABI version.
  Profile-independent: one driver `.so` name and one `peripheral_ids` list serve
  both devkits.
- `CMakeLists.txt` — builds `axon_test_source.so` against the SDK shared library.
  Requires `-DAXON_TARGET_PROFILE=<profile>`. The `.deb` itself is staged by
  `synapsectl peripherals build` via `fpm` (not CPack).

## Adapting for your own peripheral

1. Replace `src/driver/axon_test_source_*` with your own implementation. Your class
   must inherit `scifi::plugin::RecordPluginWithLimits<YourPeripheral>` (CRTP —
   pass your own class as the template argument) and override its pure virtuals.
2. In your plugin shim file, change the descriptor's `peripheral_ids` to your
   own peripheral IDs (use values from `axon::PeripheralId` if your peripheral
   has been assigned one, or contact Science to reserve one).
3. Update `manifest.json` `name`, `version`, and `peripheral_ids`.
4. In `CMakeLists.txt`, change `OUTPUT_NAME` and `CPACK_PACKAGE_NAME` to match.
5. Decide which devkits you support. If it's only one, delete the other
   `src/gateware/<profile>/` directory and drop its branch from
   `AXON_KNOWN_PROFILES` in `CMakeLists.txt` and from the `#if` ladder in
   `axon_test_source_constants.h` — with a single profile left, `--profile`
   becomes optional again.
6. Build, deploy, done.

## Local development (without an apt-published SDK)

If you're iterating on an SDK at the same time, drop the freshly-built `.deb`
into this repo's `sdk/` directory. Both build Dockerfiles prefer a local .deb
over the apt repo version. There are **two** SDKs, one per build half — drop in
whichever you're iterating on (or both):

- **Driver half** — `scifi-peripheral-sdk_*.deb`, consumed by the driver build.
- **Gateware half** — `axon-peripheral-sdk*.deb`, consumed by
  `Dockerfiles/gateware.Dockerfile`.

```bash
# Driver SDK
cp ../scifi-peripheral-sdk/scifi-peripheral-sdk_0.1.0_arm64.deb sdk/
# Gateware SDK
cp ../electronics/shared/tools/axon_peripheral_sdk/axon-peripheral-sdk_1.0.8-1~jammy_amd64.deb sdk/
synapsectl peripherals build both . --profile nerv512u-devkit
```

`sdk/*.deb` is gitignored, so you don't need to worry about accidentally
committing the binary.

Note the override is unconditional: while any `axon-peripheral-sdk*.deb` sits
in `sdk/`, the apt repo is never consulted and the `AXON_SDK_VERSION` pin in
`Dockerfiles/gateware.Dockerfile` (currently `1.0.8-1~jammy`, the release that
ships both target profiles) has no effect. Remove the local `.deb` to go back
to the published SDK.
