#include "axon_test_source_peripheral.h"

#include <sys/random.h>

#include <algorithm>
#include <limits>

#include "api/synapse.pb.h"
#include "axon_test_source_constants.h"
#include "spdlog/spdlog.h"

namespace axon_test_source {

namespace {

constexpr auto MARKER_TIMEOUT = std::chrono::seconds(5);
constexpr int RX_RECEIVE_TIMEOUT_MS = 500;

}  // namespace

AxonTestSourcePeripheral::AxonTestSourcePeripheral(uint32_t id, uint32_t hw_addr,
                                                   zmq::context_t& ctx,
                                                   std::string_view tx_endpoint,
                                                   std::string_view rx_endpoint)
    : RecordPluginWithLimits(id, hw_addr, ctx, tx_endpoint, rx_endpoint), hw_addr_(hw_addr) {}

synapse::Peripheral AxonTestSourcePeripheral::to_proto() const {
  return from_peripheral_descriptor({.name = "Axon Test Source", .vendor = "Science Corporation"});
}

float AxonTestSourcePeripheral::get_lsb(float, float) const {
  return 1.0f;  // synthetic samples are already in microvolts
}

std::span<const uint16_t> AxonTestSourcePeripheral::parse_frame_payload(
    std::span<const uint32_t> payload_words) {
  if (primary_failure_ != scifi::Status::OK || !start_submitted_) {
    return {};
  }
  if (!begin_seen_ && std::chrono::steady_clock::now() >= begin_deadline_) {
    fail_recording(scifi::Status::TIMEOUT);
    return {};
  }

  if (!payload_words.empty() && payload_words.front() > 0xffffU) {
    if (payload_words.size() != 3 || payload_words[0] != START_MAGIC || begin_seen_ ||
        payload_words[1] != static_cast<uint32_t>(recording_nonce_) ||
        payload_words[2] != static_cast<uint32_t>(recording_nonce_ >> 32)) {
      fail_recording(scifi::Status::INVALID_PARAMETER);
      return {};
    }
    begin_seen_ = true;
    return {};
  }
  if (!begin_seen_) {
    return {};
  }
  if (payload_words.size() < channels_enabled_) {
    fail_recording(scifi::Status::INVALID_PARAMETER);
    return {};
  }
  for (uint32_t i = 0; i < channels_enabled_; ++i) {
    frame_buffer_[i] = static_cast<uint16_t>(payload_words[i]);
  }
  return {frame_buffer_, channels_enabled_};
}

scifi::Status AxonTestSourcePeripheral::start_recording_impl(uint32_t sample_rate,
                                                             uint32_t bit_width,
                                                             std::vector<synapse::Channel> channels,
                                                             float, float, float,
                                                             uint32_t& actual_sample_rate) {
  if (data_subscription_owned_) {
    return scifi::Status::INVALID_STATE;
  }
  primary_failure_ = scifi::Status::OK;
  start_submitted_ = false;
  begin_seen_ = false;
  subscription_cleanup_attempted_ = false;

  if (bit_width > std::numeric_limits<uint16_t>::max()) {
    return finish_recording(scifi::Status::INVALID_PARAMETER);
  }
  scifi::Status status = configure_bit_width(static_cast<uint16_t>(bit_width));
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }
  if (channels.empty()) {
    return finish_recording(scifi::Status::INVALID_PARAMETER);
  }
  status = configure_channels(channels);
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }

  double rate = 0.0;
  status = configure_sample_rate(sample_rate, rate);
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }
  actual_sample_rate = static_cast<uint32_t>(rate);
  if (actual_sample_rate == 0) {
    return finish_recording(scifi::Status::INVALID_PARAMETER);
  }

  uint64_t next_nonce = 0;
  if (getrandom(&next_nonce, sizeof(next_nonce), GRND_NONBLOCK) !=
          static_cast<ssize_t>(sizeof(next_nonce)) ||
      next_nonce == 0 || next_nonce == recording_nonce_) {
    spdlog::error("AxonTestSource: could not generate a fresh recording nonce");
    return finish_recording(scifi::Status::FAILURE);
  }
  recording_nonce_ = next_nonce;

  // The gateware emits one frame every `sample_period` ticks of its pacing clock.
  const uint32_t sample_period = std::max<uint32_t>(1, CLK_FREQ_HZ / actual_sample_rate);
  spdlog::info("AxonTestSource: streaming {} channels at {} Hz (period {} clks)", channels_enabled_,
               actual_sample_rate, sample_period);

  status = send_packet(CONFIGURE, {channels_enabled_, sample_period});
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }
  status = subscribe_persistent(DATA_FRAME, RX_RECEIVE_TIMEOUT_MS);
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }
  data_subscription_owned_ = true;
  begin_deadline_ = std::chrono::steady_clock::now() + MARKER_TIMEOUT;
  status = send_packet(START_STREAM, {static_cast<uint32_t>(recording_nonce_),
                                      static_cast<uint32_t>(recording_nonce_ >> 32)});
  if (status != scifi::Status::OK) {
    return finish_recording(status);
  }
  start_submitted_ = true;
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::stop_recording_impl() {
  const auto stop_deadline = std::chrono::steady_clock::now() + MARKER_TIMEOUT;
  if (!start_submitted_) {
    return finish_recording(scifi::Status::INVALID_STATE);
  }
  if (!begin_seen_ && std::chrono::steady_clock::now() >= begin_deadline_) {
    fail_recording(scifi::Status::TIMEOUT);
  }
  if (std::chrono::steady_clock::now() >= stop_deadline) {
    return finish_recording(scifi::Status::TIMEOUT);
  }

  const scifi::Status send_status = send_packet(
      STOP_STREAM,
      {static_cast<uint32_t>(recording_nonce_), static_cast<uint32_t>(recording_nonce_ >> 32)});
  if (primary_failure_ != scifi::Status::OK) {
    if (send_status != scifi::Status::OK) {
      spdlog::error("AxonTestSource: best-effort STOP send failed ({})",
                    static_cast<int>(send_status));
    }
    return finish_recording(primary_failure_);
  }
  if (send_status != scifi::Status::OK) {
    return finish_recording(send_status);
  }

  try {
    while (true) {
      auto now = std::chrono::steady_clock::now();
      if (now >= stop_deadline || (!begin_seen_ && now >= begin_deadline_)) {
        return finish_recording(scifi::Status::TIMEOUT);
      }
      // The SDK is Idle here; read_frames would return without receiving.
      const auto packet = receive_packet();
      now = std::chrono::steady_clock::now();
      if (now >= stop_deadline || (!begin_seen_ && now >= begin_deadline_)) {
        return finish_recording(scifi::Status::TIMEOUT);
      }
      if (!packet) {
        continue;
      }
      if (packet->type() != DATA_FRAME || packet->src_addr() != hw_addr_) {
        return finish_recording(scifi::Status::INVALID_PARAMETER);
      }

      const std::span<const uint32_t> payload_words(packet->payload(), packet->payload_size());
      if (!payload_words.empty() && payload_words.front() > 0xffffU) {
        if (payload_words.size() != 3 ||
            packet->raw_message().size() != sizeof(axon::RxMsgHeader) + 3 * sizeof(uint32_t) ||
            (payload_words[0] != START_MAGIC && payload_words[0] != STOP_MAGIC) ||
            payload_words[1] != static_cast<uint32_t>(recording_nonce_) ||
            payload_words[2] != static_cast<uint32_t>(recording_nonce_ >> 32)) {
          return finish_recording(scifi::Status::INVALID_PARAMETER);
        }
        if (payload_words[0] == START_MAGIC) {
          if (begin_seen_) {
            return finish_recording(scifi::Status::INVALID_PARAMETER);
          }
          begin_seen_ = true;
          continue;
        }
        if (!begin_seen_) {
          return finish_recording(scifi::Status::INVALID_PARAMETER);
        }
        return finish_recording(scifi::Status::OK);
      }
      if (begin_seen_ && payload_words.size() < channels_enabled_) {
        return finish_recording(scifi::Status::INVALID_PARAMETER);
      }
    }
  } catch (const zmq::error_t& error) {
    spdlog::error("AxonTestSource: STOP receive failed ({})", error.num());
    return finish_recording(scifi::Status::NETWORK_ERROR);
  }
}

