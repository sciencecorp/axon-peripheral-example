#include "intan_rhd2132_peripheral.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cmath>
#include <limits>
#include <thread>

#include "spdlog/spdlog.h"
#include "zmq.hpp"

#include "api/node.pb.h"
#include "api/synapse.pb.h"
#include "intan_rhd2132_constants.h"
#include "scifi-peripheral-sdk/axon/protocol.h"
#include "scifi-peripheral-sdk/axon/frame.h"
#include "scifi-peripheral-sdk/scifi/status.h"

namespace chips {
namespace intan_rhd2132 {

namespace {

constexpr uint32_t IMPEDANCE_DAC_STEPS_PER_PERIOD = 100;
constexpr uint32_t IMPEDANCE_SAMPLES_PER_PERIOD = 50;
constexpr uint8_t IMPEDANCE_DAC_OFFSET_LSB = 128;
constexpr uint8_t IMPEDANCE_DAC_AMPLITUDE_LSB = 127;
constexpr double IMPEDANCE_ADC_LSB_V = 0.195e-6;
constexpr double DEFAULT_PARASITIC_CAPACITANCE_PF = 15.0;

struct AdcBias {
  uint8_t adc_buffer_bias;
  uint8_t mux_bias;
};

struct CorrectedImpedance {
  double magnitude_ohms;
  double phase_deg;
};

AdcBias adc_bias_for_rate(double rate_hz) {
  if (rate_hz <= 120e3) return {32, 40};
  if (rate_hz <= 140e3) return {16, 40};
  if (rate_hz <= 175e3) return {8, 40};
  if (rate_hz <= 220e3) return {8, 32};
  if (rate_hz <= 280e3) return {8, 26};
  if (rate_hz <= 350e3) return {4, 18};
  if (rate_hz <= 440e3) return {3, 16};
  if (rate_hz <= 525e3) return {3, 7};
  return {2, 4};
}

double wrap_phase_deg(double phase_deg) {
  double wrapped = std::fmod(phase_deg + 180.0, 360.0);
  if (wrapped < 0.0) {
    wrapped += 360.0;
  }
  return wrapped - 180.0;
}

CorrectedImpedance remove_parallel_capacitance(double z_mag_ohms, double phase_deg,
                                               double frequency_hz, double parasitic_pf) {
  if (z_mag_ohms <= 0.0 || frequency_hz <= 0.0 || parasitic_pf <= 0.0 ||
      !std::isfinite(z_mag_ohms)) {
    return {z_mag_ohms, phase_deg};
  }

  const double phase_rad = phase_deg * M_PI / 180.0;
  const std::complex<double> z_meas(z_mag_ohms * std::cos(phase_rad),
                                    z_mag_ohms * std::sin(phase_rad));
  const std::complex<double> y_meas = 1.0 / z_meas;
  const std::complex<double> y_parasitic(0.0, 2.0 * M_PI * frequency_hz * parasitic_pf * 1e-12);
  const std::complex<double> y_electrode = y_meas - y_parasitic;
  if (std::abs(y_electrode) < 1e-15) {
    return {std::numeric_limits<double>::infinity(), 0.0};
  }

  const std::complex<double> z_electrode = 1.0 / y_electrode;
  return {std::abs(z_electrode), std::atan2(z_electrode.imag(), z_electrode.real()) * 180.0 / M_PI};
}

}  // namespace

// ---------------------------------------------------------------------------
// constructors: you likely won't need to change much here outside of changing
// the name to match the class
// ---------------------------------------------------------------------------
IntanRhd2132Peripheral::IntanRhd2132Peripheral(uint32_t periph_id, uint32_t peripheral_addr,
                                               zmq::context_t& ctx)
    : IntanRhd2132Peripheral(periph_id, peripheral_addr, ctx, axon::TX_SOCKET, axon::RX_SOCKET) {}

IntanRhd2132Peripheral::IntanRhd2132Peripheral(uint32_t periph_id, uint32_t peripheral_addr,
                                               zmq::context_t& ctx,
                                               const std::string& axon_tx_endpoint,
                                               const std::string& axon_rx_endpoint)
    : scifi::plugin::RecordPlugin(periph_id, MAX_SAMPLE_RATE, MAX_BIT_WIDTH, MAX_GAIN,
                                  CHANNEL_COUNT, peripheral_addr, ctx, axon_tx_endpoint,
                                  axon_rx_endpoint) {}

synapse::Peripheral IntanRhd2132Peripheral::to_proto() const {
  synapse::Peripheral p;
  p.set_name("IntanRHD2132");
  p.set_vendor("Intan Technologies");
  p.set_peripheral_id(this->id);
  p.set_type(synapse::Peripheral_Type::Peripheral_Type_kBroadbandSource);
  return p;
}

float IntanRhd2132Peripheral::get_lsb(float /*hp_corner_hz*/, float /*lp_corner_hz*/) const {
  return ADC_STEP_UV;
}

// ---------------------------------------------------------------------------
// read_frames: one SPI_LOOP_RESPONSE message = one full frame (L samples)
// ---------------------------------------------------------------------------
std::vector<axon::MyelinFrame> IntanRhd2132Peripheral::read_frames(uint32_t num_frames) {
  using namespace std::chrono;

  if (!read_enable_) {
    return {};
  }
  if (channels_enabled_ == 0) {
    spdlog::warn("IntanRhd2132: No channels enabled. Cannot read data.");
    return {};
  }
  if (num_frames == 0) {
    return {};
  }

  const uint32_t sample_rate = static_cast<uint32_t>(registers_.get_sample_rate());
  if (this->channel_ranges.empty()) {
    spdlog::warn("IntanRhd2132: No channel ranges set. Cannot read data.");
    return {};
  }

  std::vector<axon::MyelinFrame> frames(num_frames);
  int recv_fail_count = 0;
  constexpr int recv_fail_max = 3;
  uint32_t frames_filled = 0;

  while (frames_filled < num_frames && read_enable_) {
    auto pkt = receive_packet();
    if (!pkt) {
      recv_fail_count++;
      if (recv_fail_count > recv_fail_max) {
        spdlog::error("IntanRhd2132: Failed to receive from Axon RX socket. Exiting read loop.");
        break;
      }
      continue;
    }
    recv_fail_count = 0;

    // Defensive size check on payload word count.
    if (pkt->payload_size() < channels_enabled_) {
      spdlog::warn("IntanRhd2132: Undersized message ({} payload words, expected {})",
                   pkt->payload_size(), channels_enabled_);
      continue;
    }

    if (pkt->type() != SPI_LOOP_RESPONSE) {
      spdlog::warn("IntanRhd2132: Unexpected message type 0x{:04X}, expected SPI_LOOP_RESPONSE",
                   pkt->type());
      continue;
    }

    // Detect drops via seq_num gap. With one packet per iteration the gateware emits a
    // single seq_num per frame, so consecutive frames must have consecutive seq_nums.
    if (first_frame_received_ && pkt->seq_num() != last_seq_num_ + 1) {
      uint64_t lost = pkt->seq_num() - last_seq_num_ - 1;
      dropped_packets_ += lost;
      if (dropped_packets_ <= 10 || dropped_packets_ % 1000 == 0) {
        spdlog::warn("IntanRhd2132: Dropped {} frames total ({} this gap)", dropped_packets_, lost);
      }
    }
    last_seq_num_ = pkt->seq_num();
    first_frame_received_ = true;

    // Extract L 16-bit samples from the payload. Each payload word is 0x0000_RRRR.
    for (uint32_t i = 0; i < channels_enabled_; i++) {
      frame_buffer_[i] = static_cast<uint16_t>((*pkt)[i] & 0xFFFF);
    }

    axon::MyelinFrame& frame = frames[frames_filled];
    frame.set_sample_rate(sample_rate);

    shared_time_source_->update(pkt->seq_num(), sample_rate);
    frame.set_timestamp(shared_time_source_->timestamp_ns());
    frame.set_unix_timestamp_ns(scifi::get_steady_clock_now().count());
    frame.set_sequence_number(pkt->seq_num());

    std::span<uint16_t> sample_data(frame_buffer_, channels_enabled_);
    frame.set_frame_data(sample_data);
    frame.set_channel_ranges(this->channel_ranges);

    samples_delivered_ += channels_enabled_;
    frames_filled++;
  }

  if (frames_filled < num_frames) {
    frames.resize(frames_filled);
  }
  return frames;
}

axon::ChannelData IntanRhd2132Peripheral::read(uint32_t num_frames) {
  if (!read_enable_) {
    return {};
  }
  std::vector<axon::MyelinFrame> myelin_frames = read_frames(num_frames);
  return axon::to_channel_data(myelin_frames, this->channels);
}

// ---------------------------------------------------------------------------
// Recording lifecycle
// ---------------------------------------------------------------------------
scifi::Status IntanRhd2132Peripheral::start_recording(uint32_t sample_rate, uint32_t bit_width,
                                                      std::vector<synapse::Channel> channels,
                                                      float gain, float hp_corner,
                                                      float lp_corner) {
  spdlog::info("IntanRhd2132: Starting recording. SR={}, BW={}, channels={}", sample_rate,
               bit_width, channels.size());

  if (read_enable_) {
    spdlog::warn("IntanRhd2132: Recording already in progress.");
    return scifi::Status::INVALID_STATE;
  }

  // Reset the SPI controller
  scifi::Status ret = send_packet(SPI_RESET, {});
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to reset SPI controller.");
    return scifi::Status::FAILURE;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  // Configure sample rate
  double actual_sample_rate;
  ret = configure_sample_rate(sample_rate, actual_sample_rate);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to configure sample rate.");
    return ret;
  }

