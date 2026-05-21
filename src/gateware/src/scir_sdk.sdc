# SDK Via Devkit timing constraints — board-level clocks only.
#
# Emitted once by `axon-sdk new` into <project>/src/scir_sdk.sdc.
# User-owned thereafter; `axon-sdk regenerate` does NOT touch it.

create_clock -name {clk_i}     -period 20.8333 [get_ports ext_xo_i]
create_clock -name {nrf_sclk}  -period 125     [get_ports nrf_sclk_i]
create_clock -name {nrf_sclk_n} -period 125    [get_nets  nrf_sclk_n]

# 320 MHz PLL output / 4 = 80 MHz clkmc (peripheral domain)
create_clock -name {clkmc}  -period 12.5  [get_nets clkmc]
create_clock -name {clk320} -period 3.125 [get_nets clk320]
create_clock -name {clk160} -period 6.25  [get_nets clk160]

set_clock_groups -asynchronous \
    -group [get_clocks clkmc]   \
    -group [get_clocks clk320]  \
    -group [get_clocks clk160]  \
    -group [get_clocks clk_i]   \
    -group [get_clocks nrf_sclk]   \
    -group [get_clocks nrf_sclk_n]
