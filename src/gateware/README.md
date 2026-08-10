# gateware

The FPGA side of the peripheral. You write the RTL and declare it in
`peripheral.yaml`; the SDK generates the wiring, the Radiant project, and the
test scaffolding.

Commands are run as `synapsectl peripherals gateware <verb>`, which passes the
verb straight through to `axon-peripheral-sdk` inside the build container. If
you have the SDK installed locally you can call `axon-peripheral-sdk <verb>`
directly instead.

## Two target profiles

`target_profile` in `peripheral.yaml` takes one value, so each devkit needs its
own project directory:

| Profile | Board | FPGA | Clock |
| --- | --- | --- | --- |
| `via-devkit` | Via devkit | LIFCL-17-9SG72C | 40 MHz |
| `nerv512u-devkit` | NeRV512U devkit | LIFCL-33U-9CTG104C | 80 MHz |

They can't share a directory because each profile's generated top wrapper,
Radiant project, and encrypted `transport.enc.v` have the same filenames but
different contents. Via's transport carries the scIR SerDes; the NeRV512U's
carries the Nucleus RISC-V/USB 3 SoC.

The peripheral RTL and its tests don't depend on any of that, so they live once
at the top of this directory and each project points at them with a `../` path.

You can build and keep both, but only one can be deployed to a device at a
time; deploying the second replaces the first.

## Layout

```
.
├── axon_test_source_peripheral.sv   # peripheral RTL, shared
├── test/
│   ├── conftest.py
│   └── tb/                          # testbench + cocotb tests, shared
├── via-devkit/
│   ├── peripheral.yaml              # peripherals, IO, message types, sources
│   ├── src/
│   │   ├── via_top.sv               # generated, checked in
│   │   ├── scir_sdk.rdf             # generated Radiant project, checked in
│   │   ├── scir_sdk.pdc             # pin constraints
│   │   ├── scir_sdk.sdc             # timing constraints
│   │   └── {transport,decap,encap}.enc.v
│   └── build/                       # build outputs, gitignored
└── nerv512u-devkit/
    ├── peripheral.yaml
    ├── src/
    │   ├── nerv_top.sv
    │   ├── nerv512u_sdk.rdf
    │   ├── nerv512u_sdk.{pdc,sdc}
    │   └── {transport,decap,encap}.enc.v
    └── build/
```

## Selecting a profile

`build` and `deploy` take `--profile`:

```bash
synapsectl peripherals build gateware . --profile nerv512u-devkit
```

It's required, since this repo has two. The same flag also tells the driver
which board it is compiling for, and the two have to match.

The `gateware` pass-through can't take `--profile`, because everything after
`gateware` goes to the SDK unchanged. Set the environment variable instead:

```bash
export SYNAPSE_GATEWARE_PROFILE=nerv512u-devkit
synapsectl peripherals gateware sim
```

Or name the project directly with the SDK's own flag:

```bash
synapsectl peripherals gateware generate --project src/gateware/via-devkit
```

## Add or change a peripheral

Add an entry under `peripherals:` in `peripheral.yaml` and run:

```
synapsectl peripherals gateware generate
```

You can leave out `dev_peripheral_id`; `generate` allocates the lowest free one
in the `0xF001..0xFFFE` window. It also creates any missing per-peripheral
stubs, regenerates the derived files, and validates the result.

IDs are allocated per project, so if you want a peripheral to have the same ID
on both boards, set it by hand in both `peripheral.yaml` files. The driver
dispatches on those IDs and there is only one driver.

To add a peripheral to both boards, edit both `peripheral.yaml` files and run
`generate` once per project.

If the SDK gains another target profile, scaffold a project for it from the
repo root:

```
synapsectl peripherals gateware new <profile> --target <profile> --peripherals axon_test_source
```

It lands in `src/gateware/<profile>/`. Point its `fpga.sources` and
`verification.cocotb_tests` at the shared `../` paths, as the existing two do.

### Stub files you can ignore

