`timescale 1ns / 1ps

// chip_peripheral_peripheral_top
// ----------------------
// Starter user peripheral for the axon-sdk template (spec §5.3, day-1
// loopback). Demonstrates the SDK peripheral contract:
//
//   Required ports (codegen wires these by name):
//     clk          — main clock (MainController clkmc, 80 MHz on via-devkit:
//                    320 MHz PLL output divided by 4 via PCLKDIV)
//     rst          — synchronous active-high reset
//     periph_addr  — 32-bit packet address (central_address | dev_peripheral_id)
//                    use it to filter inbound packets and to stamp outbound ones
//     rx_axis      — axi4_stream_interface.secondary, decap'd packets in
//     tx_axis      — axi4_stream_interface.main,      encap-bound packets out
//
// Behavior: word-for-word loopback. Every word arriving on rx_axis is
// retransmitted on tx_axis. Useful as a connectivity smoke test for the
// SDK build flow. Replace this body with your peripheral's RTL.

module chip_peripheral_peripheral_top (
    input  logic                    clk,
    input  logic                    rst,
    input  logic             [31:0] periph_addr,
    axi4_stream_interface.secondary rx_axis,
    axi4_stream_interface.main      tx_axis
);

    // Combinational passthrough. Real peripherals will register/buffer.
    assign tx_axis.tdata   = rx_axis.tdata;
    assign tx_axis.tkeep   = rx_axis.tkeep;
    assign tx_axis.tvalid  = rx_axis.tvalid;
    assign tx_axis.tlast   = rx_axis.tlast;
    assign tx_axis.tid     = rx_axis.tid;
    assign tx_axis.tdest   = rx_axis.tdest;
    assign tx_axis.tuser   = rx_axis.tuser;
    assign rx_axis.tready  = tx_axis.tready;

    // clk/rst/periph_addr unused by the combinational loopback form; kept in
    // the port list so the module matches the SDK peripheral contract
    // verbatim. Real peripherals register against clk, clear state on rst,
    // and filter inbound packets by periph_addr.
    // verilator lint_off UNUSED
    wire _unused = &{1'b0, clk, rst, periph_addr};
    // verilator lint_on UNUSED

endmodule
