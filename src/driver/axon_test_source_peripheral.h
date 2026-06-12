#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "spdlog/spdlog.h"

#include "api/channel.pb.h"
#include "api/node.pb.h"
#include "api/query.pb.h"
#include "api/synapse.pb.h"
#include "axon_test_source_constants.h"
#include "scifi-peripheral-sdk/record_plugin.h"
#include "scifi-peripheral-sdk/scifi/status.h"

namespace axon_test_source {

// A dummy data-source peripheral: configure it with a channel count and sample
// rate, and the gateware streams frames of synthetic incrementing-counter data.
// Useful for exercising the SDK data path end-to-end without real hardware.
//
// It keeps the RecordPlugin shape (so it slots into the normal recording
// pipeline), but every chip/SPI/impedance concern is gone — start_recording
// just configures the gateware generator and starts the stream.
class AxonTestSourcePeripheral
    : public scifi::plugin::RecordPluginWithLimits<AxonTestSourcePeripheral> {
 public:
  // Hardware-limits contract. The SDK reads these at construction time and
  // forwards them into RecordPlugin.
  SCIFI_RECORD_PLUGIN_LIMITS(
      /*max_sample_rate  =*/ MAX_SAMPLE_RATE,
      /*max_bit_width    =*/ MAX_BIT_WIDTH,
      /*max_gain         =*/ MAX_GAIN,
      /*max_channel_count=*/ MAX_CHANNEL_COUNT);

  // Inherit RecordPluginWithLimits's 5-arg ctor.
  using RecordPluginWithLimits::RecordPluginWithLimits;

  [[nodiscard]] synapse::Peripheral to_proto() const override;
  [[nodiscard]] float get_lsb(float hp_corner_hz, float lp_corner_hz) const override;

  [[nodiscard]] synapse::QueryResponse self_test(const synapse::SelfTestQuery& query) override;

  [[nodiscard]] const std::optional<std::string> validate_ephys_config(
      const synapse::BroadbandSourceConfig& config) const override;

  // Synthetic data has no electrodes — impedance is not supported.
  [[nodiscard]] scifi::Status get_impedance(uint32_t electrode_id, float stim_freq, float& mag,
                                            float& phase) override;

  [[nodiscard]] scifi::Status configure_sample_rate(double desired_sample_rate,
                                                    double& actual_sample_rate) override;

  [[nodiscard]] bool set_fpga_clk_freq_hz(uint32_t fpga_clk_freq_hz) override {
    if (fpga_clk_freq_hz == 0) {
      return false;
    }
    fpga_clk_freq_hz_ = fpga_clk_freq_hz;
    return true;
  }

 protected:
  [[nodiscard]] scifi::Status start_recording_impl(uint32_t sample_rate, uint32_t bit_width,
                                                   std::vector<synapse::Channel> channels,
                                                   float gain, float hp_corner,
                                                   float lp_corner,
                                                   uint32_t& actual_sample_rate) override;
  [[nodiscard]] scifi::Status stop_recording_impl() override;

  // One DATA_FRAME = one frame. Each payload word carries a counter value; its
  // low 16 bits are the sample. Walks channels_enabled_ words into frame_buffer_
  // and returns a span over it.
  [[nodiscard]] std::span<const uint16_t> parse_frame_payload(
      std::span<const uint32_t> payload_words) override;

  [[nodiscard]] scifi::Status configure_bit_width(uint16_t bit_width) override;
  [[nodiscard]] scifi::Status configure_channels(
      const std::vector<synapse::Channel>& channels) override;

 private:
  static constexpr int ZMQ_RECV_TIMEOUT_MS = 500;

  uint32_t fpga_clk_freq_hz_ = DEFAULT_FPGA_CLK_FREQ_HZ;
  uint32_t channels_enabled_ = 0;

  // Scratch buffer for parse_frame_payload, sized to the max channel count.
  // Written once per packet, read by the SDK's read_frames before the next
  // packet arrives — single-threaded by contract.
  uint16_t frame_buffer_[MAX_CHANNEL_COUNT] = {};
};

}  // namespace axon_test_source
