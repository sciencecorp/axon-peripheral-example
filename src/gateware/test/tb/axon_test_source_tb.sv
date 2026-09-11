`timescale 1ns / 1ps

// Shared SDK testbench template for axon_test_source
// ----------------------
// Flat-AXI testbench wrapper for the axon_test_source_peripheral_top peripheral DUT.
//
// cocotb cannot connect directly to SystemVerilog `interface` ports, so this
// wrapper presents a flat logic-port interface to the simulator. The flat
// ports are bound 1:1 to an internal `axi4_stream_interface` instance, which
// drives the DUT.
//
// IMPORTANT — sideband defaults: the AXI-Stream sideband signals (tkeep, tid,
// tdest, tuser) are driven to safe defaults on the rx side. If these were
// left floating, `X` values would propagate into the DUT and trip
// `COCOTB_RESOLVE_X=VALUE_ERROR` in the cocotb runner. Do NOT remove the
// sideband default assignments unless you have a peripheral-specific reason.

module axon_test_source_tb (
  input  logic clk,
  input  logic rst,
  input  logic [31:0] rx_tdata,
  input  logic        rx_tvalid,
  input  logic        rx_tlast,
  output logic        rx_tready,
  output logic [31:0] tx_tdata,
  output logic        tx_tvalid,
  output logic        tx_tlast,
  input  logic        tx_tready
);
  localparam logic [31:0] PERIPH_ADDR = 32'h0;

  axi4_stream_interface #(
    .DATA_WIDTH(32), .KEEP_WIDTH(4),
    .ID_WIDTH(8), .DEST_WIDTH(1), .USER_WIDTH(1)
  ) rx_if(), tx_if();

  // Sideband defaults driven on rx_if
  assign rx_if.tkeep = '1;
  assign rx_if.tid = '0;
  assign rx_if.tdest = '0;
  assign rx_if.tuser = '0;

  // Flat ports ↔ interface signals
  assign rx_if.tdata  = rx_tdata;
  assign rx_if.tvalid = rx_tvalid;
  assign rx_if.tlast  = rx_tlast;
  assign rx_tready    = rx_if.tready;
  assign tx_tdata     = tx_if.tdata;
  assign tx_tvalid    = tx_if.tvalid;
  assign tx_tlast     = tx_if.tlast;
  assign tx_if.tready = tx_tready;

  axon_test_source_peripheral_top dut (
    .clk(clk),
    .rst(rst),
    .periph_addr(PERIPH_ADDR),
    .rx_axis(rx_if),
    .tx_axis(tx_if)
  );
endmodule
