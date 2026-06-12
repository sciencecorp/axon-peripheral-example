#pragma once

#include <cstdint>

namespace axon_test_source {

// Message-type opcodes — must match axon_test_source_peripheral.sv.
constexpr uint32_t CONFIGURE    = 0x0052;  // host->fpga: {channel_count, sample_period}
constexpr uint32_t START_STREAM = 0x0054;  // host->fpga
constexpr uint32_t STOP_STREAM  = 0x0055;  // host->fpga
constexpr uint32_t DATA_FRAME   = 0x0056;  // fpga->host: one frame of samples

// Limits advertised to the SDK via SCIFI_RECORD_PLUGIN_LIMITS.
constexpr uint32_t MAX_CHANNEL_COUNT = 256;
constexpr uint32_t MAX_SAMPLE_RATE   = 1'000'000;  // Hz
constexpr uint32_t MAX_BIT_WIDTH     = 16;
constexpr uint32_t MAX_GAIN          = 1;

// Gateware pacing clock (via_top clkmc = 160 MHz / 4). Turns a sample rate into
// the gateware's per-frame down-counter value.
constexpr uint32_t CLK_FREQ_HZ = 40'000'000;

}  // namespace axon_test_source
