// AUTO-GENERATED — checksum: bfa449e7d45fc3486c9d88229535cd110688acd1046f986ac98abe235a4fe827
/*
 * Science Corporation — Axon Peripheral SDK
 * via_top.sv — Generated from peripheral.yaml by axon-peripheral-sdk codegen.
 *
 * You can hand-edit this file. The `// AUTO-GENERATED — checksum: <sha>` line
 * at the top is the file's fingerprint: it lets `axon-peripheral-sdk generate`
 * notice that you've modified the file and refuse to overwrite your edits.
 * If you want a fresh re-emit (discarding your hand-edits), pass
 * `--force` to acknowledge the loss.
 *
 * The body below mirrors the board reference top-level external pin list (clock + IR TX +
 * nRF SPI bridge + LDO enable) and wires 1 user peripheral into the
 * SDK transport bundle. Production-only peripherals are intentionally absent —
 * the SDK target leaves those pins free for the user's
 * `peripheral.yaml fpga.io[]` claims.
 */

`resetall `timescale 1ns / 1ps `default_nettype none
module via_top (
    input  wire        ext_xo_i,            // 48 MHz input clock

    output logic       ir_txa_o,
    output logic       ir_txb_o,

    input  wire        nrf_sclk_i,
    input  wire        nrf_csn_i,
    input  wire        nrf_mosi_i,
    output wire        nrf_miso_o,
    input  wire        nrf_irq_o,

    output logic       en_ldo_o);

    // ------------------------------------------------------------------------
    // Configuration constants — N user peripherals, zero built-ins. The
    // transport bundle's AXI-Stream switch scales 1:1 with N_USER (the
    // transport handles its own host-side wiring internally).
    // ------------------------------------------------------------------------
    localparam unsigned N_USER          = 1;
    localparam unsigned S_COUNT         = 1;  // S_COUNT         = N_USER
    localparam unsigned M_COUNT         = 1;  // M_COUNT         = N_USER
    localparam unsigned DATA_WIDTH      = 32;
    localparam unsigned ID_ENABLE       = 1;
    localparam unsigned ID_WIDTH        = 8;
    localparam unsigned DEST_WIDTH      = 1;
    localparam unsigned USER_ENABLE     = 1;
    localparam unsigned USER_WIDTH      = 1;

    // localparam GIT_HASH; generated into this project's src/ at build time and
    // forwarded to the transport so READ_DEVICE_INFO reports the user repo's HEAD.
    `include "git_hash.sv"

    // User peripheral IDs (pinned in peripheral.yaml, must lie in 0xF001..0xFFFE)
    localparam unsigned USER_INTAN_RHD2132_ID = 16'hF001;

    // ------------------------------------------------------------------------
    // Clocks, reset, PLL — board clocks, reset, PLL.
    // ------------------------------------------------------------------------
    localparam unsigned INPUT_CLK_FREQ = 48_000_000;   // ext_xo_i board oscillator
    localparam unsigned CLK0_FREQ      = 160_000_000;  // clk_gen PLL output (clk0)
    localparam unsigned SDR_CLK_FREQ   = 80_000_000;   // clksdr (serdes TX)
    localparam unsigned CLKMC_FREQ     = CLK0_FREQ / 4;  // clkmc (SoC + peripherals, 40 MHz)

    logic clkmc, clksdr, pll_locked;
    logic rst, clear_counter;
    logic [4:0] reset_count;
    logic [1:0] rst_sync;

    // via_top only uses clkmc (SoC/peripherals) and clksdr (serdes TX). clk_gen's
    // clk0 (PLL CLKOP) / clk1 (edge clock) are internal to the clock tree and
    // left unconnected here.
    clk_gen #(.PLL_CLK_FREQ(CLK0_FREQ)) u_clk_gen (
        .clki  (ext_xo_i),
        .lock  (pll_locked),
        .clk0  (),
        .clk1  (),
        .clksdr(clksdr),
        .clkmc (clkmc)
    );

    always @(posedge ext_xo_i) begin
        clear_counter <= !pll_locked;
        rst <= (reset_count != 31);
        if (clear_counter)             reset_count <= 0;
        else if (reset_count != 31)    reset_count <= reset_count + 1;
    end
    always_ff @(posedge clkmc) rst_sync <= {rst_sync[0], rst};

    assign en_ldo_o = 1;

    logic [31:0] central_address;
    // ------------------------------------------------------------------------
    // User peripheral — intan_rhd2132 (module=intan_rhd2132_peripheral_top, id=16'hF001)
    // ------------------------------------------------------------------------

    // Transport-side interfaces — wider tdest/tid carry the axis_switch
    // routing index across the SDK transport bundle.
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH + $clog2(M_COUNT + 1)),
        .USER_WIDTH(USER_WIDTH)
    ) intan_rhd2132_transport_s_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH + $clog2(S_COUNT + 1)),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) intan_rhd2132_transport_m_if ();

    // Peripheral-side interfaces — the user peripheral sees these directly
    // on its rx_axis / tx_axis ports (tid=8, tdest=1, tuser=1).
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) intan_rhd2132_src_axis_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) intan_rhd2132_sink_axis_if ();

    decap u_decap_intan_rhd2132 (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        .sink_axis_if(intan_rhd2132_transport_m_if),
        .src_axis_if (intan_rhd2132_sink_axis_if)
    );

    encap u_encap_intan_rhd2132 (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        .periph_addr (central_address | USER_INTAN_RHD2132_ID),
        .dest_addr   (32'h0000_0000),
        .sink_axis_if(intan_rhd2132_src_axis_if),
        .src_axis_if (intan_rhd2132_transport_s_if)
    );

    intan_rhd2132_peripheral_top u_user_intan_rhd2132 (
        .clk         (clkmc),
        .rst         (rst_sync[1]),
        .periph_addr (central_address | USER_INTAN_RHD2132_ID),
        .rx_axis     (intan_rhd2132_sink_axis_if),
        .tx_axis     (intan_rhd2132_src_axis_if)    );

    // ------------------------------------------------------------------------
    // Transport — the SDK-shipped bundle that handles framing, the AXI-Stream
    // switch, the IR TX path, and the nRF SPI bridge. Switch ports get the
    // user peripherals in the order they appear in peripheral.yaml.
    // ------------------------------------------------------------------------
    logic serial_data_out;
    logic [31:0] debug_data_serdes_tx;
    logic        debug_data_serdes_tx_valid;
    logic [31:0] debug_data_packet_crc;
    logic [31:0] debug_data_mc;

    transport #(
        .IS_SUB_AXON  (1),
        .S_COUNT      (S_COUNT),
        .M_COUNT      (M_COUNT),
        .DATA_WIDTH   (DATA_WIDTH),
        .ID_ENABLE    (ID_ENABLE),
        .S_ID_WIDTH   (ID_WIDTH),
        .M_DEST_WIDTH (DEST_WIDTH),
        .USER_ENABLE  (USER_ENABLE),
        .USER_WIDTH   (USER_WIDTH),
        .SDR_CLK_FREQ (SDR_CLK_FREQ),
        .REF_CLK_FREQ (CLKMC_FREQ),
        .GIT_HASH     (GIT_HASH)
    ) u_transport (
        .clk_mc       (clkmc),
        .clk_sdr      (clksdr),
        .rst          (rst_sync[1]),
        .nrf_sclk_i   (nrf_sclk_i),
        .nrf_csn_i    (nrf_csn_i),
        .nrf_mosi_i   (nrf_mosi_i),
        .nrf_miso_o   (nrf_miso_o),
        .serial_data_o(serial_data_out),

        .s_axis_tdata ({ intan_rhd2132_transport_s_if.tdata }),
        .s_axis_tvalid({ intan_rhd2132_transport_s_if.tvalid }),
        .s_axis_tready({ intan_rhd2132_transport_s_if.tready }),
        .s_axis_tlast ({ intan_rhd2132_transport_s_if.tlast }),
        .s_axis_tid   ({ intan_rhd2132_transport_s_if.tid }),
        .s_axis_tdest ({ intan_rhd2132_transport_s_if.tdest }),
        .s_axis_tuser ({ intan_rhd2132_transport_s_if.tuser }),

        .m_axis_tdata ({ intan_rhd2132_transport_m_if.tdata }),
        .m_axis_tvalid({ intan_rhd2132_transport_m_if.tvalid }),
        .m_axis_tready({ intan_rhd2132_transport_m_if.tready }),
        .m_axis_tlast ({ intan_rhd2132_transport_m_if.tlast }),
        .m_axis_tid   ({ intan_rhd2132_transport_m_if.tid }),
        .m_axis_tdest ({ intan_rhd2132_transport_m_if.tdest }),
        .m_axis_tuser ({ intan_rhd2132_transport_m_if.tuser }),

        .peripheral_ids({ USER_INTAN_RHD2132_ID }),
        .central_address(central_address),

        .debug_data_serdes_tx_o      (debug_data_serdes_tx),
        .debug_data_serdes_tx_valid_o(debug_data_serdes_tx_valid),
        .debug_data_packet_crc_o     (debug_data_packet_crc),
        .debug_data_mc_o             (debug_data_mc)
    );

    assign ir_txa_o = serial_data_out;
    assign ir_txb_o = serial_data_out;

endmodule
`default_nettype wire
