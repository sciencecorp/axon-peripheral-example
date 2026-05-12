#pragma once

#include <cstdint>
#include <set>

namespace chips {
namespace intan_rhd2132 {

constexpr uint32_t CHANNEL_COUNT = 32;
constexpr uint32_t MAX_SAMPLE_RATE = 30000;
constexpr uint32_t MAX_BIT_WIDTH = 16;
constexpr uint32_t MAX_GAIN = 192;

constexpr uint32_t DEFAULT_FPGA_CLK_FREQ_HZ = 80000000;
const std::set<uint32_t> FPGA_CLK_FREQS_HZ = {80000000, 160000000};

// clkmc is the gateware's sample-period gating clock for the RHD2132 controller.
// Hardcoded at 80 MHz for now while debugging; independent of scifi fpga_clk_freq_hz.
constexpr uint32_t CLKMC_FREQ_HZ = 80000000;

constexpr float AMPLIFIER_GAIN_VPV = 192.0f;
constexpr float ADC_FULL_SCALE_V = 2.048f;
constexpr float ADC_STEP_UV = (ADC_FULL_SCALE_V / 65536.0f) * 1e6f / AMPLIFIER_GAIN_VPV;

constexpr uint8_t ROM_COMPANY_0 = 0x49;  // 'I'
constexpr uint8_t ROM_COMPANY_1 = 0x4E;  // 'N'
constexpr uint8_t ROM_COMPANY_2 = 0x54;  // 'T'
constexpr uint8_t ROM_COMPANY_3 = 0x41;  // 'A'
constexpr uint8_t ROM_COMPANY_4 = 0x4E;  // 'N'
constexpr uint8_t ROM_UNIPOLAR = 0x01;
constexpr uint8_t ROM_NUM_AMPS = 0x20;
constexpr uint8_t ROM_CHIP_ID = 0x01;

// ---------------------------------------------------------------------------
// SPI command builders (16-bit words)
// ---------------------------------------------------------------------------
// Command format: [opcode(2) | register/channel(6) | data(8)]
//   CONVERT: opcode=00  (read ADC channel)
//   WRITE:   opcode=10  (write register)
//   READ:    opcode=11  (read register)

inline constexpr uint16_t rhd_convert(uint8_t channel) {
  return static_cast<uint16_t>((channel & 0x3F) << 8);
}

inline constexpr uint16_t rhd_read(uint8_t reg) {
  return static_cast<uint16_t>(0xC000 | ((reg & 0x3F) << 8));
}

inline constexpr uint16_t rhd_write(uint8_t reg, uint8_t data) {
  return static_cast<uint16_t>(0x8000 | ((reg & 0x3F) << 8) | data);
}

constexpr uint16_t NOP_CMD = rhd_read(0);     // benign flush command
constexpr uint16_t CALIBRATE_CMD = 0x5500;    // 0b01010101_00000000 (datasheet pg 17)
constexpr uint16_t CLEAR_CALIB_CMD = 0x6A00;  // 0b01101010_00000000 (datasheet pg 17)
constexpr uint8_t CALIBRATE_DUMMY_COUNT = 9;  // chip ignores these while calibrating

// ---------------------------------------------------------------------------
// Axon message types (per-peripheral, same codes as gateware)
// ---------------------------------------------------------------------------
constexpr uint16_t SPI_MSG = 0x0000;
constexpr uint16_t SET_TDEST = 0x0001;
constexpr uint16_t SPI_RESET = 0x0002;
constexpr uint16_t SPI_RESPONSE = 0x0003;  // one response per packet (SPI_MSG path)
constexpr uint16_t SPI_LOOP = 0x0004;
constexpr uint16_t SPI_LOOP_STOP = 0x0005;
constexpr uint16_t SPI_LOOP_ON_TRIG = 0x0006;
constexpr uint16_t SET_SAMPLE_PERIOD = 0x0007;
constexpr uint16_t SPI_LOOP_RESPONSE = 0x0008;  // L responses per packet (SPI_LOOP path)
constexpr uint16_t SET_COMMAND_PERIOD = 0x0009;
constexpr uint16_t SPI_TIMED_LOOP = 0x000A;

// ---------------------------------------------------------------------------
// Register map
// ---------------------------------------------------------------------------
enum class Register : uint8_t {
  // Writable configuration registers (0-21)
  ADC_CONFIG = 0,
  SUPPLY_SENSOR_ADC_BUF = 1,
  MUX_BIAS = 2,
  MUX_LOAD = 3,
  ADC_OUTPUT_FORMAT = 4,
  IMPEDANCE_CHECK_CTRL = 5,
  IMPEDANCE_CHECK_DAC = 6,
  IMPEDANCE_CHECK_SELECT = 7,
  RH1_DAC1 = 8,
  RH1_DAC2 = 9,
  RH2_DAC1 = 10,
  RH2_DAC2 = 11,
  RL_DAC1 = 12,
  RL_DAC2 = 13,
  AMP_PWR_0_7 = 14,
  AMP_PWR_8_15 = 15,
  AMP_PWR_16_23 = 16,
  AMP_PWR_24_31 = 17,

