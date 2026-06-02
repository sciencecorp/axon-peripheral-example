# axon-peripheral-example

A SciFi peripheral plugin built against [scifi-peripheral-sdk](../scifi-peripheral-sdk).
Fork this repo, replace `src/` with your peripheral, edit `manifest.json`, and you're done.

## tl;dr

```bash
git submodule update --init --recursive

synapsectl peripherals build .
synapsectl -u "your-device-identifier" peripherals deploy .
```

This cross-compiles `build/aarch64/intan_rhd2132.so` and SFTP-uploads it to
`/opt/scifi/data/peripherals/intan_rhd2132.so` on the device. The
`scifi-server` daemon scans that directory on startup, dlopens each plugin,
and dispatches matching peripheral IDs to the plugin's factory — no apt
install, no systemd service, no restart needed beyond `scifi-server`.

## What's in here

- `src/driver/intan_rhd2132_peripheral.{h,cpp}` — the Intan RHD2132 recording
  driver. Copied verbatim from headstage; the only changes are include paths
  (`axon/protocol.h` → `scifi-peripheral-sdk/axon/protocol.h`, etc.).
- `src/driver/intan_rhd2132_registers.{h,cpp}` and
  `src/driver/intan_rhd2132_constants.h` — the chip-specific register layer,
  also copied verbatim.
- `src/driver/intan_rhd2132_plugin.cpp` — **the only new file.** Contains the
  `SCIFI_REGISTER_PERIPHERAL(...)` block that exports the plugin entry point and
  the factory function the host calls.
- `manifest.json` — plugin metadata. Used by `synapsectl peripherals deploy` for
  packaging and by the host's plugin loader for sanity-checking ABI version.
- `CMakeLists.txt` — builds `intan_rhd2132.so` against the SDK shared library.
  The `.deb` itself is staged by `synapsectl peripherals build` via `fpm`
  (not CPack).

## Adapting for your own peripheral

1. Replace `src/driver/intan_rhd2132_*` with your own implementation. Your class
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
# Driver SDK
cp ../scifi-peripheral-sdk/scifi-peripheral-sdk_0.1.0_arm64.deb sdk/
# Gateware SDK
cp ../axon-peripheral-sdk/axon-peripheral-sdk_0.1.0_amd64.deb sdk/
synapsectl peripherals build .
```

`sdk/*.deb` is gitignored, so you don't need to worry about accidentally
committing the binary.