`generate` creates `src/<name>_peripheral.sv`, `test/tb/<name>_tb.sv`, and
`test/tb/test_<name>.py` in a project whenever those paths are empty. It goes
by convention and doesn't check where `fpga.sources` actually points, so in
this repo — where the RTL and tests are shared one level up — it drops an
unused loopback stub in each project directory.

Nothing compiles them; the generated Radiant project references the shared
files. They're gitignored because a stub declares the same module name as the
real peripheral, which confuses linters and editor indexes. Delete them
whenever you like.

## Build

```
synapsectl peripherals build gateware . --profile <profile>
```

This regenerates the derived files, validates the project, and runs Radiant.
The bitstream lands in `<profile>/build/`. Radiant needs `LM_LICENSE_FILE` set.

The two profiles produce differently-named packages, so both can sit in `dist/`
at once.

To regenerate without running Radiant:

```
synapsectl peripherals gateware generate
```

## Simulate

```
synapsectl peripherals gateware sim
```

Runs the cocotb tests listed under `verification.cocotb_tests`. Equivalent to
`pytest test/tb/test_<name>.py`.

The tests compile the shared peripheral RTL and the SDK's
`axi4_stream_interface`, never a profile's top wrapper or transport, and every
assertion counts clock cycles rather than nanoseconds. So the suite is the same
under either profile, and the 80 MHz `CLK_PERIOD_NS` in the test file doesn't
need changing for the 40 MHz board.

The testbench drives `rx_axis` / `tx_axis` through `cocotbext.axi`, configured
to match the SDK frame format:

```python
AxiStreamSource(bus, clk, rst, byte_size=32, reset_active_level=True)
AxiStreamSink  (bus, clk, rst, byte_size=32, reset_active_level=True)
```

`byte_size=32` gives one beat per 32-bit word. `reset_active_level=True`
matches the active-high `rst` these peripherals use.

## Peripheral contract

Every peripheral module exposes these ports:

| Port | Direction | Width | Notes |
| --- | --- | --- | --- |
| `clk` | input | 1 | 40 MHz on via-devkit, 80 MHz on nerv512u-devkit |
| `rst` | input | 1 | Synchronous, active high, not `rstn` |
| `periph_addr` | input | 32 | This peripheral's address |
| `rx_axis` | `axi4_stream_interface.secondary` | — | Frames in |
| `tx_axis` | `axi4_stream_interface.main` | — | Frames out |

The contract is the same on both boards, which is what lets one
`axon_test_source_peripheral.sv` serve both. Only the clock rate differs, so
RTL that converts between time and cycles should take the rate as a parameter.

Frame format on `rx_axis` / `tx_axis`:

- 32-bit `tdata`, 4-bit `tkeep`, 8-bit `tid`, 1-bit `tdest`, 1-bit `tuser`.
- Beat 0 is the header word: `{msg_type[15:0], len[15:0]}`, with `len` in bytes.
- Beats 1..N are the payload, packed little-endian into 32-bit words. The first
  payload byte is bits [7:0] of beat 1.
- `tlast` is asserted on the final beat.

Your peripheral sees only the header and payload. The SDK handles the framing
above and below it.

## Constraints

Each project has a `.pdc` for pins and a `.sdc` for timing, and both are split
into two regions by sentinel markers:

- The framework region holds the board-level clocks and pins. It is rewritten
  on every `generate`, so edits there are lost.
- Everything below the end marker is yours and is preserved.

Pin assignments are per-board, so this is a place where the two projects
genuinely differ. A constraint you add to `via-devkit/src/scir_sdk.pdc` has no
counterpart in the NeRV512U project unless you write one.

## Editing generated files

`<top>.sv` and `<seed>.rdf` are generated but checked in, so the project's
state lives in git. You can hand-edit them. Each carries a
`// AUTO-GENERATED — checksum: <sha256>` header, and `generate` re-hashes the
file before rewriting it — if it doesn't match, it stops rather than
overwriting your changes.

To discard your edits and regenerate, pass `--force`. To skip a single file,
pass `--keep <path>`.

The checksum only guards those two files. Your peripheral SystemVerilog and the
`# CUSTOMIZE:` regions in the test files are never overwritten. The framework
region of the `.pdc` and `.sdc` is always rewritten, `--force` or not.

