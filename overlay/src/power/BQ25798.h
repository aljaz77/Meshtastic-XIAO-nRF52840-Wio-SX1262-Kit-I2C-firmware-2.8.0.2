#pragma once
/**
 * Minimal register-level driver for the TI BQ25798 buck-boost charger with MPPT.
 *
 * Deliberately free of Arduino / Meshtastic dependencies: all I2C goes through the
 * small bq25798::Bus interface, so the logic can be unit-tested on a PC with a
 * simulated chip (see tests/test_bq25798.cpp in the overlay repository).
 *
 * Register map: TI datasheet SLUSE22, cross-checked against
 *   https://github.com/adafruit/Adafruit_bq25798 and
 *   https://github.com/misch2/BQ25798_Library
 * 16-bit registers are big-endian (lower address = MSB) and auto-increment on reads.
 *
 * Constants use kCamelCase on purpose: board variant headers #define plenty of UPPER_CASE
 * names (the XIAO's defines ADC_CTRL, for example), which would silently clobber them.
 */

#include <stddef.h>
#include <stdint.h>

namespace bq25798
{

constexpr uint8_t kDefaultAddr = 0x6B; // fixed, the BQ25798 has no address pin

namespace reg
{
constexpr uint8_t kMinSysVoltage = 0x00;  // VSYSMIN[5:0], 250 mV/LSB + 2500 mV
constexpr uint8_t kChargeVoltage = 0x01;   // 16-bit, VREG[10:0], 10 mV/LSB
constexpr uint8_t kChargeCurrent = 0x03;   // 16-bit, ICHG[8:0], 10 mA/LSB
constexpr uint8_t kInputVoltage = 0x05;    // VINDPM[7:0], 100 mV/LSB
constexpr uint8_t kInputCurrent = 0x06;    // 16-bit, IINDPM[8:0], 10 mA/LSB
constexpr uint8_t kTerminationCtrl = 0x09; // REG_RST = bit 6
constexpr uint8_t kRechargeCtrl = 0x0A;    // CELL[7:6]
constexpr uint8_t kChargerCtrl0 = 0x0F;   // EN_CHG = bit 5
constexpr uint8_t kChargerCtrl1 = 0x10;   // WD_RST = bit 3, WATCHDOG[2:0]
constexpr uint8_t kChargerCtrl2 = 0x11;   // SDRV_CTRL[2:1], SDRV_DLY = bit 0
constexpr uint8_t kChargerCtrl5 = 0x14;   // EN_IBAT = bit 5
constexpr uint8_t kMpptCtrl = 0x15;        // EN_MPPT = bit 0
constexpr uint8_t kNtcCtrl1 = 0x18;       // TS_IGNORE = bit 0
constexpr uint8_t kStatus0 = 0x1B;         // PG_STAT = bit 3, VBUS_PRESENT_STAT = bit 0
constexpr uint8_t kStatus1 = 0x1C;         // CHG_STAT[7:5], VBUS_STAT[4:1]
constexpr uint8_t kStatus2 = 0x1D;         // VBAT_PRESENT_STAT = bit 0
constexpr uint8_t kStatus4 = 0x1F;         // TS_COLD/COOL/WARM/HOT = bits 3..0
constexpr uint8_t kFault0 = 0x20;
constexpr uint8_t kFault1 = 0x21;
constexpr uint8_t kAdcCtrl = 0x2E;  // ADC_EN = bit 7, ADC_RATE = bit 6, ADC_SAMPLE[5:4]
constexpr uint8_t kAdcDis1 = 0x30; // DPLUS_ADC_DIS = bit 7, DMINUS_ADC_DIS = bit 6
constexpr uint8_t kIbusAdc = 0x31;  // signed, 1 mA/LSB
constexpr uint8_t kIbatAdc = 0x33;  // signed, 1 mA/LSB (+ = charging)
constexpr uint8_t kVbusAdc = 0x35;  // 1 mV/LSB
constexpr uint8_t kVac1Adc = 0x37;
constexpr uint8_t kVac2Adc = 0x39;
constexpr uint8_t kVbatAdc = 0x3B;
constexpr uint8_t kVsysAdc = 0x3D;
constexpr uint8_t kTsAdc = 0x3F;   // 0.0976563 %/LSB
constexpr uint8_t kTdieAdc = 0x41; // signed, 0.5 degC/LSB
constexpr uint8_t kPartInfo = 0x48; // PN[5:3] = 3 for BQ25798, DEV_REV[2:0]
} // namespace reg

constexpr uint8_t kPartNumberBq25798 = 3;

enum class ChargeState : uint8_t {
    NotCharging = 0,
    Trickle = 1,
    Precharge = 2,
    FastCC = 3,
    TaperCV = 4,
    Reserved = 5,
    TopOff = 6,
    Done = 7,
};

inline const char *chargeStateName(ChargeState s)
{
    switch (s) {
    case ChargeState::NotCharging:
        return "Not charging";
    case ChargeState::Trickle:
        return "Trickle charge";
    case ChargeState::Precharge:
        return "Pre-charge";
    case ChargeState::FastCC:
        return "Fast charge (CC)";
    case ChargeState::TaperCV:
        return "Taper charge (CV)";
    case ChargeState::TopOff:
        return "Top-off";
    case ChargeState::Done:
        return "Charge done";
    default:
        return "Reserved";
    }
}

inline bool isChargingState(ChargeState s)
{
    return s == ChargeState::Trickle || s == ChargeState::Precharge || s == ChargeState::FastCC ||
           s == ChargeState::TaperCV || s == ChargeState::TopOff;
}

/// Settings applied on init (and re-applied if the chip resets). 0 = keep the hardware default
/// set by the PROG resistor, which is the safest choice if you are unsure.
struct Config {
    uint16_t chargeVoltageMv = 0; // VREG, battery full voltage (pack, not per cell)
    uint16_t chargeCurrentMa = 0; // ICHG
    uint16_t inputCurrentMa = 0;  // IINDPM, max current drawn from the solar panel / adapter
    uint16_t inputVoltageMv = 0;  // VINDPM, only used when MPPT is off
    uint16_t minSystemMv = 0;     // VSYSMIN
    bool enableMppt = true;       // let the chip track the solar panel's maximum power point
    bool ignoreTs = false;        // only if there is NO thermistor on the TS pin (unsafe outdoors)
};

struct Status {
    bool vbusPresent = false;    // input (solar/USB) voltage present
    bool powerGood = false;      // input is good enough to be used
    bool batteryPresent = false; // VBAT above VBAT_UVLOZ
    ChargeState chargeState = ChargeState::NotCharging;
    uint8_t vbusType = 0;    // VBUS_STAT[3:0]
    uint8_t tsFlags = 0;     // STATUS_4[3:0]: bit3 cold, bit2 cool, bit1 warm, bit0 hot
    uint8_t fault0 = 0;      // non-zero = a fault is active
    uint8_t fault1 = 0;
};

struct Measurements {
    uint16_t vbatMv = 0;
    uint16_t vsysMv = 0;
    uint16_t vbusMv = 0;
    uint16_t vac1Mv = 0;
    uint16_t vac2Mv = 0;
    int16_t ibatMa = 0; // + charging, - discharging
    int16_t ibusMa = 0; // current drawn from the input
    float dieTempC = 0;
    float tsPercent = 0; // TS pin voltage as % of REGN (thermistor divider)
};

/// Abstract I2C transport. Return false on any bus error / NACK.
class Bus
{
  public:
    virtual ~Bus() = default;
    virtual bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) = 0;
    virtual bool writeRegs(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len) = 0;
};

