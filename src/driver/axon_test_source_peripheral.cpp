#include "axon_test_source_peripheral.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "spdlog/spdlog.h"

#include "api/synapse.pb.h"
#include "axon_test_source_constants.h"
#include "scifi-peripheral-sdk/scifi/status.h"

namespace axon_test_source {

synapse::Peripheral AxonTestSourcePeripheral::to_proto() const {
  // RecordPlugin advertises this as a broadband source. The SDK helper fills in
  // the type/peripheral_id; we only supply the human-readable descriptor.
  return from_peripheral_descriptor({.name = "Axon Test Source", .vendor = "Science Corporation"});
}

float AxonTestSourcePeripheral::get_lsb(float /*hp_corner_hz*/, float /*lp_corner_hz*/) const {
  return ADC_STEP_UV;  // synthetic data has no physical scale
}

// ---------------------------------------------------------------------------
// parse_frame_payload: the SDK's read_frames calls this once per DATA_FRAME.
// Each payload word carries a counter value; its low 16 bits are the sample.
// ---------------------------------------------------------------------------
std::span<const uint16_t> AxonTestSourcePeripheral::parse_frame_payload(
    std::span<const uint32_t> payload_words) {
  if (channels_enabled_ == 0 || payload_words.size() < channels_enabled_) {
    return {};
  }
  for (uint32_t i = 0; i < channels_enabled_; ++i) {
    frame_buffer_[i] = static_cast<uint16_t>(payload_words[i] & 0xFFFF);
  }
  return std::span<const uint16_t>(frame_buffer_, channels_enabled_);
}

// ---------------------------------------------------------------------------
// Recording lifecycle — RecordPlugin's NVI wrappers toggle is_recording() for
// us, so we just implement the _impl variants.
// ---------------------------------------------------------------------------
scifi::Status AxonTestSourcePeripheral::start_recording_impl(uint32_t sample_rate,
                                                             uint32_t bit_width,
                                                             std::vector<synapse::Channel> channels,
                                                             float /*gain*/, float /*hp_corner*/,
                                                             float /*lp_corner*/,
                                                             uint32_t& actual_sample_rate) {
  spdlog::info("AxonTestSource: starting stream. SR={}, BW={}, channels={}", sample_rate, bit_width,
               channels.size());

  scifi::Status ret = configure_bit_width(bit_width);
  if (ret != scifi::Status::OK) {
    return ret;
  }

  ret = configure_channels(channels);
  if (ret != scifi::Status::OK) {
    spdlog::error("AxonTestSource: failed to configure channels.");
    return ret;
  }
  if (channels_enabled_ == 0) {
    spdlog::error("AxonTestSource: no channels configured.");
    return scifi::Status::INVALID_PARAMETER;
  }

  double negotiated_sample_rate = 0.0;
  ret = configure_sample_rate(sample_rate, negotiated_sample_rate);
  if (ret != scifi::Status::OK) {
    spdlog::error("AxonTestSource: failed to configure sample rate.");
    return ret;
  }
  actual_sample_rate = static_cast<uint32_t>(negotiated_sample_rate);
  if (actual_sample_rate == 0) {
    spdlog::error("AxonTestSource: sample rate must be > 0.");
    return scifi::Status::INVALID_PARAMETER;
  }

  // One frame per sample period. Translate the requested rate into a gateware
  // down-counter value in clk ticks (clamp to >= 1).
  uint32_t sample_period = static_cast<uint32_t>(
      std::llround(static_cast<double>(fpga_clk_freq_hz_) / actual_sample_rate));
  if (sample_period == 0) {
    sample_period = 1;
  }

  spdlog::info("AxonTestSource: {} channels at {} Hz -> sample_period={} clks @ {} Hz",
               channels_enabled_, actual_sample_rate, sample_period, fpga_clk_freq_hz_);

  // Configure the gateware generator: channel_count, then sample_period.
  ret = send_packet(CONFIGURE, {channels_enabled_, sample_period});
  if (ret != scifi::Status::OK) {
    spdlog::error("AxonTestSource: failed to send CONFIGURE.");
    return ret;
  }

  // Subscribe before starting so we don't miss the first frames.
  scifi::Status sub_ret = subscribe_persistent(DATA_FRAME, ZMQ_RECV_TIMEOUT_MS);
  if (sub_ret != scifi::Status::OK) {
    return sub_ret;
  }
  drain_rx();

  ret = send_packet(START_STREAM, {});
  if (ret != scifi::Status::OK) {
    spdlog::error("AxonTestSource: failed to send START_STREAM.");
    unsubscribe_persistent(DATA_FRAME);
    return ret;
  }

  // ----------------------------------------------------------------------
  // [DIAG] TEMPORARY instrumentation — remove once the data path works.
  //
  // Sniff *all* inbound Axon traffic for a short window right after
  // START_STREAM and log every (src_addr, type, payload_words). This runs
  // before read_frames starts (the SDK only flips to Recording after this
  // returns OK), and uses an isolated aux channel so it doesn't disturb the
  // default channel's persistent DATA_FRAME subscription.
  //
  // What the output tells us:
  //   - no packets at all          -> gateware isn't emitting (RX/START or TX)
  //   - src == this->id, type 0x56 -> emit + address OK; bug is in the read loop
  //   - src != this->id (or type)  -> addressing/encap mismatch; value shown
  // ----------------------------------------------------------------------
  {
    spdlog::warn(
        "AxonTestSource[DIAG]: this peripheral id/addr = 0x{:x}; default channel "
        "filters on (that addr, DATA_FRAME=0x{:x})",
        static_cast<uint32_t>(this->id), DATA_FRAME);

    scifi::plugin::RxChannel sniff = open_rx_channel();
    scifi::plugin::ScopedRxSubscription guard = sniff.subscribe(DATA_FRAME, 100);
    if (guard.is_valid()) {
      // Widen to a catch-all: receive every inbound packet, any src/type.
      sniff.raw_socket().set(zmq::sockopt::subscribe, "");
    } else {
      spdlog::warn("AxonTestSource[DIAG]: could not open sniff subscription");
    }

    std::map<uint64_t, int> seen;  // key = (src_addr << 32) | type
    int total = 0;
    for (int i = 0; i < 30; ++i) {  // ~up to 3s if silent; fast if traffic flows
      std::optional<axon::RxPacket> pkt = sniff.receive_packet();
      if (!pkt) {
        continue;
      }
      ++total;
      const uint64_t key =
          (static_cast<uint64_t>(pkt->src_addr()) << 32) | static_cast<uint64_t>(pkt->type());
      if (seen[key]++ < 2) {
        spdlog::warn("AxonTestSource[DIAG]: rx src=0x{:x} type=0x{:x} payload_words={}",
                     pkt->src_addr(), pkt->type(), pkt->payload_size());
      }
    }
    spdlog::warn("AxonTestSource[DIAG]: sniff window done: {} packets, {} distinct (src,type)",
                 total, seen.size());
    for (const auto& [key, count] : seen) {
      spdlog::warn("AxonTestSource[DIAG]:   src=0x{:x} type=0x{:x} -> {} pkts",
                   static_cast<uint32_t>(key >> 32),
                   static_cast<uint32_t>(key & 0xFFFFFFFFu), count);
    }
  }  // sniff + guard destruct here: unsubscribe + disconnect the aux channel

  spdlog::info("AxonTestSource: streaming {} channels at {} Hz", channels_enabled_,
               actual_sample_rate);
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::stop_recording_impl() {
  spdlog::debug("AxonTestSource: stopping stream");

  scifi::Status ret = send_packet(STOP_STREAM, {});
  if (ret != scifi::Status::OK) {
    spdlog::warn("AxonTestSource: failed to send STOP_STREAM.");
  }

  int packets_cleared = drain_rx();
  spdlog::debug("AxonTestSource: cleared {} packets from RX socket", packets_cleared);

  unsubscribe_persistent(DATA_FRAME);

  channels_enabled_ = 0;
  this->channels.clear();
  this->channel_ranges.clear();

  return scifi::Status::OK;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
scifi::Status AxonTestSourcePeripheral::configure_sample_rate(double desired_sample_rate,
                                                              double& actual_sample_rate) {
  if (desired_sample_rate <= 0.0) {
    return scifi::Status::INVALID_PARAMETER;
  }
  actual_sample_rate = std::min(desired_sample_rate, static_cast<double>(MAX_SAMPLE_RATE));
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::configure_bit_width(uint16_t bit_width) {
  if (bit_width != MAX_BIT_WIDTH) {
    spdlog::warn("AxonTestSource: only {}-bit width supported. Requested: {}", MAX_BIT_WIDTH,
                 bit_width);
    return scifi::Status::INVALID_PARAMETER;
  }
  return scifi::Status::OK;
}

scifi::Status AxonTestSourcePeripheral::configure_channels(
    const std::vector<synapse::Channel>& chans) {
  if (chans.empty()) {
    spdlog::warn("AxonTestSource: no channels provided.");
    this->channels.clear();
    this->channel_ranges.clear();
    channels_enabled_ = 0;
    return scifi::Status::OK;
  }

  if (chans.size() > MAX_CHANNEL_COUNT) {
    spdlog::error("AxonTestSource: too many channels: {} (max {})", chans.size(),
                  MAX_CHANNEL_COUNT);
    return scifi::Status::INVALID_PARAMETER;
  }

  // Store channels sorted by electrode ID so the per-frame sample order is
  // deterministic.
  std::vector<synapse::Channel> sorted_channels(chans.begin(), chans.end());
  std::sort(sorted_channels.begin(), sorted_channels.end(),
            [](const synapse::Channel& a, const synapse::Channel& b) {
              return a.electrode_id() < b.electrode_id();
            });

  this->channels = sorted_channels;
  channels_enabled_ = static_cast<uint32_t>(sorted_channels.size());

  synapse::ChannelRange electrode_range;
  electrode_range.set_type(synapse::ChannelType::ELECTRODE);
  electrode_range.set_count(channels_enabled_);
  this->channel_ranges.clear();
  this->channel_ranges.push_back(electrode_range);

  return scifi::Status::OK;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
const std::optional<std::string> AxonTestSourcePeripheral::validate_ephys_config(
    const synapse::BroadbandSourceConfig& config) const {
  if (config.sample_rate_hz() > MAX_SAMPLE_RATE) {
    return "AxonTestSource: sample rate " + std::to_string(config.sample_rate_hz()) +
           " exceeds max " + std::to_string(MAX_SAMPLE_RATE);
  }
  if (config.bit_width() != 0 && config.bit_width() != MAX_BIT_WIDTH) {
    return "AxonTestSource: only 16-bit width supported.";
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Impedance — not meaningful for a synthetic source.
// ---------------------------------------------------------------------------
scifi::Status AxonTestSourcePeripheral::get_impedance(uint32_t /*electrode_id*/,
                                                      float /*stim_freq*/, float& mag,
                                                      float& phase) {
  spdlog::warn("AxonTestSource: impedance is not supported on a synthetic source.");
  mag = 0.0f;
  phase = 0.0f;
  return scifi::Status::INVALID_STATE;
}

// ---------------------------------------------------------------------------
// Self test — trivially passes; there is no real hardware to probe.
// ---------------------------------------------------------------------------
synapse::QueryResponse AxonTestSourcePeripheral::self_test(
    const synapse::SelfTestQuery& /*query*/) {
  synapse::QueryResponse resp;
  auto* resp_st = resp.mutable_status();

  auto* test_response = resp.mutable_self_test_response();
  auto* test = test_response->add_tests();
  test->set_test_name("Synthetic Source Test");
  test->set_passed(true);
  test->set_test_report("axon_test_source has no hardware to probe; always passes.");
  resp_st->set_code(synapse::StatusCode::kOk);
  return resp;
}

}  // namespace axon_test_source
