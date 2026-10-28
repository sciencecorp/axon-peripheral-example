`timescale 1ns / 1ps

// axon_test_source_tb
// ----------------------
// Flat-AXI testbench wrapper for the axon_test_source_peripheral_top peripheral DUT.
//
// cocotb cannot connect directly to SystemVerilog `interface` ports, so this
// wrapper presents flat RX ports and a read-only producer TX tap. The selected
// TX sink is either the raw lane or the real SDK encap/FIFO output; that sink
// alone owns READY. No private DUT state is exposed.
//
// IMPORTANT — sideband defaults: the AXI-Stream sideband signals (tkeep, tid,
// tdest, tuser) are driven to safe defaults on the rx side. If these were
// left floating, `X` values would propagate into the DUT and trip
// `COCOTB_RESOLVE_X=VALUE_ERROR` in the cocotb runner. Do NOT remove the
// sideband default assignments unless you have a peripheral-specific reason.

module axon_test_source_tb #(
    parameter bit ENCAPSULATE = 0,
    parameter int ENCAP_DEPTH = 128,
    parameter logic [31:0] PERIPH_ADDR = 32'h0,
    parameter logic [31:0] DEST_ADDR = 32'h0
) (
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] rx_tdata,
    input  logic        rx_tvalid,
    input  logic        rx_tlast,
    output logic        rx_tready,
    output logic [31:0] tx_tdata,
    output logic        tx_tvalid,
    output logic        tx_tlast,
    input  logic        tx_tready,
    output logic [31:0] raw_tdata,
    output logic        raw_tvalid,
    output logic        raw_tlast,
    output logic        raw_tready,
    output logic [ 3:0] raw_tkeep,
    output logic [ 7:0] raw_tid,
    output logic        raw_tdest,
    output logic        raw_tuser
);

  axi4_stream_interface #(
      .DATA_WIDTH(32),
      .KEEP_WIDTH(4),
      .ID_WIDTH  (8),
      .DEST_WIDTH(1),
      .USER_WIDTH(1)
  )
      rx_if (), tx_if (), wire_if ();

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
  // Read-only producer tap: only the selected sink below owns READY.
  assign raw_tdata  = tx_if.tdata;
  assign raw_tvalid = tx_if.tvalid;
  assign raw_tlast  = tx_if.tlast;
  assign raw_tready = tx_if.tready;
  assign raw_tkeep  = tx_if.tkeep;
  assign raw_tid   = tx_if.tid;
  assign raw_tdest = tx_if.tdest;
  assign raw_tuser = tx_if.tuser;

  generate
    if (ENCAPSULATE) begin : g_encap
      encap #(
          .DEPTH(ENCAP_DEPTH)
      ) encapsulator (
          .clk(clk),
          .rstn(!rst),
          .periph_addr(PERIPH_ADDR),
          .dest_addr(DEST_ADDR),
          .sink_axis_if(tx_if),
          .src_axis_if(wire_if)
      );
      assign tx_tdata = wire_if.tdata;
      assign tx_tvalid = wire_if.tvalid;
      assign tx_tlast = wire_if.tlast;
      assign wire_if.tready = tx_tready;
    end else begin : g_raw
      assign tx_tdata = tx_if.tdata;
      assign tx_tvalid = tx_if.tvalid;
      assign tx_tlast = tx_if.tlast;
      assign tx_if.tready = tx_tready;
    end
  endgenerate

  axon_test_source_peripheral_top dut (
      .clk(clk),
      .rst(rst),
      .periph_addr(PERIPH_ADDR),
      .rx_axis(rx_if),
      .tx_axis(tx_if)
  );
endmodule
