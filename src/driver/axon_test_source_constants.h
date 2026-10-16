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

// Gateware pacing clock (clkmc). Turns a sample rate into the gateware's
// per-frame down-counter value, so it MUST match the clkmc of the target
// profile the bitstream was built for — get this wrong and every sample rate
// is off by the ratio of the two clocks, silently.
//
// The value comes from the profile's `clocking.clkmc_hz`
// (`axon-peripheral-sdk list-profiles`). CMake defines exactly one
// AXON_PROFILE_* macro from -DAXON_TARGET_PROFILE=<profile>; there is
// deliberately no default, so an unparameterised build fails here rather than
// shipping a plausible-but-wrong rate.
#if defined(AXON_PROFILE_VIA_DEVKIT)
constexpr uint32_t CLK_FREQ_HZ = 40'000'000;   // via-devkit: scIR clkmc
#elif defined(AXON_PROFILE_NERV512U_DEVKIT)
constexpr uint32_t CLK_FREQ_HZ = 80'000'000;   // nerv512u-devkit: nerv_top clkmc
#else
#error "No target profile selected. Configure with -DAXON_TARGET_PROFILE=via-devkit or -DAXON_TARGET_PROFILE=nerv512u-devkit."
#endif

}  // namespace axon_test_source
