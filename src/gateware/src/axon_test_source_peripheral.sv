`timescale 1ns / 1ps

// axon_test_source_peripheral_top
// -------------------------------
// Dummy data source for the axon-peripheral-sdk. Configure it with a channel
// count and a per-frame sample period, then it streams frames of synthetic
// incrementing-counter data — one frame per period — until told to stop.
// Useful for exercising the SDK data path end-to-end without real hardware.
//
// Inspired by axon_source.sv (the gateware packet generator used by the Axon
// throughput tester) but rebuilt to the SDK peripheral contract: the SDK
// transport handles all Axon framing, so this module only deals with the
// per-frame header word + payload.
//
//   Required ports (codegen wires these by name):
//     clk          — main clock
//     rst          — synchronous active-high reset
//     periph_addr  — 32-bit address of THIS peripheral (unused: the transport
//                    already routes frames to us)
//     rx_axis      — axi4_stream_interface.secondary, command frames in
//     tx_axis      — axi4_stream_interface.main,      data frames out
//
// SDK frame format on rx_axis / tx_axis (one 32-bit word per beat):
//
//                  31              16 15               0
//                 +------------------+------------------+
//   word 0:       |    msg_type      |   len (bytes)    |  <-- header
//                 +------------------+------------------+
//   word 1..N:    |             payload[i]              |  tlast on final beat
//                 +-------------------------------------+
//
// Command frames (host -> peripheral, on rx_axis):
//   CONFIGURE    (0x0052): word0 = channel_count, word1 = sample_period (clks)
//   START_STREAM (0x0054): no payload — begin streaming
//   STOP_STREAM  (0x0055): no payload — stop streaming
//
// Data frames (peripheral -> host, on tx_axis):
//   DATA_FRAME   (0x0056): channel_count payload words. The payload is a single
//                          free-running counter that increments once per emitted
//                          word, so each word's low 16 bits are a sample and the
//                          host sees a contiguous ramp (drops are detectable).

module axon_test_source_peripheral_top (
    input  logic                    clk,
    input  logic                    rst,
    input  logic             [31:0] periph_addr,
    axi4_stream_interface.secondary rx_axis,
    axi4_stream_interface.main      tx_axis
);

    // ---- Message-type opcodes (high half of the header word) ----------------
    localparam logic [15:0] MSG_CONFIGURE    = 16'h0052;
    localparam logic [15:0] MSG_START_STREAM = 16'h0054;
    localparam logic [15:0] MSG_STOP_STREAM  = 16'h0055;
    localparam logic [15:0] MSG_DATA_FRAME   = 16'h0056;

    // ---- Configuration registers (written by CONFIGURE) ---------------------
    logic [15:0] channel_count;
    logic [31:0] sample_period;
    logic        stream_en;

    // =========================================================================
    // RX path — snoop the command stream. Commands are small and infrequent, so
    // we hold tready high and process each beat as it arrives.
    // =========================================================================
    assign rx_axis.tready = 1'b1;

    typedef enum logic [1:0] {
        RX_HEADER,
        RX_CFG_COUNT,
        RX_CFG_PERIOD,
        RX_DRAIN
    } rx_state_t;
    rx_state_t rx_state;

    wire rx_beat = rx_axis.tvalid & rx_axis.tready;

    always_ff @(posedge clk) begin
        if (rst) begin
            rx_state      <= RX_HEADER;
            channel_count <= '0;
            sample_period <= '0;
            stream_en     <= 1'b0;
        end else begin
            case (rx_state)
                RX_HEADER: begin
                    if (rx_beat) begin
                        case (rx_axis.tdata[31:16])
                            MSG_CONFIGURE: begin
                                // channel_count + sample_period payload follows
                                if (!rx_axis.tlast) rx_state <= RX_CFG_COUNT;
                            end
                            MSG_START_STREAM: begin
                                stream_en <= 1'b1;
                                if (!rx_axis.tlast) rx_state <= RX_DRAIN;
                            end
                            MSG_STOP_STREAM: begin
                                stream_en <= 1'b0;
                                if (!rx_axis.tlast) rx_state <= RX_DRAIN;
                            end
                            default: begin
                                if (!rx_axis.tlast) rx_state <= RX_DRAIN;
                            end
                        endcase
                    end
                end
                RX_CFG_COUNT: begin
                    if (rx_beat) begin
                        channel_count <= rx_axis.tdata[15:0];
                        rx_state      <= rx_axis.tlast ? RX_HEADER : RX_CFG_PERIOD;
                    end
                end
                RX_CFG_PERIOD: begin
                    if (rx_beat) begin
                        sample_period <= rx_axis.tdata;
                        rx_state      <= rx_axis.tlast ? RX_HEADER : RX_DRAIN;
                    end
                end
                RX_DRAIN: begin
                    // Consume any trailing payload words of an unrecognised or
                    // over-long frame until tlast.
                    if (rx_beat && rx_axis.tlast) rx_state <= RX_HEADER;
                end
                default: rx_state <= RX_HEADER;
            endcase
        end
    end

    // =========================================================================
    // Pacing — fire once every sample_period clocks while streaming. frame_due
    // is sticky (set-dominant) so a tick that lands while a frame is in flight
    // is not lost: the next frame starts as soon as the current one finishes.
    // =========================================================================
    logic [31:0] period_cnt;
    logic        frame_due;
    logic        frame_start;  // pulse: TX consumed the due request this cycle

    always_ff @(posedge clk) begin
        if (rst) begin
            period_cnt <= '0;
            frame_due  <= 1'b0;
        end else if (!stream_en) begin
            period_cnt <= sample_period;
            frame_due  <= 1'b0;
        end else if (period_cnt == 0) begin
            period_cnt <= sample_period;
            frame_due  <= 1'b1;             // set dominates a coincident start
        end else begin
            period_cnt <= period_cnt - 1;
            if (frame_start) frame_due <= 1'b0;
        end
    end

    // =========================================================================
    // TX path — emit one DATA_FRAME (header + channel_count counter words) each
    // time a frame is due.
    // =========================================================================
    typedef enum logic [1:0] { TX_IDLE, TX_HEADER, TX_DATA } tx_state_t;
    tx_state_t tx_state;

    logic [31:0] counter;
    logic [15:0] words_left;

    wire        tx_beat           = tx_axis.tvalid & tx_axis.tready;
    wire [15:0] payload_len_bytes = channel_count << 2;  // 4 bytes per 32-bit word

    assign frame_start = (tx_state == TX_IDLE) & frame_due & stream_en & (channel_count != 0);

    // Combinational stream outputs (Moore: stable while waiting on tready).
    always_comb begin
        tx_axis.tdata  = '0;
        tx_axis.tvalid = 1'b0;
        tx_axis.tlast  = 1'b0;
        tx_axis.tkeep  = '1;
        tx_axis.tid    = '0;
        tx_axis.tdest  = '0;
        tx_axis.tuser  = '0;
        case (tx_state)
            TX_HEADER: begin
                tx_axis.tdata  = {MSG_DATA_FRAME, payload_len_bytes};
                tx_axis.tvalid = 1'b1;
            end
            TX_DATA: begin
                tx_axis.tdata  = counter;
                tx_axis.tvalid = 1'b1;
                tx_axis.tlast  = (words_left == 16'd1);
            end
            default: ;  // TX_IDLE: idle
        endcase
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            tx_state   <= TX_IDLE;
            counter    <= '0;
            words_left <= '0;
        end else begin
            case (tx_state)
                TX_IDLE: begin
                    if (frame_start) begin
                        words_left <= channel_count;
                        tx_state   <= TX_HEADER;
                    end
                end
                TX_HEADER: begin
                    if (tx_beat) tx_state <= TX_DATA;
                end
                TX_DATA: begin
                    if (tx_beat) begin
                        counter    <= counter + 1;
                        words_left <= words_left - 1;
                        if (words_left == 16'd1) tx_state <= TX_IDLE;
                    end
                end
                default: tx_state <= TX_IDLE;
            endcase
        end
    end

    // periph_addr is unused — the SDK transport already routes frames to this
    // peripheral. Kept in the port list so the module matches the SDK contract.
    // verilator lint_off UNUSED
    wire _unused = &{1'b0, periph_addr};
    // verilator lint_on UNUSED

endmodule
