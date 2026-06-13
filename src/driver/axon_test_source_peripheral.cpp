#include "axon_test_source_peripheral.h"

#include <algorithm>

#include "spdlog/spdlog.h"

#include "api/synapse.pb.h"
#include "axon_test_source_constants.h"

namespace axon_test_source {

synapse::Peripheral AxonTestSourcePeripheral::to_proto() const {
  return from_peripheral_descriptor({.name = "Axon Test Source", .vendor = "Science Corporation"});
}

float AxonTestSourcePeripheral::get_lsb(float, float) const {
  return 1.0f;  // synthetic samples are already in microvolts
}

// One DATA_FRAME per call; each payload word's low 16 bits are a signed sample.
std::span<const uint16_t> AxonTestSourcePeripheral::parse_frame_payload(
    std::span<const uint32_t> payload_words) {
  if (payload_words.size() < channels_enabled_) {
    return {};
  }
  for (uint32_t i = 0; i < channels_enabled_; ++i) {
    frame_buffer_[i] = static_cast<uint16_t>(payload_words[i]);
  }
  return {frame_buffer_, channels_enabled_};
}

scifi::Status AxonTestSourcePeripheral::start_recording_impl(
    uint32_t sample_rate, uint32_t bit_width, std::vector<synapse::Channel> channels, float,
    float, float, uint32_t& actual_sample_rate) {
  if (configure_bit_width(bit_width) != scifi::Status::OK) {
    return scifi::Status::INVALID_PARAMETER;
  }
  if (configure_channels(channels) != scifi::Status::OK || channels_enabled_ == 0) {
    return scifi::Status::INVALID_PARAMETER;
  }

  double rate = 0.0;
  configure_sample_rate(sample_rate, rate);
  actual_sample_rate = static_cast<uint32_t>(rate);
  if (actual_sample_rate == 0) {
    return scifi::Status::INVALID_PARAMETER;
  }

  // The gateware emits one frame every `sample_period` ticks of its pacing clock.
  const uint32_t sample_period = std::max<uint32_t>(1, CLK_FREQ_HZ / actual_sample_rate);
  spdlog::info("AxonTestSource: streaming {} channels at {} Hz (period {} clks)", channels_enabled_,
               actual_sample_rate, sample_period);

  if (send_packet(CONFIGURE, {channels_enabled_, sample_period}) != scifi::Status::OK) {
    return scifi::Status::FAILURE;
  }
  if (subscribe_persistent(DATA_FRAME) != scifi::Status::OK) {  // subscribe before starting
    return scifi::Status::CONNECTION_FAILED;
  }
  drain_rx();
  if (send_packet(START_STREAM, {}) != scifi::Status::OK) {
    unsubscribe_persistent(DATA_FRAME);
    return scifi::Status::FAILURE;
  }
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::stop_recording_impl() {
  send_packet(STOP_STREAM, {});
  drain_rx();
  unsubscribe_persistent(DATA_FRAME);
  channels_enabled_ = 0;
  this->channels.clear();
  this->channel_ranges.clear();
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::configure_sample_rate(double desired, double& actual) {
  if (desired <= 0.0) {
    return scifi::Status::INVALID_PARAMETER;
  }
  actual = std::min(desired, static_cast<double>(MAX_SAMPLE_RATE));
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::configure_bit_width(uint16_t bit_width) {
  return bit_width == MAX_BIT_WIDTH ? scifi::Status::OK : scifi::Status::INVALID_PARAMETER;
}

scifi::Status AxonTestSourcePeripheral::configure_channels(
    const std::vector<synapse::Channel>& chans) {
  if (chans.size() > MAX_CHANNEL_COUNT) {
    return scifi::Status::INVALID_PARAMETER;
  }
  this->channels = chans;
  channels_enabled_ = static_cast<uint32_t>(chans.size());

  synapse::ChannelRange electrodes;
  electrodes.set_type(synapse::ChannelType::ELECTRODE);
  electrodes.set_count(channels_enabled_);
  this->channel_ranges = {electrodes};
  return scifi::Status::OK;
}

// No real hardware to constrain or probe — accept any config, no electrodes.
const std::optional<std::string> AxonTestSourcePeripheral::validate_ephys_config(
    const synapse::BroadbandSourceConfig&) const {
  return std::nullopt;
}

scifi::Status AxonTestSourcePeripheral::get_impedance(uint32_t, float, float& mag, float& phase) {
  mag = 0.0f;
  phase = 0.0f;
  return scifi::Status::INVALID_STATE;
}

synapse::QueryResponse AxonTestSourcePeripheral::self_test(const synapse::SelfTestQuery&) {
  synapse::QueryResponse resp;
  resp.mutable_self_test_response();
  resp.mutable_status()->set_code(synapse::StatusCode::kOk);
  return resp;
}

}  // namespace axon_test_source
