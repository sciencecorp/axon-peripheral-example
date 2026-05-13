#include "intan_rhd2132_registers.h"

#include <cmath>

#include <spdlog/spdlog.h>

namespace intan_rhd2132 {

IntanRhd2132Registers::IntanRhd2132Registers() { reset_registers_to_default(); }

uint8_t IntanRhd2132Registers::read_register_local(Register reg) const {
  auto it = reg_map_.find(reg);
  if (it != reg_map_.end()) {
    return it->second;
  }
  uint8_t addr = static_cast<uint8_t>(reg);
  if (addr < NUM_WRITABLE_REGISTERS) {
    return REG_DEFAULTS[addr];
  }
  return 0;
}

void IntanRhd2132Registers::write_register_local(Register reg, uint8_t data) {
  spdlog::trace("RHD2132: Writing 0x{:02X} to register 0x{:02X} locally", data,
                static_cast<uint8_t>(reg));
  reg_map_[reg] = data;
}

void IntanRhd2132Registers::reset_registers_to_default() {
  reg_map_.clear();
  for (uint8_t i = 0; i < NUM_WRITABLE_REGISTERS; i++) {
    reg_map_[static_cast<Register>(i)] = REG_DEFAULTS[i];
  }
}

scifi::Status IntanRhd2132Registers::set_sample_rate(double desired_sample_rate,
                                                     double& actual_sample_rate) {
  if (desired_sample_rate <= 0 || desired_sample_rate > MAX_SAMPLE_RATE) {
    spdlog::error("RHD2132: Invalid sample rate: {:.1f} Hz (max {})", desired_sample_rate,
                  MAX_SAMPLE_RATE);
    return scifi::Status::INVALID_PARAMETER;
  }

  // sample_period in clkmc cycles. sample_rate = clkmc_freq / sample_period.
  // clkmc is hardcoded at 80 MHz for now while debugging.
  sample_period_ = static_cast<uint32_t>(CLKMC_FREQ_HZ / desired_sample_rate);
  actual_sample_rate = static_cast<double>(CLKMC_FREQ_HZ) / sample_period_;
  sample_rate_ = actual_sample_rate;

  spdlog::info(
      "RHD2132: Sample rate set to {:.1f} Hz (requested {:.1f} Hz, clkmc {} Hz, period {} cycles)",
      actual_sample_rate, desired_sample_rate, CLKMC_FREQ_HZ, sample_period_);
  return scifi::Status::OK;
}

double IntanRhd2132Registers::get_sample_rate() const { return sample_rate_; }

void IntanRhd2132Registers::set_amplifier_power(uint32_t channel_mask) {
  write_register_local(Register::AMP_PWR_0_7, static_cast<uint8_t>(channel_mask & 0xFF));
  write_register_local(Register::AMP_PWR_8_15, static_cast<uint8_t>((channel_mask >> 8) & 0xFF));
  write_register_local(Register::AMP_PWR_16_23, static_cast<uint8_t>((channel_mask >> 16) & 0xFF));
  write_register_local(Register::AMP_PWR_24_31, static_cast<uint8_t>((channel_mask >> 24) & 0xFF));
}

uint32_t IntanRhd2132Registers::get_amplifier_power() const {
  uint32_t mask = 0;
  mask |= static_cast<uint32_t>(read_register_local(Register::AMP_PWR_0_7));
  mask |= static_cast<uint32_t>(read_register_local(Register::AMP_PWR_8_15)) << 8;
  mask |= static_cast<uint32_t>(read_register_local(Register::AMP_PWR_16_23)) << 16;
  mask |= static_cast<uint32_t>(read_register_local(Register::AMP_PWR_24_31)) << 24;
  return mask;
}

namespace {
template <typename Setting, size_t N>
const Setting& closest_bandwidth(const Setting (&table)[N], float target_hz) {
  size_t best_idx = 0;
  float best_err = std::abs(table[0].corner_hz - target_hz);
  for (size_t i = 1; i < N; i++) {
    const float err = std::abs(table[i].corner_hz - target_hz);
    if (err < best_err) {
      best_err = err;
      best_idx = i;
    }
  }
  return table[best_idx];
}
}  // namespace

scifi::Status IntanRhd2132Registers::set_bandwidth(float hp_corner_hz, float lp_corner_hz,
                                                   float& actual_hp_hz, float& actual_lp_hz) {
  if (hp_corner_hz < LOWER_BANDWIDTH_MIN_HZ || hp_corner_hz > LOWER_BANDWIDTH_MAX_HZ) {
    spdlog::error("RHD2132: hp_corner {:.3f} Hz outside chip range [{:.2f}, {:.0f}]", hp_corner_hz,
                  LOWER_BANDWIDTH_MIN_HZ, LOWER_BANDWIDTH_MAX_HZ);
    return scifi::Status::INVALID_PARAMETER;
  }
  if (lp_corner_hz < UPPER_BANDWIDTH_MIN_HZ || lp_corner_hz > UPPER_BANDWIDTH_MAX_HZ) {
    spdlog::error("RHD2132: lp_corner {:.0f} Hz outside chip range [{:.0f}, {:.0f}]", lp_corner_hz,
                  UPPER_BANDWIDTH_MIN_HZ, UPPER_BANDWIDTH_MAX_HZ);
    return scifi::Status::INVALID_PARAMETER;
  }

  const auto& upper = closest_bandwidth(UPPER_BANDWIDTH_TABLE, lp_corner_hz);
  const auto& lower = closest_bandwidth(LOWER_BANDWIDTH_TABLE, hp_corner_hz);

  // Upper bandwidth: regs 8-11. Bit 7 is offchip-resistor select (0 = on-chip), bit 6 is
  // reserved (0). Reg 9/11 bit 7 is also ADC aux1/2 enable (0 = disabled).
  write_register_local(Register::RH1_DAC1, upper.rh1_dac1 & 0x3F);
  write_register_local(Register::RH1_DAC2, upper.rh1_dac2 & 0x1F);
  write_register_local(Register::RH2_DAC1, upper.rh2_dac1 & 0x3F);
  write_register_local(Register::RH2_DAC2, upper.rh2_dac2 & 0x1F);

  // Lower bandwidth: regs 12-13.
  // Reg 12 bit 7 = offchip RL (0 = on-chip), bits 6:0 = RL DAC1.
  // Reg 13 bit 7 = ADC aux3 en (0), bit 6 = RL DAC3, bits 5:0 = RL DAC2.
  write_register_local(Register::RL_DAC1, lower.rl_dac1 & 0x7F);
  const uint8_t reg13 =
      static_cast<uint8_t>(((lower.rl_dac3 & 0x01) << 6) | (lower.rl_dac2 & 0x3F));
  write_register_local(Register::RL_DAC2, reg13);

  actual_hp_hz = lower.corner_hz;
  actual_lp_hz = upper.corner_hz;

  spdlog::info("RHD2132: Bandwidth set to HP {:.2f} Hz / LP {:.0f} Hz (requested {:.2f} / {:.0f})",
               actual_hp_hz, actual_lp_hz, hp_corner_hz, lp_corner_hz);
  return scifi::Status::OK;
}

void IntanRhd2132Registers::set_dsp_enabled(bool enabled, uint8_t cutoff_variable) {
  uint8_t val = read_register_local(Register::ADC_OUTPUT_FORMAT);
  if (enabled) {
    val |= 0x10;
    val = (val & 0xF0) | (cutoff_variable & 0x0F);
  } else {
    val &= ~0x10;
  }
  write_register_local(Register::ADC_OUTPUT_FORMAT, val);
}

void IntanRhd2132Registers::set_twos_complement(bool enabled) {
  uint8_t val = read_register_local(Register::ADC_OUTPUT_FORMAT);
  if (enabled) {
    val |= 0x40;
  } else {
    val &= ~0x40;
  }
  write_register_local(Register::ADC_OUTPUT_FORMAT, val);
}

void IntanRhd2132Registers::set_fast_settle(bool enabled) {
  uint8_t val = read_register_local(Register::ADC_CONFIG);
  if (enabled) {
    val |= ADC_CONFIG_FAST_SETTLE_BIT;
  } else {
    val &= ~ADC_CONFIG_FAST_SETTLE_BIT;
  }
  write_register_local(Register::ADC_CONFIG, val);
}

void IntanRhd2132Registers::set_fpga_clk_freq_hz(uint32_t fpga_clk_freq_hz) {
  if (FPGA_CLK_FREQS_HZ.count(fpga_clk_freq_hz) == 0) {
    spdlog::error("RHD2132: Invalid FPGA clock frequency: {} Hz", fpga_clk_freq_hz);
    return;
  }
  fpga_clk_freq_hz_ = fpga_clk_freq_hz;
}

std::vector<uint32_t> IntanRhd2132Registers::build_write_commands() const {
  std::vector<uint32_t> cmds;
  cmds.reserve(NUM_WRITABLE_REGISTERS);
  for (uint8_t i = 0; i < NUM_WRITABLE_REGISTERS; i++) {
    Register reg = static_cast<Register>(i);
    auto it = reg_map_.find(reg);
    uint8_t val = (it != reg_map_.end()) ? it->second : REG_DEFAULTS[i];
    cmds.push_back(rhd_write(i, val));
  }
  return cmds;
}

std::vector<uint32_t> IntanRhd2132Registers::build_read_commands() const {
  std::vector<uint32_t> cmds;
  cmds.reserve(NUM_WRITABLE_REGISTERS);
  for (uint8_t i = 0; i < NUM_WRITABLE_REGISTERS; i++) {
    cmds.push_back(rhd_read(i));
  }
  return cmds;
}

}  // namespace intan_rhd2132
