#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "api/channel.pb.h"
#include "api/query.pb.h"
#include "api/synapse.pb.h"
#include "axon_test_source_constants.h"
#include "scifi-peripheral-sdk/record_plugin.h"
#include "scifi-peripheral-sdk/scifi/status.h"

namespace axon_test_source {

// Streams synthetic neural data (LFP + spikes) generated in the gateware.
// Configure it with a channel count and sample rate; start_recording tells the
// gateware to stream and parse_frame_payload unpacks each frame. There is no
// real hardware, so impedance/self-test/etc. are trivial.
class AxonTestSourcePeripheral
    : public scifi::plugin::RecordPluginWithLimits<AxonTestSourcePeripheral> {
 public:
  SCIFI_RECORD_PLUGIN_LIMITS(MAX_SAMPLE_RATE, MAX_BIT_WIDTH, MAX_GAIN, MAX_CHANNEL_COUNT);
  AxonTestSourcePeripheral(uint32_t id, uint32_t hw_addr, zmq::context_t& ctx,
                           std::string_view tx_endpoint, std::string_view rx_endpoint);

  [[nodiscard]] synapse::Peripheral to_proto() const override;
  [[nodiscard]] synapse::QueryResponse self_test(const synapse::SelfTestQuery&) override;
  [[nodiscard]] float get_lsb(float hp_corner_hz, float lp_corner_hz) const override;
  [[nodiscard]] const std::optional<std::string> validate_ephys_config(
      const synapse::BroadbandSourceConfig& config) const override;
  [[nodiscard]] scifi::Status get_impedance(uint32_t electrode_id, float stim_freq, float& mag,
                                            float& phase) override;
  [[nodiscard]] scifi::Status configure_sample_rate(double desired, double& actual) override;

 protected:
  [[nodiscard]] scifi::Status start_recording_impl(uint32_t sample_rate, uint32_t bit_width,
                                                   std::vector<synapse::Channel> channels,
                                                   float gain, float hp_corner, float lp_corner,
                                                   uint32_t& actual_sample_rate) override;
  [[nodiscard]] scifi::Status stop_recording_impl() override;
  [[nodiscard]] std::span<const uint16_t> parse_frame_payload(
      std::span<const uint32_t> payload_words) override;
  [[nodiscard]] scifi::Status configure_bit_width(uint16_t bit_width) override;
  [[nodiscard]] scifi::Status configure_channels(
      const std::vector<synapse::Channel>& channels) override;

 private:
  void fail_recording(scifi::Status status);
  void release_data_subscription();
  scifi::Status finish_recording(scifi::Status status);

  const uint32_t hw_addr_;
  uint64_t recording_nonce_ = 0;
  std::chrono::steady_clock::time_point begin_deadline_{};
  scifi::Status primary_failure_ = scifi::Status::OK;
  bool start_submitted_ = false;
  bool begin_seen_ = false;
  bool data_subscription_owned_ = false;
  bool subscription_cleanup_attempted_ = false;
  uint32_t channels_enabled_ = 0;
  uint16_t frame_buffer_[MAX_CHANNEL_COUNT] = {};  // scratch for parse_frame_payload
};

}  // namespace axon_test_source
