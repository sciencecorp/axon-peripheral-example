#pragma once

#include <cstdint>

namespace axon_test_source {

// ---------------------------------------------------------------------------
// Message-type opcodes — must match axon_test_source_peripheral.sv (and the
// api.msg_types table in src/gateware/peripheral.yaml). These are the high half
// of the SDK frame header word.
// ---------------------------------------------------------------------------
constexpr uint32_t CONFIGURE = 0x0052;     // host->periph: {channel_count, sample_period}
constexpr uint32_t START_STREAM = 0x0054;  // host->periph: begin streaming
constexpr uint32_t STOP_STREAM = 0x0055;   // host->periph: stop streaming
constexpr uint32_t DATA_FRAME = 0x0056;    // periph->host: one frame of dummy words

// ---------------------------------------------------------------------------
// Hardware-limits contract (forwarded to RecordPlugin via
// SCIFI_RECORD_PLUGIN_LIMITS). Generic placeholders for a dummy source — bump
// MAX_CHANNEL_COUNT / MAX_SAMPLE_RATE freely; the gateware imposes no real cap
// beyond the Axon packet size.
// ---------------------------------------------------------------------------
constexpr uint32_t MAX_CHANNEL_COUNT = 256;     // payload = count*4 bytes (<= Axon packet limit)
constexpr uint32_t MAX_SAMPLE_RATE = 1000000;   // Hz
constexpr uint32_t MAX_BIT_WIDTH = 16;          // 16-bit samples only
constexpr float MAX_GAIN = 1.0f;                // gain is meaningless for synthetic data

// Gateware sample-period gating clock (clkmc). via_top.sv derives clkmc as
// CLK0_FREQ/4 = 160 MHz / 4 = 40 MHz. (The peripheral.sv stub comment elsewhere
// says 80 MHz; that disagrees with via_top — confirm against your board. This
// only affects the accuracy of the requested sample rate, not correctness.)
constexpr uint32_t DEFAULT_FPGA_CLK_FREQ_HZ = 40000000;

// LSB reported to the host. Synthetic data has no physical scale, so 1.0.
constexpr float ADC_STEP_UV = 1.0f;

}  // namespace axon_test_source
