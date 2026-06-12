# >>> AXON PERIPHERAL SDK FRAMEWORK CONSTRAINTS (generated; do not edit) >>>
# SDK Via Devkit timing constraints — board-level clocks only.
#
# This is the FRAMEWORK region of <project>/src/scir_sdk.sdc: `axon-peripheral-sdk
# generate` re-emits it on every run. Do not edit here — add your own timing
# constraints in the user-append region below the framework end marker, which
# `generate` preserves verbatim.

create_clock -name {clk_i} -period 20.8333 [get_ports ext_xo_i]
create_clock -name {nrf_sclk} -period 125 [get_ports nrf_sclk_i]
create_clock -name {nrf_sclk_n} -period 125 [get_nets nrf_sclk_n]

# via_top derived clocks from clk_gen (160 MHz PLL, "80 MHz WCLK" config, matching
# production scir/src/top.sdc): clkmc = 40 MHz (SoC + peripherals), clksdr = 80 MHz
# (serdes TX). clk_gen's clk0/clk1 outputs are unconnected at via_top, so they are
# not constrained here.
create_clock -name {clkmc} -period 25 [get_nets clkmc]
create_clock -name {clksdr} -period 12.5 [get_nets clksdr]

set_clock_groups -asynchronous \
    -group [get_clocks clkmc] \
    -group [get_clocks clksdr] \
    -group [get_clocks clk_i] \
    -group [get_clocks nrf_sclk] \
    -group [get_clocks nrf_sclk_n]
# <<< AXON PERIPHERAL SDK FRAMEWORK CONSTRAINTS <<<

# ---------------------------------------------------------------------------
# Add your peripheral pin/timing constraints below. Everything below the
# framework block above is yours and is preserved across
# `axon-peripheral-sdk generate`.
# ---------------------------------------------------------------------------
