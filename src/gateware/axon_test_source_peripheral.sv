`timescale 1ns / 1ps `default_nettype none

// axon_test_source_peripheral_top
// -------------------------------
// Dummy *neural* data source for the axon-peripheral-sdk. Configure it with a
// channel count and a per-frame sample period, then it streams frames of
// synthetic neural-looking data: action potentials (biphasic spikes) on a
// sinusoidal LFP, plus background noise. Useful for exercising the SDK data
// path AND downstream spike/LFP processing with realistic, reproducible input.
//
// To save resources, a single master signal is synthesised once per sample
// period and pushed through a delay line (BRAM). Every channel reads the same
// line at its own pseudo-random (but fixed) offset (DELAY_LUT[c]), so spikes
// land at different, non-linearly-spaced times across channels. The whole
// thing is deterministic from reset, so the host can predict it.
//
// Amplitudes assume get_lsb = 1.0 in the driver, i.e. 1 count == 1 uV:
//   - action potential ~200 uV peak-to-peak
//   - LFP ~100 uV amplitude (200 uV pk-pk)
//   - background noise ~+/-32 uV uniform
//
//   Required ports (codegen wires these by name):
//     clk          — main clock
//     rst          — synchronous active-high reset
//     periph_addr  — 32-bit address of THIS peripheral (unused; the transport
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
//   START_STREAM (0x0054): nonce low word, nonce high word (exactly 8 bytes)
//   STOP_STREAM  (0x0055): matching nonce, in the same word order
//
// Data frames (peripheral -> host, on tx_axis):
//   DATA_FRAME   (0x0056): channel_count zero-extended 16-bit sample words, or
//                          [START_MAGIC / STOP_MAGIC, nonce low, nonce high].
// Markers have exactly 12 payload bytes and share the normal DATA TX lane.