  // Configure bit width
  ret = configure_bit_width(bit_width);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to configure bit width.");
    return ret;
  }

  // Configure channels (sets amplifier power registers)
  ret = configure_channels(channels);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to configure channels.");
    return ret;
  }

  // Configure analog filter corners. Both must be valid (>0) to override defaults; if either
  // is unset (-1), the chip-default 1 Hz / 7.5 kHz from REG_DEFAULTS is used.
  if (hp_corner > 0 && lp_corner > 0) {
    float actual_hp_hz = 0;
    float actual_lp_hz = 0;
    ret = registers_.set_bandwidth(hp_corner, lp_corner, actual_hp_hz, actual_lp_hz);
    if (ret != scifi::Status::OK) {
      spdlog::error("IntanRhd2132: Failed to configure bandwidth.");
      return ret;
    }
  }

  // Push register configuration to hardware
  ret = push_registers_();
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to push registers.");
    return ret;
  }

  // Verify chip identity via ROM registers
  ret = verify_chip_identity_();
  if (ret != scifi::Status::OK) {
    spdlog::warn("IntanRhd2132: Chip identity verification failed. Continuing anyway.");
  }

  // Run ADC self-calibration (datasheet pg 17: required after register configuration).
  // Without this, ADC comparator offsets aren't trimmed and conversions can have systematic
  // bit-level errors.
  ret = calibrate_adc_();
  if (ret != scifi::Status::OK) {
    spdlog::warn("IntanRhd2132: ADC calibration failed. Continuing anyway.");
  }

  // Set sample period on the gateware (no SPI_RESPONSE generated by this control message)
  uint32_t period = registers_.get_sample_period();
  ret = send_packet(SET_SAMPLE_PERIOD, {period});
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to set sample period.");
    return scifi::Status::FAILURE;
  }

  // Reset drop-tracking state for a fresh loop. The gateware now drops the 2 pipeline-garbage
  // responses internally; the first SPI_LOOP_RESPONSE we receive is iteration 0.
  first_frame_received_ = false;
  last_seq_num_ = 0;
  samples_delivered_ = 0;
  dropped_packets_ = 0;

  // Subscribe + connect RX socket BEFORE starting the loop. This avoids the race where the
  // gateware emits responses before our subscription is active. The persistent subscription
  // lives until stop_recording calls unsubscribe_persistent(SPI_LOOP_RESPONSE).
  scifi::Status sub_ret = subscribe_persistent(SPI_LOOP_RESPONSE, ZMQ_RECV_TIMEOUT_MS);
  if (sub_ret != scifi::Status::OK) {
    return sub_ret;
  }

  // Drain any stale messages that may have arrived during subscription setup. drain_rx
  // restores the previous rcvtimeo for us, so no manual reset needed.
  int drained = drain_rx();
  if (drained > 0) {
    spdlog::debug("IntanRhd2132: Drained {} stale messages before starting loop", drained);
  }

  read_enable_ = true;

  // Start the acquisition loop. From here on, every SPI response we receive corresponds
  // (after the initial 2 garbage samples) to a known channel position via seq_num arithmetic.
  std::vector<uint32_t> loop_cmds = build_acquisition_loop_();
  spdlog::info("IntanRhd2132: Starting SPI_LOOP with {} commands", loop_cmds.size());
  ret = send_packet(SPI_LOOP, loop_cmds);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to start acquisition loop.");
    read_enable_ = false;
    return scifi::Status::FAILURE;
  }

  spdlog::info("IntanRhd2132: Recording started. {} channels at {:.1f} Hz", channels_enabled_,
               actual_sample_rate);
  return scifi::Status::OK;
}