/// Percent (0..100) from per-cell voltage using a descending open-circuit-voltage table
/// (same format as Meshtastic's OCV_ARRAY: index 0 = 100 %, last index = 0 %).
inline int ocvPercent(uint16_t cellMv, const uint16_t *ocv, int points)
{
    if (points < 2)
        return -1;
    if (cellMv >= ocv[0])
        return 100;
    if (cellMv <= ocv[points - 1])
        return 0;
    for (int i = 1; i < points; i++) {
        if (cellMv >= ocv[i]) {
            const float stepPct = 100.0f / (points - 1);
            const float frac = (float)(cellMv - ocv[i]) / (float)(ocv[i - 1] - ocv[i]);
            return (int)((points - 1 - i) * stepPct + frac * stepPct + 0.5f);
        }
    }
    return 0;
}

class Charger
{
  public:
    explicit Charger(Bus &bus, uint8_t addr = kDefaultAddr) : bus(bus), addr(addr) {}

    /// True if a BQ25798 answers at the address. Also distinguishes it from other parts
    /// sharing 0x6B (BQ25896, QMI8658, SEN6x...) via the part-number field.
    bool probe(uint8_t *partInfoOut = nullptr)
    {
        uint8_t info;
        if (!read8(reg::kPartInfo, info))
            return false;
        if (partInfoOut)
            *partInfoOut = info;
        return ((info >> 3) & 0x07) == kPartNumberBq25798;
    }

