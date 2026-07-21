# axon-peripheral-example

A SciFi peripheral plugin built against [scifi-peripheral-sdk](../headstage/ops/sdk/scifi-peripheral-sdk).
Fork this repo, replace `src/` with your peripheral, edit `manifest.json`, and you're done.

## tl;dr

```bash
git submodule update --init --recursive

synapsectl peripherals build both .
synapsectl -u "your-device-identifier" peripherals deploy both .
```

This cross-compiles the driver and gateware, packages them into a `.deb`
(staged under `dist/`), and deploys it; the driver installs to
`/usr/lib/scifi/plugins/axon_test_source.so` and the bitstream to
`/usr/lib/scifi/gateware/axon_test_source.bit` on the device. After deploy
reports success, restart `scifi-server` yourself — on startup it scans
`/usr/lib/scifi/plugins/`, dlopens each plugin, and dispatches matching
peripheral IDs to the plugin's factory.

## What's in here

This example is `axon_test_source`: a **dummy data source** peripheral. Configure
it with a channel count and sample rate, and the gateware streams frames of
synthetic incrementing-counter data — handy for exercising the SDK data path
end-to-end without real hardware. It's the Axon throughput-tester idea
(`axon_source.sv`) rebuilt onto the SDK peripheral contract.

- `src/driver/axon_test_source_peripheral.{h,cpp}` — the driver. A
  `RecordPluginWithLimits` whose `start_recording` configures the gateware
  generator (`CONFIGURE`), starts the stream (`START_STREAM`), and whose
  `parse_frame_payload` unpacks each `DATA_FRAME` word's low 16 bits as a sample.
- `src/driver/axon_test_source_constants.h` — message-type opcodes (shared with
  the gateware) and the hardware-limits constants.
- `src/driver/axon_test_source_plugin.cpp` — the registration shim. Contains the
  `SCIFI_REGISTER_PERIPHERAL(...)` block that exports the plugin entry point and
  the factory function the host calls.
- `src/gateware/` — the matching FPGA peripheral (`axon_test_source_peripheral.sv`)
  plus its cocotb tests. See `src/gateware/README.md`.
- `manifest.json` — plugin metadata. Used by `synapsectl peripherals deploy` for
  packaging and by the host's plugin loader for sanity-checking ABI version.
- `CMakeLists.txt` — builds `axon_test_source.so` against the SDK shared library.
  The `.deb` itself is staged by `synapsectl peripherals build` via `fpm`
  (not CPack).

## Adapting for your own peripheral

1. Replace `src/driver/axon_test_source_*` with your own implementation. Your class
   must inherit `scifi::plugin::RecordPluginWithLimits<YourPeripheral>` (CRTP —
   pass your own class as the template argument) and override its pure virtuals.
2. In your plugin shim file, change the descriptor's `peripheral_ids` to your
   own peripheral IDs (use values from `axon::PeripheralId` if your peripheral
   has been assigned one, or contact Science to reserve one).
3. Update `manifest.json` `name`, `version`, and `peripheral_ids`.
4. In `CMakeLists.txt`, change `OUTPUT_NAME` and `CPACK_PACKAGE_NAME` to match.
5. Build, deploy, done.

## Local development (without an apt-published SDK)

If you're iterating on an SDK at the same time, drop the freshly-built `.deb`
into this repo's `sdk/` directory. Both build Dockerfiles prefer a local .deb
over the apt repo version. There are **two** SDKs, one per build half — drop in
whichever you're iterating on (or both):

- **Driver half** — `scifi-peripheral-sdk_*.deb`, consumed by the driver build.
- **Gateware half** — `axon-peripheral-sdk*.deb`, consumed by
  `Dockerfiles/gateware.Dockerfile`.

```bash
# Driver SDK (built from headstage: ./ops/package/scripts/package_axon_peripheral_driver_sdk.sh)
cp ../headstage/output/scifi-peripheral-sdk_0.2.0_arm64.deb sdk/
# Gateware SDK
cp ../axon-peripheral-sdk/axon-peripheral-sdk_0.1.0_amd64.deb sdk/
synapsectl peripherals build both .
```

`sdk/*.deb` is gitignored, so you don't need to worry about accidentally
committing the binary.