scifi::Status IntanRhd2132Peripheral::stop_recording() {
  spdlog::debug("IntanRhd2132: Stopping recording");
  read_enable_ = false;

  // Stop the acquisition loop
  scifi::Status ret = send_packet(SPI_LOOP_STOP, {});
  if (ret != scifi::Status::OK) {
    spdlog::warn("IntanRhd2132: Failed to stop SPI loop.");
  }

  int packets_cleared = drain_rx();
  spdlog::debug("IntanRhd2132: Cleared {} packets from RX socket", packets_cleared);

  // Drop the persistent subscription so a re-start_recording reinstalls a fresh filter
  // (and the channel is back to a clean state for any short-scope helpers).
  unsubscribe_persistent(SPI_LOOP_RESPONSE);

  channels_enabled_ = 0;
  this->channels.clear();
  this->channel_ranges.clear();

  return scifi::Status::OK;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
scifi::Status IntanRhd2132Peripheral::configure_sample_rate(double desired_sample_rate,
                                                            double& actual_sample_rate) {
  return registers_.set_sample_rate(desired_sample_rate, actual_sample_rate);
}

scifi::Status IntanRhd2132Peripheral::configure_bit_width(uint16_t bit_width) {
  if (bit_width != 16) {
    spdlog::warn("IntanRhd2132: Only 16-bit width supported. Requested: {}", bit_width);
    return scifi::Status::INVALID_PARAMETER;
  }
  return scifi::Status::OK;
}

scifi::Status IntanRhd2132Peripheral::configure_channels(
    const std::vector<synapse::Channel>& chans) {
  if (chans.empty()) {
    spdlog::warn("IntanRhd2132: No channels provided.");
    this->channels.clear();
    this->channel_ranges.clear();
    channels_enabled_ = 0;
    registers_.set_amplifier_power(0);
    return scifi::Status::OK;
  }

  if (chans.size() > CHANNEL_COUNT) {
    spdlog::error("IntanRhd2132: Too many channels: {} (max {})", chans.size(), CHANNEL_COUNT);
    return scifi::Status::INVALID_PARAMETER;
  }

  // Build amplifier power mask from electrode IDs
  uint32_t amp_mask = 0;
  for (const auto& ch : chans) {
    uint32_t eid = ch.electrode_id();
    if (eid >= CHANNEL_COUNT) {
      spdlog::error("IntanRhd2132: Invalid electrode ID: {} (max {})", eid, CHANNEL_COUNT - 1);
      return scifi::Status::INVALID_PARAMETER;
    }
    amp_mask |= (1u << eid);
  }
  registers_.set_amplifier_power(amp_mask);

  // Store channels sorted by electrode ID
  std::vector<synapse::Channel> sorted_channels(chans.begin(), chans.end());
  std::sort(sorted_channels.begin(), sorted_channels.end(),
            [](const synapse::Channel& a, const synapse::Channel& b) {
              return a.electrode_id() < b.electrode_id();
            });

  this->channels = sorted_channels;
  channels_enabled_ = std::count_if(
      sorted_channels.begin(), sorted_channels.end(),
      [](const synapse::Channel& ch) { return ch.type() == synapse::ChannelType::ELECTRODE; });

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
const std::optional<std::string> IntanRhd2132Peripheral::validate_ephys_config(
    const synapse::BroadbandSourceConfig& config) const {
  if (config.sample_rate_hz() > MAX_SAMPLE_RATE) {
    return "IntanRhd2132: Sample rate " + std::to_string(config.sample_rate_hz()) +
           " exceeds max " + std::to_string(MAX_SAMPLE_RATE);
  }
  if (config.bit_width() != 0 && config.bit_width() != 16) {
    return "IntanRhd2132: Only 16-bit width supported.";
  }

  // Bandwidth corners come from the ElectrodeConfig's low_cutoff_hz (= HP corner) and
  // high_cutoff_hz (= LP corner). Per the datasheet on-chip register tables, the chip
  // supports HP in [0.1, 500] Hz and LP in [100, 20000] Hz. We snap to the nearest table
  // entry at start_recording time, so we only validate the requested values are inside
  // the chip's total range here.
  if (config.signal().has_electrode()) {
    const auto& ec = config.signal().electrode();
    const float hp = ec.low_cutoff_hz();
    const float lp = ec.high_cutoff_hz();
    if (hp > 0 && (hp < LOWER_BANDWIDTH_MIN_HZ || hp > LOWER_BANDWIDTH_MAX_HZ)) {
      return "IntanRhd2132: HP corner " + std::to_string(hp) + " Hz outside chip range [" +
             std::to_string(LOWER_BANDWIDTH_MIN_HZ) + ", " +
             std::to_string(LOWER_BANDWIDTH_MAX_HZ) + "]";
    }
    if (lp > 0 && (lp < UPPER_BANDWIDTH_MIN_HZ || lp > UPPER_BANDWIDTH_MAX_HZ)) {
      return "IntanRhd2132: LP corner " + std::to_string(lp) + " Hz outside chip range [" +
             std::to_string(UPPER_BANDWIDTH_MIN_HZ) + ", " +
             std::to_string(UPPER_BANDWIDTH_MAX_HZ) + "]";
    }
  }

  return std::nullopt;
}

const std::optional<std::string> IntanRhd2132Peripheral::validate_channels(
    const std::vector<synapse::Channel>& chans) const {
  for (const auto& ch : chans) {
    if (ch.electrode_id() >= CHANNEL_COUNT) {
      return "IntanRhd2132: Electrode ID " + std::to_string(ch.electrode_id()) +
             " exceeds max channel " + std::to_string(CHANNEL_COUNT - 1);
    }
  }
  if (chans.size() > CHANNEL_COUNT) {
    return "IntanRhd2132: Too many channels (" + std::to_string(chans.size()) + "), max " +
           std::to_string(CHANNEL_COUNT);
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Impedance measurement
// ---------------------------------------------------------------------------
// Drives the chip's on-chip Zcheck DAC through the selected series capacitor C_S. The timed
// loop uniformly spaces every SPI command, so DAC writes can run faster than ADC CONVERTs.
// Impedance is recovered by lock-in detection at the actual CONVERT command phases and by
// dividing the measured voltage phasor by the current phasor implied by the DAC waveform.
scifi::Status IntanRhd2132Peripheral::get_impedance(uint32_t electrode_id, float stim_freq,
                                                    float& mag, float& phase) {
  if (read_enable_) {
    spdlog::error("IntanRhd2132: Cannot measure impedance while recording.");
    return scifi::Status::INVALID_STATE;
  }
  if (electrode_id >= CHANNEL_COUNT) {
    spdlog::error("IntanRhd2132: Invalid electrode_id {} for impedance measurement", electrode_id);
    return scifi::Status::INVALID_PARAMETER;
  }
  if (stim_freq <= 0.0f) {
    spdlog::error("IntanRhd2132: stim_freq {:.0f} Hz must be positive", stim_freq);
    return scifi::Status::INVALID_PARAMETER;
  }

  constexpr uint32_t SETTLE_ITERATIONS = 20;
  constexpr uint32_t MEASURE_ITERATIONS = 200;

  static_assert(IMPEDANCE_DAC_STEPS_PER_PERIOD % IMPEDANCE_SAMPLES_PER_PERIOD == 0);
  constexpr uint32_t writes_per_sample =
      IMPEDANCE_DAC_STEPS_PER_PERIOD / IMPEDANCE_SAMPLES_PER_PERIOD;
  constexpr uint32_t loop_len = IMPEDANCE_DAC_STEPS_PER_PERIOD + IMPEDANCE_SAMPLES_PER_PERIOD;

  const double loop_time_s = loop_len * SPI_CMD_TIME_S;
  const double requested_period_s = 1.0 / static_cast<double>(stim_freq);
  if (loop_time_s > requested_period_s) {
    spdlog::error("IntanRhd2132: stim_freq {:.0f} Hz is too high for {} impedance-loop commands",
                  stim_freq, loop_len);
    return scifi::Status::INVALID_PARAMETER;
  }

  const double requested_command_hz = static_cast<double>(stim_freq) * loop_len;
  const uint32_t command_period_clkmc =
      static_cast<uint32_t>(std::round(static_cast<double>(CLKMC_FREQ_HZ) / requested_command_hz));
  if (command_period_clkmc == 0) {
    spdlog::error("IntanRhd2132: stim_freq {:.0f} Hz rounds to zero command-period cycles",
                  stim_freq);
    return scifi::Status::INVALID_PARAMETER;
  }
  const double actual_command_hz =
      static_cast<double>(CLKMC_FREQ_HZ) / static_cast<double>(command_period_clkmc);
  const double actual_stim_freq = actual_command_hz / loop_len;
  const double actual_sample_rate_hz = actual_stim_freq * IMPEDANCE_SAMPLES_PER_PERIOD;
  const AdcBias adc_bias = adc_bias_for_rate(actual_sample_rate_hz);

  spdlog::info(
      "IntanRhd2132: Impedance ch {} at {:.0f} Hz; loop_len={} cmds, command_period={} "
      "clkmc cycles, {} DAC writes/period, {} samples/period, convert rate={:.0f} Hz",
      electrode_id, actual_stim_freq, loop_len, command_period_clkmc,
      IMPEDANCE_DAC_STEPS_PER_PERIOD, IMPEDANCE_SAMPLES_PER_PERIOD, actual_sample_rate_hz);

  std::vector<uint32_t> loop_seq;
  loop_seq.reserve(loop_len);
  std::vector<uint32_t> convert_payload_indices;
  convert_payload_indices.reserve(IMPEDANCE_SAMPLES_PER_PERIOD);
  std::vector<double> convert_phases_rad;
  convert_phases_rad.reserve(IMPEDANCE_SAMPLES_PER_PERIOD);
  std::vector<double> dac_slot_values;
  dac_slot_values.reserve(loop_len);

  const uint8_t reg_dac = static_cast<uint8_t>(Register::IMPEDANCE_CHECK_DAC);
  uint8_t current_dac = IMPEDANCE_DAC_OFFSET_LSB;
  for (uint32_t k = 0; k < IMPEDANCE_DAC_STEPS_PER_PERIOD; k++) {
    const double dac_phase = 2.0 * M_PI * static_cast<double>(loop_seq.size()) / loop_len;
    const double dac_value =
        IMPEDANCE_DAC_OFFSET_LSB + IMPEDANCE_DAC_AMPLITUDE_LSB * std::sin(dac_phase);
    const int dac_val = static_cast<int>(std::round(dac_value));
    const uint8_t dac_byte = static_cast<uint8_t>(std::clamp(dac_val, 0, 255));
    current_dac = dac_byte;
    loop_seq.push_back(rhd_write(reg_dac, dac_byte));
    dac_slot_values.push_back(current_dac);

    if ((k + 1) % writes_per_sample == 0) {
      convert_payload_indices.push_back(static_cast<uint32_t>(loop_seq.size()));
      convert_phases_rad.push_back(2.0 * M_PI * static_cast<double>(loop_seq.size()) / loop_len);
      loop_seq.push_back(rhd_convert(static_cast<uint8_t>(electrode_id)));
      dac_slot_values.push_back(current_dac);
    }
  }

  if (loop_seq.size() != loop_len ||
      convert_payload_indices.size() != IMPEDANCE_SAMPLES_PER_PERIOD) {
    spdlog::error("IntanRhd2132: Internal impedance loop layout error");
    return scifi::Status::FAILURE;
  }

  double dac_cos_sum = 0.0;
  double dac_sin_sum = 0.0;
  for (uint32_t k = 0; k < loop_len; k++) {
    const double theta = 2.0 * M_PI * static_cast<double>(k) / loop_len;
    dac_cos_sum += dac_slot_values[k] * std::cos(theta);
    dac_sin_sum += dac_slot_values[k] * std::sin(theta);
  }
  const double dac_cos_lsb = (2.0 / loop_len) * dac_cos_sum;
  const double dac_sin_lsb = (2.0 / loop_len) * dac_sin_sum;
  const std::complex<double> dac_v_phasor_volts =
      std::complex<double>(dac_cos_lsb, -dac_sin_lsb) * static_cast<double>(ZCHECK_DAC_LSB_V);
  const double v_amplitude_volts = std::abs(dac_v_phasor_volts);

  std::array<uint8_t, NUM_WRITABLE_REGISTERS> prev_register_values{};
  for (uint8_t i = 0; i < NUM_WRITABLE_REGISTERS; i++) {
    prev_register_values[i] = registers_.read_register_local(static_cast<Register>(i));
  }
  auto restore_registers = [&]() {
    for (uint8_t i = 0; i < NUM_WRITABLE_REGISTERS; i++) {
      registers_.write_register_local(static_cast<Register>(i), prev_register_values[i]);
    }
    (void)push_registers_();
  };

  // Configure the full chip state the impedance loop depends on.
  constexpr uint8_t IMPEDANCE_ADC_FORMAT = 0xC0;
  registers_.write_register_local(Register::ADC_CONFIG, ADC_CONFIG_DEFAULT);
  registers_.write_register_local(Register::SUPPLY_SENSOR_ADC_BUF, adc_bias.adc_buffer_bias);
  registers_.write_register_local(Register::MUX_BIAS, adc_bias.mux_bias);
  registers_.write_register_local(Register::MUX_LOAD, 0x00);
  registers_.write_register_local(Register::ADC_OUTPUT_FORMAT, IMPEDANCE_ADC_FORMAT);
  const uint8_t zctrl = ZCHECK_REG5_DAC_PWR | ZCHECK_REG5_SCALE_0P1PF | ZCHECK_REG5_EN;
  registers_.write_register_local(Register::IMPEDANCE_CHECK_CTRL, zctrl);
  registers_.write_register_local(Register::IMPEDANCE_CHECK_DAC, IMPEDANCE_DAC_OFFSET_LSB);
  registers_.write_register_local(Register::IMPEDANCE_CHECK_SELECT,
                                  static_cast<uint8_t>(electrode_id & 0x3F));

  float actual_hp = 0, actual_lp = 0;
  (void)registers_.set_bandwidth(1.0f, 10000.0f, actual_hp, actual_lp);
  registers_.set_amplifier_power(0xFFFFFFFFu);

  // Bring the chip to a known state. get_impedance can be called without a prior
  // start_recording, so we cannot assume the chip is configured -- amplifiers may be off,
  // ADC uncalibrated, etc. Mirrors start_recording's setup so the gateware ends up in the
  // same state regardless of which path got us here.
  spdlog::debug("IntanRhd2132: Resetting and initializing chip for impedance measurement");
  scifi::Status ret = send_packet(SPI_RESET, {});
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to reset SPI controller before impedance");
    restore_registers();
    return ret;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  ret = push_registers_();
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to push registers before impedance");
    restore_registers();
    return ret;
  }
  ret = verify_chip_identity_();
  if (ret != scifi::Status::OK) {
    spdlog::warn("IntanRhd2132: Chip identity verification failed before impedance; continuing");
  }
  ret = calibrate_adc_();
  if (ret != scifi::Status::OK) {
    spdlog::warn("IntanRhd2132: ADC calibration failed before impedance; continuing");
  }

  ret = send_packet(SET_COMMAND_PERIOD,
                               {command_period_clkmc});
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to send SET_COMMAND_PERIOD for impedance");
    restore_registers();
    return ret;
  }

  // Brief settling delay for the DAC reference and electrode coupling to stabilize.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const uint32_t total_iterations = SETTLE_ITERATIONS + MEASURE_ITERATIONS;

  // Subscribe + connect RX socket BEFORE starting the loop. RAII guard cleans up on
  // every return path below.
  auto sub = subscribe(SPI_LOOP_RESPONSE, ZMQ_RECV_TIMEOUT_MS);
  if (!sub) {
    restore_registers();
    return scifi::Status::CONNECTION_FAILED;
  }
  drain_rx();

  spdlog::info(
      "IntanRhd2132: Starting impedance SPI_TIMED_LOOP with {} commands, "
      "command_period_clkmc={}",
      loop_seq.size(), command_period_clkmc);
  ret = send_packet(SPI_TIMED_LOOP, loop_seq);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to send SPI_TIMED_LOOP for impedance");
    send_packet(SPI_LOOP_STOP, {});
    restore_registers();
    return ret;
  }

  std::vector<double> samples;
  scifi::Status capture_ret =
      capture_impedance_samples_(loop_len, convert_payload_indices, total_iterations, samples);

  // Stop the loop. RX socket teardown happens in ~sub when this function returns.
  (void)send_packet(SPI_LOOP_STOP, {});

  restore_registers();

  if (capture_ret != scifi::Status::OK) {
    return capture_ret;
  }

  // Drop the settling iterations from the front, keep only complete measurement periods.
  const size_t skip_samples = SETTLE_ITERATIONS * IMPEDANCE_SAMPLES_PER_PERIOD;
  if (samples.size() < skip_samples + IMPEDANCE_SAMPLES_PER_PERIOD) {
    spdlog::error("IntanRhd2132: Impedance capture returned too few samples ({} < {})",
                  samples.size(), skip_samples + IMPEDANCE_SAMPLES_PER_PERIOD);
    return scifi::Status::FAILURE;
  }
  const size_t measure_cycles = (samples.size() - skip_samples) / IMPEDANCE_SAMPLES_PER_PERIOD;
  if (measure_cycles < 4) {
    spdlog::error("IntanRhd2132: Only {} impedance cycles available after settling",
                  measure_cycles);
    return scifi::Status::FAILURE;
  }

  std::vector<double> avg_cycle(IMPEDANCE_SAMPLES_PER_PERIOD, 0.0);
  for (size_t cycle = 0; cycle < measure_cycles; cycle++) {
    const size_t cycle_offset = skip_samples + cycle * IMPEDANCE_SAMPLES_PER_PERIOD;
    for (uint32_t k = 0; k < IMPEDANCE_SAMPLES_PER_PERIOD; k++) {
      avg_cycle[k] += samples[cycle_offset + k];
    }
  }
  for (double& sample : avg_cycle) {
    sample /= static_cast<double>(measure_cycles);
  }

  // Lock-in detection at the stim fundamental using the actual CONVERT command phases.
  double cos_sum = 0.0;
  double sin_sum = 0.0;
  for (uint32_t k = 0; k < IMPEDANCE_SAMPLES_PER_PERIOD; k++) {
    cos_sum += avg_cycle[k] * std::cos(convert_phases_rad[k]);
    sin_sum += avg_cycle[k] * std::sin(convert_phases_rad[k]);
  }
  const double real_lsb = (2.0 / IMPEDANCE_SAMPLES_PER_PERIOD) * cos_sum;
  const double sin_component_lsb = (2.0 / IMPEDANCE_SAMPLES_PER_PERIOD) * sin_sum;
  const double imag_lsb = -sin_component_lsb;
  const double resp_amplitude_lsb = std::hypot(real_lsb, imag_lsb);
  const std::complex<double> meas_v_phasor_volts =
      std::complex<double>(real_lsb, imag_lsb) * IMPEDANCE_ADC_LSB_V;
  const double resp_amplitude_v_at_input = std::abs(meas_v_phasor_volts);

  const std::complex<double> i_phasor_amps = std::complex<double>(0.0, 1.0) * 2.0 * M_PI *
                                             actual_stim_freq * ZCHECK_C_SERIES_0P1PF_F *
                                             dac_v_phasor_volts;
  const double i_stim_amplitude_a = std::abs(i_phasor_amps);
  if (i_stim_amplitude_a <= 0.0) {
    spdlog::error("IntanRhd2132: Computed zero stim current amplitude");
    return scifi::Status::FAILURE;
  }

  const std::complex<double> raw_z_complex = meas_v_phasor_volts / i_phasor_amps;
  const double lockin_phase_deg =
      std::atan2(raw_z_complex.imag(), raw_z_complex.real()) * 180.0 / M_PI;
  const double raw_phase_deg = wrap_phase_deg(lockin_phase_deg);
  const double relative_freq = actual_stim_freq / actual_sample_rate_hz;
  const double intan_mag_correction = 18.0 * relative_freq * relative_freq + 1.0;
  const double raw_z_ohms = std::abs(raw_z_complex) * intan_mag_correction;
  const CorrectedImpedance corrected_impedance = remove_parallel_capacitance(
      raw_z_ohms, raw_phase_deg, actual_stim_freq, DEFAULT_PARASITIC_CAPACITANCE_PF);

  mag = static_cast<float>(corrected_impedance.magnitude_ohms);
  phase = static_cast<float>(corrected_impedance.phase_deg);

  spdlog::info(
      "IntanRhd2132: Z(ch{}) = {:.1f} kOhm at {:.0f} Hz, phase {:.1f} deg "
      "(raw {:.1f} kOhm @ {:.1f} deg, V_amp={:.2f} LSB/{:.3f} uV, "
      "I_stim={:.3f} nA, DAC_VA={:.1f} mV, Intan corr={:.4f}x, removed {:.1f} pF, "
      "averaged {} cycles)",
      electrode_id, mag / 1e3, actual_stim_freq, phase, raw_z_ohms / 1e3, raw_phase_deg,
      resp_amplitude_lsb, resp_amplitude_v_at_input * 1e6, i_stim_amplitude_a * 1e9,
      v_amplitude_volts * 1e3, intan_mag_correction, DEFAULT_PARASITIC_CAPACITANCE_PF,
      measure_cycles);

  return scifi::Status::OK;
}

scifi::Status IntanRhd2132Peripheral::capture_impedance_samples_(
    uint32_t loop_len_words, const std::vector<uint32_t>& convert_payload_indices,
    uint32_t num_iterations, std::vector<double>& samples) {
  samples.clear();
  if (convert_payload_indices.empty()) {
    spdlog::error("IntanRhd2132: Invalid impedance capture layout with no CONVERT samples");
    return scifi::Status::INVALID_PARAMETER;
  }
  for (uint32_t payload_idx : convert_payload_indices) {
    if (payload_idx >= loop_len_words) {
      spdlog::error("IntanRhd2132: Invalid impedance CONVERT index {} for loop len {}", payload_idx,
                    loop_len_words);
      return scifi::Status::INVALID_PARAMETER;
    }
  }
  samples.reserve(convert_payload_indices.size() * num_iterations);

  uint32_t iters_received = 0;
  int recv_fail_count = 0;
  constexpr int recv_fail_max = 10;

  while (iters_received < num_iterations) {
    auto pkt = receive_packet();
    if (!pkt) {
      recv_fail_count++;
      if (recv_fail_count > recv_fail_max) {
        spdlog::error("IntanRhd2132: Impedance capture timeout after {} iterations",
                      iters_received);
        return scifi::Status::TIMEOUT;
      }
      continue;
    }
    recv_fail_count = 0;

    spdlog::debug("IntanRhd2132: Impedance rx: type=0x{:04x} src=0x{:04x} seq={} payload_words={}",
                  pkt->type(), pkt->src_addr(), pkt->seq_num(), pkt->payload_size());

    if (pkt->payload_size() < loop_len_words) {
      spdlog::warn("IntanRhd2132: Impedance message too small: {} payload words (expected {})",
                   pkt->payload_size(), loop_len_words);
      continue;
    }
    if (pkt->type() != SPI_LOOP_RESPONSE) {
      continue;
    }

    // Treat the 16-bit value as signed; get_impedance enables twos-complement output.
    for (uint32_t payload_idx : convert_payload_indices) {
      int16_t s = static_cast<int16_t>((*pkt)[payload_idx] & 0xFFFF);
      samples.push_back(static_cast<double>(s));
    }
    iters_received++;
  }
  return scifi::Status::OK;
}

synapse::QueryResponse IntanRhd2132Peripheral::self_test(const synapse::SelfTestQuery& /*query*/) {
  synapse::QueryResponse resp;
  auto* resp_st = resp.mutable_status();

  if (read_enable_) {
    spdlog::error("IntanRhd2132: Cannot run self test while recording is active");
    resp_st->set_code(synapse::StatusCode::kFailedPrecondition);
    return resp;
  }

  auto* test_response = resp.mutable_self_test_response();
  auto* identity_test = test_response->add_tests();
  identity_test->set_test_name("Chip Identity Test");

  scifi::Status ret = verify_chip_identity_();
  if (ret == scifi::Status::OK) {
    identity_test->set_passed(true);
    identity_test->set_test_report("ROM registers verified: INTAN, chip ID=0x01");
    resp_st->set_code(synapse::StatusCode::kOk);
  } else {
    identity_test->set_passed(false);
    identity_test->set_test_report("ROM register verification failed");
    resp_st->set_code(synapse::StatusCode::kInternalError);
  }
  return resp;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------
scifi::Status IntanRhd2132Peripheral::push_registers_() {
  if (read_enable_) {
    spdlog::warn("IntanRhd2132: Cannot push registers while recording is enabled");
    return scifi::Status::INVALID_STATE;
  }

  spdlog::debug("IntanRhd2132: Pushing all registers to hardware");
  std::vector<uint32_t> write_cmds = registers_.build_write_commands();
  for (const auto& cmd : write_cmds) {
    scifi::Status ret = send_packet(SPI_MSG, {cmd});
    if (ret != scifi::Status::OK) {
      spdlog::error("IntanRhd2132: Failed to push register command 0x{:04X}", cmd);
      return ret;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return scifi::Status::OK;
}

scifi::Status IntanRhd2132Peripheral::pull_registers_(bool verbose) {
  if (read_enable_) {
    spdlog::warn("IntanRhd2132: Cannot pull registers while recording is enabled");
    return scifi::Status::INVALID_STATE;
  }

  // Subscribe to SPI_RESPONSE messages that we expect to get after sending a SPI_MSG command
  // Once the variable 'sub' goes out of scope, we automatically unsubscribe from the topic
  // and perform cleanup
  auto sub = subscribe(SPI_RESPONSE, SPI_RESPONSE_TIMEOUT_MS);
  if (!sub) {
    return scifi::Status::CONNECTION_FAILED;
  }

  std::vector<uint32_t> read_cmds = registers_.build_read_commands();
  constexpr int max_retry = 3;

  for (uint8_t reg_idx = 0; reg_idx < NUM_WRITABLE_REGISTERS; reg_idx++) {
    int attempts = 0;
    bool success = false;

    while (attempts < max_retry && !success) {
      // Send SPI_MSG command
      scifi::Status send_ret =
          send_packet(SPI_MSG, {read_cmds[reg_idx]});
      if (send_ret != scifi::Status::OK) {
        spdlog::error("IntanRhd2132: Failed to send read command for register {}", reg_idx);
        attempts++;
        continue;
      }

      // Receive response
      auto pkt = receive_packet();
      if (!pkt) {
        spdlog::warn("IntanRhd2132: Timeout reading register {}", reg_idx);
        attempts++;
        continue;
      }

      if (pkt->payload_size() < 1) {
        spdlog::warn("IntanRhd2132: Unexpected response size for register read");
        attempts++;
        continue;
      }

      if (pkt->type() != SPI_RESPONSE) {
        spdlog::warn("IntanRhd2132: Unexpected response type 0x{:04X}", pkt->type());
        attempts++;
        continue;
      }

      // Parse response
      uint8_t rxd_val = static_cast<uint8_t>((*pkt)[0] & 0xFF);

      if (verbose) {
        uint8_t expected = REG_DEFAULTS[reg_idx];
        spdlog::debug("IntanRhd2132: Register {} = 0x{:02X} (expected 0x{:02X}) {}", reg_idx,
                      rxd_val, expected, (rxd_val == expected) ? "OK" : "MISMATCH");
      }

      registers_.write_register_local(static_cast<Register>(reg_idx), rxd_val);
      success = true;
    }

    if (!success) {
      spdlog::error("IntanRhd2132: Failed to read register {} after {} attempts", reg_idx,
                    max_retry);
    }
  }

  // ~sub disconnects + unsubscribes when this function returns.
  return scifi::Status::OK;
}

scifi::Status IntanRhd2132Peripheral::verify_chip_identity_() {
  // Read ROM registers 40-44 (should spell "INTAN") and register 63 (chip ID)
  auto sub = subscribe(SPI_RESPONSE, SPI_RESPONSE_TIMEOUT_MS);
  if (!sub) {
    return scifi::Status::CONNECTION_FAILED;
  }

  constexpr uint8_t rom_regs[] = {40, 41, 42, 43, 44, 63};
  constexpr uint8_t expected[] = {ROM_COMPANY_0, ROM_COMPANY_1, ROM_COMPANY_2,
                                  ROM_COMPANY_3, ROM_COMPANY_4, ROM_CHIP_ID};
  bool identity_ok = true;

  for (size_t i = 0; i < sizeof(rom_regs); i++) {
    uint32_t cmd = rhd_read(rom_regs[i]);
    scifi::Status send_ret = send_packet(SPI_MSG, {cmd});
    if (send_ret != scifi::Status::OK) {
      identity_ok = false;
      continue;
    }

    auto pkt = receive_packet();
    if (!pkt || pkt->payload_size() < 1) {
      spdlog::warn("IntanRhd2132: No response for ROM register {}", rom_regs[i]);
      identity_ok = false;
      continue;
    }

    if (pkt->type() != SPI_RESPONSE) {
      identity_ok = false;
      continue;
    }

    uint8_t val = static_cast<uint8_t>((*pkt)[0] & 0xFF);
    if (val != expected[i]) {
      spdlog::warn("IntanRhd2132: ROM register {} = 0x{:02X}, expected 0x{:02X}", rom_regs[i], val,
                   expected[i]);
      identity_ok = false;
    }
  }

  // ~sub disconnects + unsubscribes on return.
  if (identity_ok) {
    spdlog::info("IntanRhd2132: Chip identity verified (INTAN, chip ID=0x01)");
    return scifi::Status::OK;
  }
  return scifi::Status::FAILURE;
}

scifi::Status IntanRhd2132Peripheral::calibrate_adc_() {
  // Datasheet pg 17: ADC self-calibration is initiated by sending CALIBRATE (0x5500),
  // followed by 9 dummy commands which the chip ignores while it calibrates. The chip
  // returns 0x0000 (in twos-complement mode) for all 10 commands during calibration.
  spdlog::info("IntanRhd2132: Running ADC self-calibration");

  // Pack CALIBRATE + 9 NOP dummies into one SPI_MSG burst so the gateware sends them
  // back-to-back without inter-command gaps.
  std::vector<uint32_t> calib_cmds;
  calib_cmds.reserve(1 + CALIBRATE_DUMMY_COUNT);
  calib_cmds.push_back(CALIBRATE_CMD);
  for (uint8_t i = 0; i < CALIBRATE_DUMMY_COUNT; i++) {
    calib_cmds.push_back(NOP_CMD);
  }

  scifi::Status ret = send_packet(SPI_MSG, calib_cmds);
  if (ret != scifi::Status::OK) {
    spdlog::error("IntanRhd2132: Failed to send CALIBRATE command sequence");
    return ret;
  }

  // Give the chip a moment to finish; the SPI commands themselves clock the calibration
  // logic, but adding a small wait avoids any race with the next operation.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return scifi::Status::OK;
}

std::vector<uint32_t> IntanRhd2132Peripheral::build_acquisition_loop_() const {
  std::vector<uint32_t> cmds;
  cmds.reserve(channels_enabled_);

  // Build CONVERT commands for each enabled channel, in electrode_id order
  for (const auto& ch : this->channels) {
    if (ch.type() != synapse::ChannelType::ELECTRODE) {
      continue;
    }
    cmds.push_back(rhd_convert(static_cast<uint8_t>(ch.electrode_id())));
  }
  return cmds;
}

}  // namespace intan_rhd2132
}  // namespace chips