    /// Number of series cells as configured by the PROG pin (1..4), 0 on error.
    uint8_t cellCount()
    {
        uint8_t v;
        if (!read8(reg::kRechargeCtrl, v))
            return 0;
        return ((v >> 6) & 0x03) + 1;
    }

    /// Apply cfg. Returns false on a bus error or if a value is unsafe for the cell count.
    bool configure(const Config &cfg)
    {
        const uint8_t cells = cellCount();
        if (cells == 0)
            return false;

        if (cfg.chargeVoltageMv) {
            // Refuse anything above 4.4 V/cell: no lithium chemistry is charged higher, and a
            // wrong build flag must never be able to overcharge a pack.
            if (cfg.chargeVoltageMv < 3000 || cfg.chargeVoltageMv > 4400u * cells)
                return false;
            if (!write16(reg::kChargeVoltage, (cfg.chargeVoltageMv / 10) & 0x07FF))
                return false;
        }
        if (cfg.chargeCurrentMa) {
            if (!write16(reg::kChargeCurrent, (clamp(cfg.chargeCurrentMa, 50, 5000) / 10) & 0x01FF))
                return false;
        }
        if (cfg.inputCurrentMa) {
            if (!write16(reg::kInputCurrent, (clamp(cfg.inputCurrentMa, 100, 3300) / 10) & 0x01FF))
                return false;
        }
        if (cfg.inputVoltageMv) {
            if (!write8(reg::kInputVoltage, (uint8_t)(clamp(cfg.inputVoltageMv, 3600, 22000) / 100)))
                return false;
        }
        if (cfg.minSystemMv) {
            const uint16_t mv = clamp(cfg.minSystemMv, 2500, 16000);
            if (!updateBits(reg::kMinSysVoltage, 0x3F, (uint8_t)((mv - 2500) / 250)))
                return false;
        }

        return
            // Disable the I2C watchdog: otherwise the chip reverts to defaults ~40 s after the
            // MCU stops talking to it (e.g. while it sleeps). We detect real resets instead.
            updateBits(reg::kChargerCtrl1, 0x07, 0x00) &&
            updateBits(reg::kMpptCtrl, 0x01, cfg.enableMppt ? 0x01 : 0x00) &&
            updateBits(reg::kNtcCtrl1, 0x01, cfg.ignoreTs ? 0x01 : 0x00) &&
            // Measure discharge current too, not only charge current
            updateBits(reg::kChargerCtrl5, 0x20, 0x20) &&
            // Skip the USB D+/D- channels (unused) to shorten ADC conversions
            updateBits(reg::kAdcDis1, 0xC0, 0xC0) &&
            // Make sure charging is enabled
            updateBits(reg::kChargerCtrl0, 0x20, 0x20);
    }