void AxonTestSourcePeripheral::fail_recording(scifi::Status status) {
  if (primary_failure_ == scifi::Status::OK) {
    primary_failure_ = status;
    spdlog::error("AxonTestSource: recording protocol failed ({})", static_cast<int>(status));
  }
  // Empty spans alone cannot terminate the SDK reader under continuous traffic.
  release_data_subscription();
}

void AxonTestSourcePeripheral::release_data_subscription() {
  if (!data_subscription_owned_ || subscription_cleanup_attempted_) {
    return;
  }
  subscription_cleanup_attempted_ = true;
  const scifi::Status status = unsubscribe_persistent(DATA_FRAME);
  if (status == scifi::Status::OK) {
    data_subscription_owned_ = false;
  } else {
    if (primary_failure_ == scifi::Status::OK) {
      primary_failure_ = status;
    }
    spdlog::error("AxonTestSource: DATA subscription cleanup failed ({})",
                  static_cast<int>(status));
  }
}

scifi::Status AxonTestSourcePeripheral::finish_recording(scifi::Status status) {
  if (primary_failure_ == scifi::Status::OK) {
    primary_failure_ = status;
  }
  release_data_subscription();
  start_submitted_ = false;
  begin_seen_ = false;
  channels_enabled_ = 0;
  this->channels.clear();
  this->channel_ranges.clear();
  return primary_failure_;
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
