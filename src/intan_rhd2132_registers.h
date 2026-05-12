#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "intan_rhd2132_constants.h"
#include "scifi-peripheral-sdk/scifi/status.h"

namespace chips {
namespace intan_rhd2132 {

class IntanRhd2132Registers {
 public:
  IntanRhd2132Registers();
  ~IntanRhd2132Registers() = default;

  [[nodiscard]] uint8_t read_register_local(Register reg) const;
  void write_register_local(Register reg, uint8_t data);
  void reset_registers_to_default();

  [[nodiscard]] scifi::Status set_sample_rate(double desired_sample_rate,
                                              double& actual_sample_rate);
  [[nodiscard]] double get_sample_rate() const;
  [[nodiscard]] uint32_t get_sample_period() const { return sample_period_; }

  void set_amplifier_power(uint32_t channel_mask);
  [[nodiscard]] uint32_t get_amplifier_power() const;

  // Configure the amplifier bandwidth corners. The chip only supports a discrete set of
  // values (per the datasheet lookup tables); the function picks the closest table entry
  // by absolute frequency error and writes the corresponding RH1/RH2/RL DAC bytes into
  // registers 8-13. Returns the actual corners selected (which may differ from the input
  // by up to one table-step due to discretization).
  scifi::Status set_bandwidth(float hp_corner_hz, float lp_corner_hz, float& actual_hp_hz,
                              float& actual_lp_hz);

  void set_dsp_enabled(bool enabled, uint8_t cutoff_variable = 0);
  void set_twos_complement(bool enabled);
  void set_fast_settle(bool enabled);

  void set_fpga_clk_freq_hz(uint32_t fpga_clk_freq_hz);
  [[nodiscard]] uint32_t get_fpga_clk_freq_hz() const { return fpga_clk_freq_hz_; }

  std::vector<uint32_t> build_write_commands() const;
  std::vector<uint32_t> build_read_commands() const;

 private:
  std::map<Register, uint8_t> reg_map_;
  uint32_t fpga_clk_freq_hz_ = DEFAULT_FPGA_CLK_FREQ_HZ;
  uint32_t sample_period_ = 0;
  double sample_rate_ = 0.0;
};

}  // namespace intan_rhd2132
}  // namespace chips
