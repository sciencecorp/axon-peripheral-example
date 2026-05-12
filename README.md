# scifi-peripheral-example

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

- `src/intan_rhd2132_peripheral.{h,cpp}` — the Intan RHD2132 recording driver.
  Copied verbatim from headstage; the only changes are include paths
  (`axon/protocol.h` → `scifi-peripheral-sdk/axon/protocol.h`, etc.).
- `src/intan_rhd2132_registers.{h,cpp}` and `src/intan_rhd2132_constants.h` —
  the chip-specific register layer, also copied verbatim.
- `src/intan_rhd2132_plugin.cpp` — **the only new file.** Contains the
  `SCIFI_PLUGIN_REGISTER(...)` block that exports the plugin entry point and
  the factory function the host calls.
- `manifest.json` — plugin metadata. Used by `synapsectl apps deploy` for
  packaging and by the host's plugin loader for sanity-checking ABI version.
- `CMakeLists.txt` — builds `intan_rhd2132.so` against the SDK shared library
  and packages it via CPack into a `.deb`.

## Adapting for your own peripheral

1. Replace `src/intan_rhd2132_*` with your own implementation. Your class must
   inherit `axon::RecordPeripheral` and override its pure virtuals.
2. In your plugin shim file, change the descriptor's `peripheral_ids` to your
   own peripheral IDs (use values from `axon::PeripheralId` if your peripheral
   has been assigned one, or contact Science to reserve one).
3. Update `manifest.json` `name`, `version`, and `peripheral_ids`.
4. In `CMakeLists.txt`, change `OUTPUT_NAME` and `CPACK_PACKAGE_NAME` to match.
5. Build, deploy, done.

## Local development (without an apt-published SDK)

If you're iterating on the SDK at the same time, drop the freshly-built
`scifi-peripheral-sdk_*.deb` into this repo's `sdk/` directory. The Dockerfile
prefers a local .deb over the apt repo version:

```bash
cp ../scifi-peripheral-sdk/scifi-peripheral-sdk_0.1.0_arm64.deb sdk/
synapsectl apps build .
```

`sdk/*.deb` is gitignored, so you don't need to worry about accidentally
committing the binary.
