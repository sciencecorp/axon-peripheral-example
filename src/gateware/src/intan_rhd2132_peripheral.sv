`timescale 1ns / 1ps

// intan_rhd2132_peripheral_top
// ----------------------
// Starter user peripheral for the axon-peripheral-sdk template. Demonstrates the SDK peripheral contract:
//
//   Required ports (codegen wires these by name):
//     clk          — main clock (80 MHz on via-devkit; 320 MHz PLL output
//                    divided by 4)
//     rst          — synchronous active-high reset
//     periph_addr  — 32-bit address of THIS peripheral; use it to filter
//                    inbound frames and to stamp outbound ones
//     rx_axis      — axi4_stream_interface.secondary, frames in
//     tx_axis      — axi4_stream_interface.main,      frames out
//
// Stream payload format on rx_axis / tx_axis (one 32-bit word per tvalid beat):
//
//                  31              16 15               0
//                 +------------------+------------------+
//   word 0:       |    msg_type      |   len (bytes)    |  <-- header
//                 +------------------+------------------+
//   word 1:       |             payload[0]              |
//                 +-------------------------------------+
//   word 2:       |             payload[1]              |
//                 +-------------------------------------+
//                 |                ...                  |
//                 +-------------------------------------+
//   word N:       |            payload[N-1]             |  tlast=1
//                 +-------------------------------------+
//
//   N        = len / 4  (len is in bytes; payload is always 32-bit aligned)
//   msg_type = your peripheral's command/event opcode (you define the table)
//   tlast    = asserted on the final payload word of every frame
//
// Behavior: word-for-word loopback. Every word arriving on rx_axis is
// retransmitted on tx_axis. Useful as a connectivity smoke test for the
// SDK build flow. Replace this body with your peripheral's RTL.

module intan_rhd2132_peripheral_top (
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
