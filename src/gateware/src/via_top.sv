// AUTO-GENERATED — checksum: 6cb7243bac132560436ee31fae95fb2658ec5aaeb707aade0ceae0d13d9029e8
/*
 * Science Corporation — Axon Peripheral SDK
 * via_top.sv — AUTO-GENERATED from peripheral.yaml by axon-sdk codegen.
 *
 * Do NOT hand-edit. The `// AUTO-GENERATED — checksum: <sha>` line at the top
 * is the file's fingerprint; editing will block the next regeneration unless
 * --force-regenerate is passed.
 *
 * This top mirrors the SDK-shipped scIR external pin list (clock + IR TX +
 * nRF SPI bridge + LDO enable) and wires 1 user peripheral into the
 * encrypted `transport` bundle. ASIC01/MUX01 and other production-only
 * peripherals are intentionally absent — the SDK target leaves those pins
 * free for the user's `peripheral.yaml fpga.io[]` claims. *
 * Each user peripheral is framed by a decap (upstream) + encap (downstream)
 * pair; the peripheral-side `rx_axis` / `tx_axis` carry the simplified
 * peripheral-contract frame, while the transport-side carries the full Axon
 * packet (header + payload + CRC). */

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
    // transport bundle's AXI-Stream switch scales 1:1 with N_USER (no phantom
    // host slot at the wrapper — MainController appends its own host port
    // internally to its embedded axis_switch).
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

    // User peripheral IDs (pinned in peripheral.yaml, must lie in 0xF001..0xFFFE)
    localparam unsigned USER_CHIP_PERIPHERAL_ID = 16'hF001;

    // ------------------------------------------------------------------------
    // Clocks, reset, PLL — mirrors scIR.
    // ------------------------------------------------------------------------
    localparam unsigned INPUT_CLK_FREQ = 48_000_000;
    localparam unsigned DDR_CLK_FREQ   = 160_000_000;
    localparam unsigned SDR_CLK_FREQ   = 80_000_000;
    localparam unsigned SYS_CLK_FREQ   = DDR_CLK_FREQ / 4;

    logic clkmc, clk320, clk160, clksdr, pll_locked;
    logic rst, clear_counter;
    logic [4:0] reset_count;
    logic [1:0] rst_sync;

    clk_gen #(.PLL_CLK_FREQ(DDR_CLK_FREQ)) u_clk_gen (
        .clki  (ext_xo_i),
        .lock  (pll_locked),
        .clk320(clk320),
        .clk160(clk160),
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
    // User peripheral — chip_peripheral (module=chip_peripheral_peripheral_top, id=16'hF001)
    //
    // Wiring chain (downstream → user):
    //   transport.m_axis[k] → chip_peripheral_transport_m_if → decap → chip_peripheral_sink_axis_if → u_user_chip_peripheral.rx_axis
    // Wiring chain (user → upstream):
    //   u_user_chip_peripheral.tx_axis → chip_peripheral_src_axis_if → encap → chip_peripheral_transport_s_if → transport.s_axis[k]
    // ------------------------------------------------------------------------

    // Transport-side interfaces (full Axon packet — wider tdest/tid carry the
    // axis_switch routing index).
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH + $clog2(M_COUNT + 1)),
        .USER_WIDTH(USER_WIDTH)
    ) chip_peripheral_transport_s_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH + $clog2(S_COUNT + 1)),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) chip_peripheral_transport_m_if ();

    // Peripheral-side interfaces (simplified peripheral-contract frame:
    // payload only, no Axon header/CRC; tid=8, tdest=1, tuser=1).
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) chip_peripheral_src_axis_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) chip_peripheral_sink_axis_if ();

    decap u_decap_chip_peripheral (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        .sink_axis_if(chip_peripheral_transport_m_if),
        .src_axis_if (chip_peripheral_sink_axis_if)
    );

    encap u_encap_chip_peripheral (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        .periph_addr (central_address | USER_CHIP_PERIPHERAL_ID),
        .dest_addr   (32'h0000_0000),
        .sink_axis_if(chip_peripheral_src_axis_if),
        .src_axis_if (chip_peripheral_transport_s_if)
    );

    chip_peripheral_peripheral_top u_user_chip_peripheral (
        .clk         (clkmc),
        .rst         (rst_sync[1]),
        .periph_addr (central_address | USER_CHIP_PERIPHERAL_ID),
        .rx_axis     (chip_peripheral_sink_axis_if),
        .tx_axis     (chip_peripheral_src_axis_if)    );

    // ------------------------------------------------------------------------
    // Transport (encrypted bundle: MainController + packet_crc +
    // serdes_tx_cont + soc_spi_slave). Switch ports get the user peripherals
    // in the order they appear in peripheral.yaml.
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
        .REF_CLK_FREQ (SYS_CLK_FREQ)
    ) u_transport (
        .clk_mc       (clkmc),
        .clk_sdr      (clksdr),
        .rst          (rst_sync[1]),
        .nrf_sclk_i   (nrf_sclk_i),
        .nrf_csn_i    (nrf_csn_i),
        .nrf_mosi_i   (nrf_mosi_i),
        .nrf_miso_o   (nrf_miso_o),
        .serial_data_o(serial_data_out),

        .s_axis_tdata ({ chip_peripheral_transport_s_if.tdata }),
        .s_axis_tvalid({ chip_peripheral_transport_s_if.tvalid }),
        .s_axis_tready({ chip_peripheral_transport_s_if.tready }),
        .s_axis_tlast ({ chip_peripheral_transport_s_if.tlast }),
        .s_axis_tid   ({ chip_peripheral_transport_s_if.tid }),
        .s_axis_tdest ({ chip_peripheral_transport_s_if.tdest }),
        .s_axis_tuser ({ chip_peripheral_transport_s_if.tuser }),

        .m_axis_tdata ({ chip_peripheral_transport_m_if.tdata }),
        .m_axis_tvalid({ chip_peripheral_transport_m_if.tvalid }),
        .m_axis_tready({ chip_peripheral_transport_m_if.tready }),
        .m_axis_tlast ({ chip_peripheral_transport_m_if.tlast }),
        .m_axis_tid   ({ chip_peripheral_transport_m_if.tid }),
        .m_axis_tdest ({ chip_peripheral_transport_m_if.tdest }),
        .m_axis_tuser ({ chip_peripheral_transport_m_if.tuser }),

        .peripheral_ids({ USER_CHIP_PERIPHERAL_ID }),
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
