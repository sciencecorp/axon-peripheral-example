// AUTO-GENERATED — checksum: fd4e0cb6547b6d470995c59ac7e702e08fc574e621b2be5fcfe55cc645828ffc
/*
 * Science Corporation — Axon Peripheral SDK
 * nerv_top.sv — Generated from peripheral.yaml by axon-peripheral-sdk codegen.
 *
 * You can hand-edit this file. The `// AUTO-GENERATED — checksum: <sha>` line
 * at the top is the file's fingerprint: it lets `axon-peripheral-sdk generate`
 * notice that you've modified the file and refuse to overwrite your edits.
 * If you want a fresh re-emit (discarding your hand-edits), pass
 * `--force` to acknowledge the loss.
 *
 * The body below mirrors the NeRV512U devkit external pin list (60 MHz clock +
 * USB 3 + UART debug) and wires 1 user peripheral into the SDK
 * transport bundle. The transport wraps the Nucleus RISC-V/USB3 SoC and the
 * Axon MainController; the neural-recording peripherals from the production
 * NeRV512U (ASIC01 / MUX01) are intentionally absent — the SDK target leaves
 * those pins free for the user's `peripheral.yaml fpga.io[]` claims.
 */

`resetall `timescale 1ns / 1ps `default_nettype none
module nerv_top (
    input  wire        clki,        // 60 MHz input clock
    output logic       clki_en,     // oscillator enable

    // USB 3 host link
    input  wire        REFINCLKEXTM_i,
    input  wire        REFINCLKEXTP_i,
    inout  wire        RESEXTUSB2,
    input  wire        RXM_i,
    input  wire        RXP_i,
    output logic       TXM_o,
    output logic       TXP_o,
    inout  wire        VBUS_i,
    inout  wire        usb23_DM,
    inout  wire        usb23_DP,

    // UART debug + board power-sequencing trigger
    input  wire        uart_rx,
    output logic       uart_tx,
    output logic       vbus_trig);

    assign clki_en = 1'b1;

    // ------------------------------------------------------------------------
    // Configuration constants — N user peripherals, zero built-ins. The
    // transport bundle's AXI-Stream switch scales 1:1 with N_USER (the
    // transport handles its own host-side wiring internally).
    // ------------------------------------------------------------------------
    localparam unsigned N_USER      = 1;
    localparam unsigned S_COUNT     = 1;  // S_COUNT     = N_USER
    localparam unsigned M_COUNT     = 1;  // M_COUNT     = N_USER
    localparam unsigned DATA_WIDTH  = 32;
    localparam unsigned ID_ENABLE   = 1;
    localparam unsigned ID_WIDTH    = 8;
    localparam unsigned DEST_WIDTH  = 1;
    localparam unsigned USER_ENABLE = 1;
    localparam unsigned USER_WIDTH  = 1;

    // localparam GIT_HASH; generated into this project's src/ at build time and
    // forwarded to the transport so READ_DEVICE_INFO reports the user repo's HEAD.
    `include "git_hash.sv"

    // User peripheral IDs (pinned in peripheral.yaml, must lie in 0xF001..0xFFFE)
    localparam unsigned USER_AXON_TEST_SOURCE_ID = 16'hF001;

    // ------------------------------------------------------------------------
    // Clocks, reset, PLL — the devkit clk_gen emits clkmc (40 MHz, SoC +
    // peripherals + MainController), clkusb (60 MHz USB), and clksoc (SoC). No
    // 160 MHz deser/ASIC01 clock — the devkit has no on-chip neural peripheral.
    // ------------------------------------------------------------------------
    localparam unsigned PLL_CLK_FREQ = 160_000_000;  // clk_gen PLL config

    logic clkmc, clkusb, clksoc, pll_locked;
    logic rst, clear_counter;
    logic [4:0] reset_count;
    logic [1:0] rst_sync;      // synchronized into clkmc
    logic [1:0] rst_sync_soc;  // synchronized into clksoc

    clk_gen #(.PLL_CLK_FREQ(PLL_CLK_FREQ)) u_clk_gen (
        .clki  (clki),
        .lock  (pll_locked),
        .clkmc (clkmc),
        .clksoc(clksoc),
        .clkusb(clkusb)
    );

    always @(posedge clki) begin
        clear_counter <= !pll_locked;
        rst <= (reset_count != 31);
        if (clear_counter)          reset_count <= 0;
        else if (reset_count != 31) reset_count <= reset_count + 1;
    end
    always_ff @(posedge clkmc)  rst_sync     <= {rst_sync[0], rst};
    always_ff @(posedge clksoc) rst_sync_soc <= {rst_sync_soc[0], rst};

    // ------------------------------------------------------------------------
    // Board power sequencing — VBUS trigger (mirrors NeRV512U_wrapper.sv).
    // Hold vbus_trig / uart_tx low, then assert them after a ~5 s delay
    // (done_counter > 8 periods of the 500 ms timer) so the SoC + USB are fully
    // up before the host sees VBUS and enumerates. WITHOUT this the FPGA
    // configures fine but the device never enumerates as a USB device.
    // ------------------------------------------------------------------------
    localparam unsigned CLK_FREQ = PLL_CLK_FREQ / 4;  // 160 MHz / 4 = 40 MHz (matches wrapper's timer base)

    logic soc_uart_tx;  // SoC debug UART — kept off the uart_tx pin (that pin is the power-seq trigger, per production)
    logic done1000ms;
    timer #(
        .CLK_FREQ             (CLK_FREQ),
        .DURATION_MILLISECONDS(500)
    ) timer_inst_1000ms (
        .clk (clkmc),
        .rstn(1'b1),
        .en  (pll_locked),
        .done(done1000ms)
    );

    logic [4:0] done_counter = 0;
    always @(posedge clkmc) begin
        if (done1000ms) begin
            if (done_counter > 8) begin
                uart_tx   <= 1;
                vbus_trig <= 1;
            end else begin
                uart_tx   <= 0;
                vbus_trig <= 0;
                done_counter = done_counter + 1;
            end
        end
    end

    logic [31:0] central_address;
    // ------------------------------------------------------------------------
    // User peripheral — axon_test_source (module=axon_test_source_peripheral_top, id=16'hF001)
    // ------------------------------------------------------------------------

    // Transport-side interfaces — wider tdest/tid carry the axis_switch
    // routing index across the SDK transport bundle.
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH + $clog2(M_COUNT + 1)),
        .USER_WIDTH(USER_WIDTH)
    ) axon_test_source_transport_s_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH + $clog2(S_COUNT + 1)),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) axon_test_source_transport_m_if ();

    // Peripheral-side interfaces — the user peripheral sees these directly
    // on its rx_axis / tx_axis ports (tid=8, tdest=1, tuser=1).
    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) axon_test_source_src_axis_if ();

    axi4_stream_interface #(
        .DATA_WIDTH(DATA_WIDTH),
        .ID_WIDTH  (ID_WIDTH),
        .DEST_WIDTH(DEST_WIDTH),
        .USER_WIDTH(USER_WIDTH)
    ) axon_test_source_sink_axis_if ();

    decap u_decap_axon_test_source (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        .sink_axis_if(axon_test_source_transport_m_if),
        .src_axis_if (axon_test_source_sink_axis_if)
    );

    encap u_encap_axon_test_source (
        .clk         (clkmc),
        .rstn        (~rst_sync[1]),
        // Source address = controller base | this peripheral's 1-based
        // connection index (switch-port order; index in the low 8 bits, <=255).
        // USER_*_ID is the enumeration id (see .peripheral_ids below), NOT the
        // address — ORing the full dev id (0xF001..) here corrupts bits [15:8].
        .periph_addr (central_address | 32'd1),
        .dest_addr   (32'h0000_0000),
        .sink_axis_if(axon_test_source_src_axis_if),
        .src_axis_if (axon_test_source_transport_s_if)
    );

    axon_test_source_peripheral_top u_user_axon_test_source (
        .clk         (clkmc),
        .rst         (rst_sync[1]),
        // This peripheral's own address (matches the encap source stamp above).
        .periph_addr (central_address | 32'd1),
        .rx_axis     (axon_test_source_sink_axis_if),
        .tx_axis     (axon_test_source_src_axis_if)    );

    // ------------------------------------------------------------------------
    // Transport — the SDK-shipped bundle that handles the AXI-Stream switch and
    // the Nucleus RISC-V/USB3 SoC host bridge. Switch ports get the user
    // peripherals in the order they appear in peripheral.yaml.
    // ------------------------------------------------------------------------
    logic [15:0] lram_fifo_status_depth;
    logic [ 7:0] debug_o;
    logic [31:0] debug_data_mc;

    transport #(
        .IS_SUB_AXON  (0),
        .S_COUNT      (S_COUNT),
        .M_COUNT      (M_COUNT),
        .DATA_WIDTH   (DATA_WIDTH),
        .ID_ENABLE    (ID_ENABLE),
        .S_ID_WIDTH   (ID_WIDTH),
        .M_DEST_WIDTH (DEST_WIDTH),
        .USER_ENABLE  (USER_ENABLE),
        .USER_WIDTH   (USER_WIDTH),
        .GIT_HASH     (GIT_HASH)
    ) u_transport (
        .clkusb        (clkusb),
        .clkmc         (clkmc),
        .clksoc        (clksoc),
        .rst_mc        (rst_sync[1]),
        .rst_soc       (rst_sync_soc[1]),

        .REFINCLKEXTM_i(REFINCLKEXTM_i),
        .REFINCLKEXTP_i(REFINCLKEXTP_i),
        .RESEXTUSB2    (RESEXTUSB2),
        .RXM_i         (RXM_i),
        .RXP_i         (RXP_i),
        .TXM_o         (TXM_o),
        .TXP_o         (TXP_o),
        .VBUS_i        (VBUS_i),
        .usb23_DM      (usb23_DM),
        .usb23_DP      (usb23_DP),

        .uart_rxd_i    (uart_rx),
        .uart_txd_o    (soc_uart_tx),
        .debug_o       (debug_o),

        .s_axis_tdata ({ axon_test_source_transport_s_if.tdata }),
        .s_axis_tvalid({ axon_test_source_transport_s_if.tvalid }),
        .s_axis_tready({ axon_test_source_transport_s_if.tready }),
        .s_axis_tlast ({ axon_test_source_transport_s_if.tlast }),
        .s_axis_tid   ({ axon_test_source_transport_s_if.tid }),
        .s_axis_tdest ({ axon_test_source_transport_s_if.tdest }),
        .s_axis_tuser ({ axon_test_source_transport_s_if.tuser }),

        .m_axis_tdata ({ axon_test_source_transport_m_if.tdata }),
        .m_axis_tvalid({ axon_test_source_transport_m_if.tvalid }),
        .m_axis_tready({ axon_test_source_transport_m_if.tready }),
        .m_axis_tlast ({ axon_test_source_transport_m_if.tlast }),
        .m_axis_tid   ({ axon_test_source_transport_m_if.tid }),
        .m_axis_tdest ({ axon_test_source_transport_m_if.tdest }),
        .m_axis_tuser ({ axon_test_source_transport_m_if.tuser }),

        .peripheral_ids({ USER_AXON_TEST_SOURCE_ID }),
        .central_address(central_address),

        .lram_fifo_status_depth(lram_fifo_status_depth),
        .debug_data_mc_o       (debug_data_mc)
    );

endmodule
`default_nettype wire