    /// True if the chip lost our configuration (power-on reset, REG_RST, battery swap...).
    /// configure() sets WATCHDOG=0; any non-zero value means it came back with defaults.
    bool wasReset(bool &reset)
    {
        uint8_t v;
        if (!read8(reg::kChargerCtrl1, v))
            return false;
        reset = (v & 0x07) != 0;
        return true;
    }

    /// Start one ADC conversion of all channels (14-bit, ~100 ms). The chip clears ADC_EN
    /// when done; results stay readable until the next conversion. One-shot instead of
    /// continuous saves several hundred uA on a battery-powered node.
    bool startAdcConversion() { return write8(reg::kAdcCtrl, 0x80 | 0x40 | (1 << 4)); }

    bool readStatus(Status &s)
    {
        uint8_t st[5]; // STATUS_0 .. STATUS_4
        uint8_t f[2];
        if (!bus.readRegs(addr, reg::kStatus0, st, sizeof(st)) || !bus.readRegs(addr, reg::kFault0, f, sizeof(f)))
            return false;
        s.vbusPresent = st[0] & 0x01;
        s.powerGood = st[0] & 0x08;
        s.chargeState = (ChargeState)((st[1] >> 5) & 0x07);
        s.vbusType = (st[1] >> 1) & 0x0F;
        s.batteryPresent = st[2] & 0x01;
        s.tsFlags = st[4] & 0x0F;
        s.fault0 = f[0];
        s.fault1 = f[1];
        return true;
    }

    bool readMeasurements(Measurements &m)
    {
        uint8_t b[0x43 - reg::kIbusAdc]; // IBUS_ADC (0x31) .. TDIE_ADC (0x41/0x42)
        if (!bus.readRegs(addr, reg::kIbusAdc, b, sizeof(b)))
            return false;
        auto u16 = [&](uint8_t r) { return (uint16_t)((b[r - reg::kIbusAdc] << 8) | b[r - reg::kIbusAdc + 1]); };
        m.ibusMa = (int16_t)u16(reg::kIbusAdc);
        m.ibatMa = (int16_t)u16(reg::kIbatAdc);
        m.vbusMv = u16(reg::kVbusAdc);
        m.vac1Mv = u16(reg::kVac1Adc);
        m.vac2Mv = u16(reg::kVac2Adc);
        m.vbatMv = u16(reg::kVbatAdc);
        m.vsysMv = u16(reg::kVsysAdc);
        m.tsPercent = u16(reg::kTsAdc) * 0.0976563f;
        m.dieTempC = (int16_t)u16(reg::kTdieAdc) * 0.5f;
        return true;
    }

    /// Cut power to the whole system (BATFET off) after a 10 s delay. The chip leaves ship
    /// mode by itself when an input (solar panel) is connected or QON is pressed, so a node
    /// that ran flat overnight cold-boots again when the sun comes up.
    bool enterShipMode() { return updateBits(reg::kChargerCtrl2, 0x07, (0x02 << 1) | 0x00); }

  private:
    Bus &bus;
    uint8_t addr;

    static uint16_t clamp(uint16_t v, uint16_t lo, uint16_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

    bool read8(uint8_t r, uint8_t &v) { return bus.readRegs(addr, r, &v, 1); }
    bool write8(uint8_t r, uint8_t v) { return bus.writeRegs(addr, r, &v, 1); }
    bool write16(uint8_t r, uint16_t v)
    {
        const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)(v & 0xFF)};
        return bus.writeRegs(addr, r, b, 2);
    }
    bool updateBits(uint8_t r, uint8_t mask, uint8_t value)
    {
        uint8_t v;
        if (!read8(r, v))
            return false;
        const uint8_t nv = (uint8_t)((v & ~mask) | (value & mask));
        return nv == v ? true : write8(r, nv);
    }
};

} // namespace bq25798
