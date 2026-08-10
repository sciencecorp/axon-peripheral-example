# >>> AXON PERIPHERAL SDK FRAMEWORK CONSTRAINTS (generated; do not edit) >>>
# SDK NeRV512U Devkit timing constraints — board-level clocks only.
#
# This is the FRAMEWORK region of <project>/src/nerv512u_sdk.sdc:
# `axon-peripheral-sdk generate` re-emits it on every run. Do not edit here — add
# your own timing constraints in the user-append region below the framework end
# marker, which `generate` preserves verbatim.

# 60 MHz board oscillator on clki.
create_clock -name {clk_i} -period 16.66667 [get_ports clki]

# nerv_top derived clocks from the devkit clk_gen: the 160 MHz PLL output is
# divided by four for clkmc = 40 MHz (SoC + peripherals + MainController);
# clksoc = 80 MHz (SoC), clkusb = 60 MHz (USB).
# The clk320 PLL clkop that feeds the PCLKDIV is internal to clk_gen and left to
# Radiant's PLL-derived generated clock, so it is not constrained here.
create_clock -name {clkmc} -period 25 [get_nets clkmc]
create_clock -name {clksoc} -period 12.5 [get_nets clksoc]
create_clock -name {clkusb} -period 16.6667 [get_nets clkusb]

set_clock_groups -asynchronous \
    -group [get_clocks clkmc] \
    -group [get_clocks clksoc] \
    -group [get_clocks clkusb] \
    -group [get_clocks clk_i]
# <<< AXON PERIPHERAL SDK FRAMEWORK CONSTRAINTS <<<

# ---------------------------------------------------------------------------
# Add your peripheral pin/timing constraints below. Everything below the
# framework block above is yours and is preserved across
# `axon-peripheral-sdk generate`.
# ---------------------------------------------------------------------------