  // ROM identification registers (read-only)
  ROM_COMPANY_0 = 40,
  ROM_COMPANY_1 = 41,
  ROM_COMPANY_2 = 42,
  ROM_COMPANY_3 = 43,
  ROM_COMPANY_4 = 44,
  ROM_DIE_REVISION = 60,
  ROM_UNIPOLAR = 61,
  ROM_NUM_AMPS = 62,
  ROM_CHIP_ID = 63,
};

constexpr uint8_t NUM_WRITABLE_REGISTERS = 18;
constexpr uint8_t FIRST_ROM_REGISTER = 40;

// ---------------------------------------------------------------------------
// Register 0 (ADC_CONFIG) bit fields
// ---------------------------------------------------------------------------
constexpr uint8_t ADC_CONFIG_DEFAULT = 0xDE;
//   [7:6] = 11  ADC reference bandwidth (lowest noise)
//   [5]   = 0   amp_fast_settle (normal operation)
//   [4]   = 1   amp_Vref_enable
//   [3]   = 1   ADC comparator bias (default)
//   [2]   = 1   ADC comparator select (default)
//   [1:0] = 10  reserved (must be 10)

constexpr uint8_t ADC_CONFIG_FAST_SETTLE_BIT = 0x20;

// ---------------------------------------------------------------------------
// Impedance measurement parameters
// ---------------------------------------------------------------------------
// Register 5 (Impedance Check Control) bits:
//   D[6] Zcheck DAC power
//   D[5] Zcheck load (always 0 in normal operation)
//   D[4:3] Zcheck scale: 00 = 0.1 pF, 01 = 1 pF, 11 = 10 pF
//   D[2] Zcheck conn all (always 0 unless electroplating)
//   D[1] Zcheck sel pol (RHD2216 only, always 0 for RHD2132)
//   D[0] Zcheck en
constexpr uint8_t ZCHECK_REG5_OFF = 0x00;
constexpr uint8_t ZCHECK_REG5_DAC_PWR = 0x40;
constexpr uint8_t ZCHECK_REG5_SCALE_0P1PF = 0x00;
constexpr uint8_t ZCHECK_REG5_SCALE_1PF = 0x08;
constexpr uint8_t ZCHECK_REG5_SCALE_10PF = 0x18;
constexpr uint8_t ZCHECK_REG5_EN = 0x01;

// DAC: 8-bit, output = (val/256) × 1.225 V (datasheet pg 30)
constexpr float ZCHECK_DAC_FULL_SCALE_V = 1.225f;
constexpr float ZCHECK_DAC_LSB_V = ZCHECK_DAC_FULL_SCALE_V / 256.0f;  // 4.785 mV/LSB

// Series capacitor values for the Zcheck scale settings (datasheet pg 30)
constexpr double ZCHECK_C_SERIES_0P1PF_F = 0.1e-12;
constexpr double ZCHECK_C_SERIES_1PF_F = 1.0e-12;
constexpr double ZCHECK_C_SERIES_10PF_F = 10.0e-12;

// SPI command timing (used for impedance loop sizing).
// Each 16-bit transaction takes 16 SCLKs + CSB_HI of 4 SCLKs = 20 SCLKs.
// At SCLK = SPI_CLK_FREQ_HZ, that's ~20 / SPI_CLK_FREQ_HZ seconds per command.
constexpr uint32_t SPI_CLK_FREQ_HZ = 20000000;  // 20 MHz, gateware default
constexpr float SPI_CMD_TIME_S = 20.0f / SPI_CLK_FREQ_HZ;

// ---------------------------------------------------------------------------
// Bandwidth configuration tables (datasheet pages 25-26)
// ---------------------------------------------------------------------------
// Register 8:  D[7]=offchip RH1,    D[5:0]=RH1 DAC1
// Register 9:  D[7]=ADC aux1 en,    D[4:0]=RH1 DAC2
// Register 10: D[7]=offchip RH2,    D[5:0]=RH2 DAC1
// Register 11: D[7]=ADC aux2 en,    D[4:0]=RH2 DAC2
// Register 12: D[7]=offchip RL,     D[6:0]=RL DAC1
// Register 13: D[7]=ADC aux3 en, D[6]=RL DAC3, D[5:0]=RL DAC2

struct UpperBandwidthSetting {
  float corner_hz;
  uint8_t rh1_dac1;  // reg 8 bits[5:0]
  uint8_t rh1_dac2;  // reg 9 bits[4:0]
  uint8_t rh2_dac1;  // reg 10 bits[5:0]
  uint8_t rh2_dac2;  // reg 11 bits[4:0]
};

// fH (low-pass corner / upper amplifier bandwidth), 100 Hz to 20 kHz
constexpr UpperBandwidthSetting UPPER_BANDWIDTH_TABLE[] = {
    {20000.0f, 8, 0, 4, 0},  {15000.0f, 11, 0, 8, 0}, {10000.0f, 17, 0, 16, 0},
    {7500.0f, 22, 0, 23, 0}, {5000.0f, 33, 0, 37, 0}, {3000.0f, 3, 1, 13, 1},
    {2500.0f, 13, 1, 25, 1}, {2000.0f, 27, 1, 44, 1}, {1500.0f, 1, 2, 23, 2},
    {1000.0f, 46, 2, 30, 3}, {750.0f, 41, 3, 36, 4},  {500.0f, 30, 5, 43, 6},
    {300.0f, 6, 9, 2, 11},   {250.0f, 42, 10, 5, 13}, {200.0f, 24, 13, 7, 16},
    {150.0f, 44, 17, 8, 21}, {100.0f, 38, 26, 5, 31},
};

struct LowerBandwidthSetting {
  float corner_hz;
  uint8_t rl_dac1;  // reg 12 bits[6:0]
  uint8_t rl_dac2;  // reg 13 bits[5:0]
  uint8_t rl_dac3;  // reg 13 bit[6]
};

// fL (high-pass corner / lower amplifier bandwidth), 0.1 Hz to 500 Hz
constexpr LowerBandwidthSetting LOWER_BANDWIDTH_TABLE[] = {
    {500.00f, 13, 0, 0}, {300.00f, 15, 0, 0}, {250.00f, 17, 0, 0}, {200.00f, 18, 0, 0},
    {150.00f, 21, 0, 0}, {100.00f, 25, 0, 0}, {75.00f, 28, 0, 0},  {50.00f, 34, 0, 0},
    {30.00f, 44, 0, 0},  {25.00f, 48, 0, 0},  {20.00f, 54, 0, 0},  {15.00f, 62, 0, 0},
    {10.00f, 5, 1, 0},   {7.50f, 18, 1, 0},   {5.00f, 40, 1, 0},   {3.00f, 20, 2, 0},
    {2.50f, 42, 2, 0},   {2.00f, 8, 3, 0},    {1.50f, 9, 4, 0},    {1.00f, 44, 6, 0},
    {0.75f, 49, 9, 0},   {0.50f, 35, 17, 0},  {0.30f, 1, 40, 0},   {0.25f, 56, 54, 0},
    {0.10f, 16, 60, 1},
};

constexpr float UPPER_BANDWIDTH_MIN_HZ = 100.0f;
constexpr float UPPER_BANDWIDTH_MAX_HZ = 20000.0f;
constexpr float LOWER_BANDWIDTH_MIN_HZ = 0.1f;
constexpr float LOWER_BANDWIDTH_MAX_HZ = 500.0f;

// ---------------------------------------------------------------------------
// Default register values (from Intan RHD2000 datasheet)
// ---------------------------------------------------------------------------
constexpr uint8_t REG_DEFAULTS[] = {
    0xDE,  // 0:  ADC config
    0x20,  // 1:  supply sensor off, ADC buffer bias=32
    0x28,  // 2:  MUX bias=40
    0x02,  // 3:  digout_HiZ=1
    0x40,  // 4:  twos complement (bit 6=1), DSP disabled, weak MISO off
    0x00,  // 5:  impedance check disabled
    0x00,  // 6:  impedance check DAC=0
    0x00,  // 7:  impedance check select=0
    0x16,  // 8:  RH1 DAC1 = 22 (default: 7.5 kHz upper BW)
    0x00,  // 9:  RH1 DAC2 = 0
    0x17,  // 10: RH2 DAC1 = 23 (default: 7.5 kHz upper BW)
    0x00,  // 11: RH2 DAC2 = 0
    0x2C,  // 12: RL DAC1  = 44 (default: 1.0 Hz lower BW)
    0x06,  // 13: RL DAC2  = 6, RL DAC3 = 0
    0xFF,  // 14: amps 0-7 powered on
    0xFF,  // 15: amps 8-15 powered on
    0xFF,  // 16: amps 16-23 powered on
    0xFF,  // 17: amps 24-31 powered on
};

inline constexpr const char* BASE_DIR = "data/intan_rhd2132_";

}  // namespace intan_rhd2132
}  // namespace chips