module axon_test_source_peripheral_top (
    input logic                                  clk,
    input logic                                  rst,
    input logic                           [31:0] periph_addr,
          axi4_stream_interface.secondary        rx_axis,
          axi4_stream_interface.main             tx_axis
);

  // ---- Message-type opcodes (high half of the header word) ----------------
  localparam logic [15:0] MSG_CONFIGURE = 16'h0052;
  localparam logic [15:0] MSG_START_STREAM = 16'h0054;
  localparam logic [15:0] MSG_STOP_STREAM = 16'h0055;
  localparam logic [15:0] MSG_DATA_FRAME = 16'h0056;
  localparam logic [31:0] START_MAGIC = 32'hA57A0001;
  localparam logic [31:0] STOP_MAGIC = 32'hA57A0002;

  // ---- Synthesis parameters -----------------------------------------------
  localparam int DEPTH = 512;  // delay-line depth (> max DELAY_LUT entry), power of 2
  localparam int ADDR_W = 9;  // $clog2(DEPTH)
  localparam logic [23:0] LFP_PHASE_INC = 24'd8389;  // ~2000 samples/LFP cycle (~10 Hz @ 20 kHz)
  localparam logic [15:0] SPIKE_THRESH = 16'd64;  // LFSR < THRESH fires a spike (~1/1024 / sample)
  localparam int SPIKE_N = 32;  // spike-template length
  localparam logic [15:0] LFSR_SEED = 16'hACE1;
  localparam logic [31:0] NOISE_SEED = 32'hCAFEF00D;

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
    RX_WORD0,
    RX_WORD1,
    RX_DRAIN
  } rx_state_t;
  rx_state_t rx_state;
  logic [15:0] rx_command;
  logic [31:0] rx_word0;

  wire rx_beat = rx_axis.tvalid && rx_axis.tready;
  wire command_complete = (rx_state == RX_WORD1) && rx_beat &&
                            rx_axis.tlast && (rx_axis.tkeep == '1);
  wire configure_command = command_complete && (rx_command == MSG_CONFIGURE);
  wire start_command = command_complete && (rx_command == MSG_START_STREAM);
  wire [63:0] command_nonce = {rx_axis.tdata, rx_word0};

  // Nothing executes until the second complete payload word is also TLAST.
  // Truncated, overlong and partial-byte commands drain without side effects.
  always_ff @(posedge clk) begin
    if (rst) begin
      rx_state   <= RX_HEADER;
      rx_command <= '0;
      rx_word0   <= '0;
    end else begin
      case (rx_state)
        RX_HEADER: begin
          if (rx_beat && !rx_axis.tlast) begin
            if ((rx_axis.tkeep == '1) && (rx_axis.tdata[15:0] == 16'd8) &&
                            ((rx_axis.tdata[31:16] == MSG_CONFIGURE) ||
                             (rx_axis.tdata[31:16] == MSG_START_STREAM) ||
                             (rx_axis.tdata[31:16] == MSG_STOP_STREAM))) begin
              rx_command <= rx_axis.tdata[31:16];
              rx_state   <= RX_WORD0;
            end else begin
              rx_state <= RX_DRAIN;
            end
          end
        end
        RX_WORD0: begin
          if (rx_beat) begin
            if (rx_axis.tlast) begin
              rx_state <= RX_HEADER;
            end else if (rx_axis.tkeep == '1) begin
              rx_word0 <= rx_axis.tdata;
              rx_state <= RX_WORD1;
            end else begin
              rx_state <= RX_DRAIN;
            end
          end
        end
        RX_WORD1: begin
          if (rx_beat) rx_state <= rx_axis.tlast ? RX_HEADER : RX_DRAIN;
        end
        RX_DRAIN: begin
          if (rx_beat && rx_axis.tlast) rx_state <= RX_HEADER;
        end
        default: rx_state <= RX_HEADER;
      endcase
    end
  end

  // One epoch owns its nonce/configuration through the final END handshake.
  // A STOP during BEGIN still finishes BEGIN, then emits END without DATA.
  typedef enum logic [2:0] {
    EPOCH_IDLE,
    EPOCH_BEGIN_PENDING,
    EPOCH_BEGIN_STOP_PENDING,
    EPOCH_STREAMING,
    EPOCH_END_PENDING
  } epoch_state_t;
  epoch_state_t epoch_state;
  logic [63:0] epoch_nonce;
  logic epoch_seen;
  logic begin_complete;
  logic end_complete;

  wire stop_command = command_complete && (rx_command == MSG_STOP_STREAM) &&
                        (epoch_state != EPOCH_IDLE) && (command_nonce == epoch_nonce);

  // Preserve generator cadence from START, even if its marker is stalled.
  // Only DATA admission waits for BEGIN; STOP suppresses subsequent ticks.
  assign stream_en = (epoch_state == EPOCH_BEGIN_PENDING) || (epoch_state == EPOCH_STREAMING);

  always_ff @(posedge clk) begin
    if (rst) begin
      epoch_state   <= EPOCH_IDLE;
      epoch_nonce   <= '0;
      epoch_seen    <= 1'b0;
      channel_count <= '0;
      sample_period <= '0;
    end else begin
      case (epoch_state)
        EPOCH_IDLE: begin
          if (configure_command && (rx_word0 >= 32'd1) && (rx_word0 <= 32'd256)) begin
            channel_count <= rx_word0[15:0];
            sample_period <= rx_axis.tdata;
          end
          // Retain the retired nonce so an immediate stale START
          // cannot reopen the epoch. Freshness beyond it is host-owned.
          if (start_command && (channel_count != 16'd0) &&
                        (!epoch_seen || (command_nonce != epoch_nonce))) begin
            epoch_nonce <= command_nonce;
            epoch_seen  <= 1'b1;
            epoch_state <= EPOCH_BEGIN_PENDING;
          end
        end
        EPOCH_BEGIN_PENDING: begin
          if (begin_complete) begin
            epoch_state <= stop_command ? EPOCH_END_PENDING : EPOCH_STREAMING;
          end else if (stop_command) begin
            epoch_state <= EPOCH_BEGIN_STOP_PENDING;
          end
        end
        EPOCH_BEGIN_STOP_PENDING: begin
          if (begin_complete) epoch_state <= EPOCH_END_PENDING;
        end
        EPOCH_STREAMING: begin
          if (stop_command) epoch_state <= EPOCH_END_PENDING;
        end
        EPOCH_END_PENDING: begin
          if (end_complete) epoch_state <= EPOCH_IDLE;
        end
        default: epoch_state <= EPOCH_IDLE;
      endcase
    end
  end

  // =========================================================================
  // Pacing — once every sample_period clocks while streaming, pulse gen_tick
  // (advance the master signal by one sample) and set frame_due (emit a
  // frame). frame_due is sticky so a tick that lands mid-frame isn't lost.
  // =========================================================================
  logic [31:0] period_cnt;
  logic        frame_due;
  logic        gen_tick;
  logic        frame_start;  // pulse: TX consumed the due request this cycle

  always_ff @(posedge clk) begin
    if (rst) begin
      period_cnt <= '0;
      frame_due  <= 1'b0;
      gen_tick   <= 1'b0;
    end else begin
      gen_tick <= 1'b0;
      if (!stream_en || stop_command) begin
        period_cnt <= sample_period;
        frame_due  <= 1'b0;
      end else if (period_cnt == 0) begin
        period_cnt <= sample_period;
        frame_due  <= 1'b1;
        gen_tick   <= 1'b1;
      end else begin
        period_cnt <= period_cnt - 1;
        if (frame_start) frame_due <= 1'b0;
      end
    end
  end

  // =========================================================================
  // Master signal synthesis — sinusoidal LFP + LFSR-triggered biphasic spikes
  // + uniform background noise. One new sample per gen_tick. Amplitudes are in
  // counts == uV (get_lsb=1.0).
  // =========================================================================
  // Sine LFP lookup (full wave, 256 entries, amplitude ~100 uV).
  localparam logic signed [15:0] SINE_LUT[0:255] = '{
      0,
      2,
      5,
      7,
      10,
      12,
      15,
      17,
      20,
      22,
      24,
      27,
      29,
      31,
      34,
      36,
      38,
      41,
      43,
      45,
      47,
      49,
      51,
      53,
      56,
      58,
      60,
      62,
      63,
      65,
      67,
      69,
      71,
      72,
      74,
      76,
      77,
      79,
      80,
      82,
      83,
      84,
      86,
      87,
      88,
      89,
      90,
      91,
      92,
      93,
      94,
      95,
      96,
      96,
      97,
      98,
      98,
      99,
      99,
      99,
      100,
      100,
      100,
      100,
      100,
      100,
      100,
      100,
      100,
      99,
      99,
      99,
      98,
      98,
      97,
      96,
      96,
      95,
      94,
      93,
      92,
      91,
      90,
      89,
      88,
      87,
      86,
      84,
      83,
      82,
      80,
      79,
      77,
      76,
      74,
      72,
      71,
      69,
      67,
      65,
      63,
      62,
      60,
      58,
      56,
      53,
      51,
      49,
      47,
      45,
      43,
      41,
      38,
      36,
      34,
      31,
      29,
      27,
      24,
      22,
      20,
      17,
      15,
      12,
      10,
      7,
      5,
      2,
      0,
      -2,
      -5,
      -7,
      -10,
      -12,
      -15,
      -17,
      -20,
      -22,
      -24,
      -27,
      -29,
      -31,
      -34,
      -36,
      -38,
      -41,
      -43,
      -45,
      -47,
      -49,
      -51,
      -53,
      -56,
      -58,
      -60,
      -62,
      -63,
      -65,
      -67,
      -69,
      -71,
      -72,
      -74,
      -76,
      -77,
      -79,
      -80,
      -82,
      -83,
      -84,
      -86,
      -87,
      -88,
      -89,
      -90,
      -91,
      -92,
      -93,
      -94,
      -95,
      -96,
      -96,
      -97,
      -98,
      -98,
      -99,
      -99,
      -99,
      -100,
      -100,
      -100,
      -100,
      -100,
      -100,
      -100,
      -100,
      -100,
      -99,
      -99,
      -99,
      -98,
      -98,
      -97,
      -96,
      -96,
      -95,
      -94,
      -93,
      -92,
      -91,
      -90,
      -89,
      -88,
      -87,
      -86,
      -84,
      -83,
      -82,
      -80,
      -79,
      -77,
      -76,
      -74,
      -72,
      -71,
      -69,
      -67,
      -65,
      -63,
      -62,
      -60,
      -58,
      -56,
      -53,
      -51,
      -49,
      -47,
      -45,
      -43,
      -41,
      -38,
      -36,
      -34,
      -31,
      -29,
      -27,
      -24,
      -22,
      -20,
      -17,
      -15,
      -12,
      -10,
      -7,
      -5,
      -2
  };

  // Biphasic action-potential template (32 samples), ~200 uV peak-to-peak:
  // sharp negative peak then positive afterpotential.
  localparam logic signed [15:0] SPIKE_ROM[0:31] = '{
      0,
      -2,
      -6,
      -20,
      -48,
      -90,
      -130,
      -144,
      -122,
      -74,
      -24,
      13,
      35,
      47,
      54,
      56,
      54,
      49,
      42,
      34,
      25,
      18,
      12,
      8,
      4,
      2,
      1,
      1,
      0,
      0,
      0,
      0
  };

  // Per-channel delay offsets (samples) — pseudo-random, fixed. Channel 0 has
  // offset 0 (the "live" reference); the rest are scattered so spikes don't
  // sweep linearly across channels. All entries are < DEPTH.
  localparam logic [9:0] DELAY_LUT[0:255] = '{
      0,
      107,
      188,
      3,
      210,
      212,
      206,
      77,
      219,
      95,
      239,
      50,
      70,
      145,
      112,
      42,
      96,
      32,
      224,
      111,
      67,
      144,
      161,
      45,
      157,
      142,
      48,
      91,
      189,
      246,
      188,
      24,
      136,
      251,
      189,
      106,
      150,
      130,
      43,
      38,
      53,
      185,
      20,
      49,
      215,
      88,
      83,
      7,
      118,
      87,
      7,
      248,
      131,
      251,
      177,
      191,
      107,
      1,
      2,
      241,
      170,
      223,
      42,
      198,
      149,
      46,
      74,
      26,
      168,
      107,
      28,
      227,
      175,
      148,
      47,
      189,
      97,
      60,
      246,
      135,
      153,
      182,
      234,
      212,
      85,
      207,
      11,
      60,
      31,
      224,
      128,
      56,
      189,
      206,
      194,
      143,
      172,
      48,
      66,
      151,
      112,
      133,
      235,
      246,
      34,
      11,
      251,
      43,
      78,
      252,
      222,
      214,
      3,
      39,
      248,
      59,
      90,
      139,
      88,
      41,
      99,
      83,
      4,
      13,
      16,
      183,
      114,
      21,
      100,
      255,
      205,
      236,
      46,
      202,
      3,
      198,
      183,
      95,
      56,
      174,
      132,
      195,
      32,
      124,
      160,
      16,
      4,
      161,
      126,
      84,
      198,
      157,
      248,
      108,
      237,
      244,
      141,
      52,
      94,
      197,
      170,
      179,
      31,
      143,
      39,
      21,
      127,
      43,
      229,
      248,
      86,
      60,
      53,
      42,
      42,
      74,
      39,
      138,
      24,
      134,
      179,
      1,
      38,
      1,
      169,
      127,
      227,
      48,
      214,
      156,
      240,
      205,
      218,
      25,
      68,
      130,
      193,
      104,
      17,
      203,
      193,
      245,
      118,
      107,
      47,
      63,
      236,
      225,
      48,
      7,
      96,
      85,
      46,
      102,
      46,
      46,
      147,
      175,
      211,
      133,
      229,
      64,
      246,
      106,
      132,
      178,
      107,
      84,
      31,
      18,
      165,
      118,
      191,
      50,
      161,
      160,
      147,
      52,
      192,
      35,
      136,
      229,
      60,
      85,
      146,
      75,
      33,
      138,
      131,
      178,
      198,
      172,
      229,
      205,
      16,
      67
  };

  logic [23:0] phase_acc;
  logic [15:0] lfsr;  // spike-trigger PRNG
  logic [31:0] noise_lfsr;  // background-noise PRNG
  logic spike_active;
  logic [5:0] spike_idx;
  logic [ADDR_W-1:0] wptr;
  logic [ADDR_W-1:0] last_waddr;

  wire lfsr_fb = lfsr[15] ^ lfsr[13] ^ lfsr[12] ^ lfsr[10];  // 16-bit maximal
  wire noise_fb = noise_lfsr[31] ^ noise_lfsr[21] ^ noise_lfsr[1] ^ noise_lfsr[0];  // 32-bit maximal

  // Current sample = saturate(LFP + spike + noise), from the CURRENT state;
  // the state advances (below) for the next sample.
  wire signed [15:0] lfp_val = SINE_LUT[phase_acc[23:16]];
  wire signed [15:0] spike_val = spike_active ? SPIKE_ROM[spike_idx] : 16'sd0;
  wire signed [15:0] noise_val = $signed({10'b0, noise_lfsr[5:0]}) - 16'sd32;  // -32..+31 uV
  wire signed [17:0] mix = lfp_val + spike_val + noise_val;
  wire signed [15:0] master_sample =
        (mix > 18'sd32767)  ? 16'sd32767 :
        (mix < -18'sd32768) ? -16'sd32768 : mix[15:0];

  always_ff @(posedge clk) begin
    if (rst) begin
      phase_acc    <= '0;
      lfsr         <= LFSR_SEED;
      noise_lfsr   <= NOISE_SEED;
      spike_active <= 1'b0;
      spike_idx    <= '0;
      wptr         <= '0;
      last_waddr   <= '0;
    end else if (gen_tick) begin
      last_waddr <= wptr;
      wptr       <= wptr + 1'b1;  // wraps naturally (power-of-2 depth)
      phase_acc  <= phase_acc + LFP_PHASE_INC;
      lfsr       <= {lfsr[14:0], lfsr_fb};
      noise_lfsr <= {noise_lfsr[30:0], noise_fb};
      if (spike_active) begin
        spike_idx <= spike_idx + 1'b1;
        if (spike_idx == SPIKE_N - 1) spike_active <= 1'b0;
      end else if (lfsr < SPIKE_THRESH) begin
        spike_active <= 1'b1;
        spike_idx    <= '0;
      end
    end
  end

  // =========================================================================
  // Delay line (simple dual-port BRAM): write the master sample once per
  // gen_tick. Read only in TX_RD, one cycle after setting raddr. Its registered
  // output then holds the offered DATA word, even across delay-line wraps.
  // =========================================================================
  logic [      15:0] delay_buf   [0:DEPTH-1] = '{default: 16'h0000};
  logic [ADDR_W-1:0] raddr;
  logic [      15:0] rdata;
  logic              sample_read;

  always_ff @(posedge clk) begin
    if (gen_tick) delay_buf[wptr] <= master_sample;
    if (sample_read) rdata <= delay_buf[raddr];
  end

  // =========================================================================
  // TX path — one lane for BEGIN, admitted DATA and END, in that order.
  // Channel c reads (last_waddr - DELAY_LUT[c]); no whole-frame RAM snapshot.
  // =========================================================================
  typedef enum logic [2:0] {
    TX_IDLE,
    TX_HEADER,
    TX_RD,
    TX_DATA,
    TX_MARKER_MAGIC,
    TX_MARKER_LOW,
    TX_MARKER_HIGH
  } tx_state_t;
  typedef enum logic [1:0] {
    FRAME_DATA,
    FRAME_BEGIN,
    FRAME_END
  } frame_kind_t;
  tx_state_t tx_state;
  frame_kind_t frame_kind;

  logic [15:0] frame_channel_count;
  logic [15:0] widx;
  wire [15:0] nxt_widx = widx + 16'd1;
  wire last_word = (widx == (frame_channel_count - 16'd1));

  wire tx_beat = tx_axis.tvalid && tx_axis.tready;
  wire [15:0] payload_len_bytes = (frame_kind == FRAME_DATA) ? (frame_channel_count << 2) : 16'd12;

  // The complete matching STOP beats admission on the same clock edge.
  assign frame_start = (tx_state == TX_IDLE) && (epoch_state == EPOCH_STREAMING) &&
                         frame_due && !stop_command && (channel_count != 16'd0);
  assign sample_read = (tx_state == TX_RD);
  assign begin_complete = (tx_state == TX_MARKER_HIGH) && (frame_kind == FRAME_BEGIN) && tx_beat;
  assign end_complete = (tx_state == TX_MARKER_HIGH) && (frame_kind == FRAME_END) && tx_beat;

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
        tx_axis.tdata  = {16'h0000, rdata};
        tx_axis.tvalid = 1'b1;
        tx_axis.tlast  = last_word;
      end
      TX_MARKER_MAGIC: begin
        tx_axis.tdata  = (frame_kind == FRAME_BEGIN) ? START_MAGIC : STOP_MAGIC;
        tx_axis.tvalid = 1'b1;
      end
      TX_MARKER_LOW: begin
        tx_axis.tdata  = epoch_nonce[31:0];
        tx_axis.tvalid = 1'b1;
      end
      TX_MARKER_HIGH: begin
        tx_axis.tdata  = epoch_nonce[63:32];
        tx_axis.tvalid = 1'b1;
        tx_axis.tlast  = 1'b1;
      end
      default: ;  // TX_IDLE / TX_RD have no offered beat.
    endcase
  end

  always_ff @(posedge clk) begin
    if (rst) begin
      tx_state            <= TX_IDLE;
      frame_kind          <= FRAME_DATA;
      frame_channel_count <= '0;
      widx                <= '0;
      raddr               <= '0;
    end else begin
      case (tx_state)
        TX_IDLE: begin
          if ((epoch_state == EPOCH_BEGIN_PENDING) ||
                        (epoch_state == EPOCH_BEGIN_STOP_PENDING)) begin
            frame_kind <= FRAME_BEGIN;
            tx_state   <= TX_HEADER;
          end else if (epoch_state == EPOCH_END_PENDING) begin
            frame_kind <= FRAME_END;
            tx_state   <= TX_HEADER;
          end else if (frame_start) begin
            frame_kind          <= FRAME_DATA;
            frame_channel_count <= channel_count;
            widx                <= '0;
            tx_state            <= TX_HEADER;
          end
        end
        TX_HEADER: begin
          if (tx_beat) begin
            if (frame_kind == FRAME_DATA) begin
              raddr    <= last_waddr - DELAY_LUT[8'd0][ADDR_W-1:0];
              tx_state <= TX_RD;
            end else begin
              tx_state <= TX_MARKER_MAGIC;
            end
          end
        end
        TX_RD:   tx_state <= TX_DATA;
        TX_DATA: begin
          if (tx_beat) begin
            if (last_word) begin
              tx_state <= TX_IDLE;
            end else begin
              widx     <= nxt_widx;
              // Valid CONFIGURE bounds channels to 1..256, so
              // this non-final next index is always in 0..255.
              raddr    <= last_waddr - DELAY_LUT[nxt_widx[7:0]][ADDR_W-1:0];
              tx_state <= TX_RD;
            end
          end
        end
        TX_MARKER_MAGIC: begin
          if (tx_beat) tx_state <= TX_MARKER_LOW;
        end
        TX_MARKER_LOW: begin
          if (tx_beat) tx_state <= TX_MARKER_HIGH;
        end
        TX_MARKER_HIGH: begin
          if (tx_beat) tx_state <= TX_IDLE;
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

`default_nettype wire
