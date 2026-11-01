#pragma once
/**
 * Meshtastic battery-level provider for a TI BQ25798 solar/MPPT charger on the main I2C bus.
 *
 * This file is #included into src/Power.cpp by scripts/apply_overlay.py (it needs the
 * file-local HasBatteryLevel class and `batteryLevel` pointer). It provides
 * bq25798Init(), which Power::setup() calls before falling back to the analog ADC.
 *
 * Everything is detected at runtime: without the charger board the node behaves exactly
 * like stock firmware.
 *
 * It also implements the power-telemetry hooks declared in BQ25798Telemetry.h, so panel,
 * battery and system voltage/current show up as Meshtastic power metrics.
 *
 * Tunables (set with -D build flags; 0 = keep the chip's PROG-pin default):
 *   HAS_BQ25798                       1 = compile the driver in (default on for the XIAO I2C kit)
 *   BQ25798_ADDR                      I2C address (0x6B)
 *   BQ25798_CHARGE_VOLTAGE_MV         battery full voltage, e.g. 4200 (Li-ion) / 3600 (LiFePO4)
 *   BQ25798_CHARGE_CURRENT_MA         max charge current
 *   BQ25798_INPUT_CURRENT_MA          max current taken from the panel
 *   BQ25798_INPUT_VOLTAGE_MV          fixed input voltage limit (only when MPPT is off)
 *   BQ25798_MIN_SYS_MV                minimum system voltage
 *   BQ25798_ENABLE_MPPT               1 = maximum power point tracking (default 1)
 *   BQ25798_IGNORE_TS                 1 = no thermistor fitted (default 0, not recommended)
 *   BQ25798_SHIP_MODE_ON_LOW_BATTERY  1 = cut power on low battery, auto-restart on sun (default 1)
 *   BQ25798_POLL_INTERVAL_MS          how often the chip is read (default 5000)
 *   BQ25798_LOG_INTERVAL_MS           how often a status line is logged (default 60000)
 */

#include "configuration.h"
#include "power/BQ25798Telemetry.h"

#if !defined(HAS_BQ25798) && defined(SEEED_XIAO_NRF_KIT_I2C)
#define HAS_BQ25798 1
#endif

#if defined(HAS_BQ25798) && HAS_BQ25798 && !defined(HAS_PMU)

#include "Observer.h"
#include "power/BQ25798.h"
#include "sleep.h"
#include <Wire.h>

#ifndef BQ25798_ADDR
#define BQ25798_ADDR 0x6B
#endif
#ifndef BQ25798_CHARGE_VOLTAGE_MV
#define BQ25798_CHARGE_VOLTAGE_MV 0
#endif
#ifndef BQ25798_CHARGE_CURRENT_MA
#define BQ25798_CHARGE_CURRENT_MA 0
#endif
#ifndef BQ25798_INPUT_CURRENT_MA
#define BQ25798_INPUT_CURRENT_MA 0
#endif
#ifndef BQ25798_INPUT_VOLTAGE_MV
#define BQ25798_INPUT_VOLTAGE_MV 0
#endif
#ifndef BQ25798_MIN_SYS_MV
#define BQ25798_MIN_SYS_MV 0
#endif
#ifndef BQ25798_ENABLE_MPPT
#define BQ25798_ENABLE_MPPT 1
#endif
#ifndef BQ25798_IGNORE_TS
#define BQ25798_IGNORE_TS 0
#endif
#ifndef BQ25798_SHIP_MODE_ON_LOW_BATTERY
#define BQ25798_SHIP_MODE_ON_LOW_BATTERY 1
#endif
#ifndef BQ25798_POLL_INTERVAL_MS
#define BQ25798_POLL_INTERVAL_MS 5000
#endif
#ifndef BQ25798_LOG_INTERVAL_MS
#define BQ25798_LOG_INTERVAL_MS 60000
#endif

/// bq25798::Bus on top of Arduino Wire
class BQ25798WireBus : public bq25798::Bus
{
  public:
    explicit BQ25798WireBus(TwoWire &wire) : wire(wire) {}

    bool readRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) override
    {
        wire.beginTransmission(addr);
        wire.write(reg);
        if (wire.endTransmission() != 0)
            return false;
        if (wire.requestFrom(addr, (uint8_t)len) != len)
            return false;
        for (size_t i = 0; i < len; i++)
            buf[i] = wire.read();
        return true;
    }

    bool writeRegs(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len) override
    {
        wire.beginTransmission(addr);
        wire.write(reg);
        for (size_t i = 0; i < len; i++)
            wire.write(buf[i]);
        return wire.endTransmission() == 0;
    }

  private:
    TwoWire &wire;
};

class BQ25798BatteryLevel : public HasBatteryLevel
{
  public:
    BQ25798BatteryLevel() : bus(Wire), charger(bus, BQ25798_ADDR) {}

    bool init()
    {
        uint8_t partInfo = 0;
        if (!charger.probe(&partInfo)) {
            LOG_DEBUG("BQ25798 not found at 0x%02x", BQ25798_ADDR);
            return false;
        }
        LOG_INFO("BQ25798 solar charger found at 0x%02x (part info 0x%02x, %u-cell)", BQ25798_ADDR, partInfo,
                 charger.cellCount());
        if (!applyConfig())
            return false;

        // Prime the ADC so the first reading is real
        charger.startAdcConversion();
        delay(150);
        present = true;
        lastPollMs = 0;
        refresh(true);

        deepSleepObserver.observe(&notifyDeepSleep);
        return true;
    }

    int getBatteryPercent() override
    {
        refresh();
        if (!haveReading)
            return -1;
        if (status.chargeState == bq25798::ChargeState::Done)
            return 100;
        static const uint16_t ocv[NUM_OCV_POINTS] = {OCV_ARRAY};
        return bq25798::ocvPercent(meas.vbatMv / NUM_CELLS, ocv, NUM_OCV_POINTS);
    }

    uint16_t getBattVoltage() override
    {
        refresh();
        return haveReading ? meas.vbatMv : 0;
    }

    bool isBatteryConnect() override
    {
        refresh();
        // Only claim a battery once we have a real voltage: reporting 0 mV for a present
        // battery would count towards Meshtastic's low-voltage shutdown.
        return present && haveReading && status.batteryPresent;
    }

    bool isVbusIn() override
    {
        refresh();
        // Power-good rather than just "voltage present": Meshtastic skips its low-battery
        // shutdown while external power is in, and a feeble dusk panel shouldn't block that.
        return present && status.powerGood;
    }

    bool isCharging() override
    {
        refresh();
        return present && bq25798::isChargingState(status.chargeState);
    }

    bool isPresent() const { return present; }

    /// Fill free power-metric channels: panel input, battery, system rail (see BQ25798Telemetry.h)
    bool getPowerMetrics(meshtastic_PowerMetrics &pm)
    {
        refresh();
        if (!present || !haveReading)
            return false;

        struct Channel {
            bool *hasV;
            float *v;
            bool *hasI;
            float *i;
        };
        Channel ch[] = {
            {&pm.has_ch1_voltage, &pm.ch1_voltage, &pm.has_ch1_current, &pm.ch1_current},
            {&pm.has_ch2_voltage, &pm.ch2_voltage, &pm.has_ch2_current, &pm.ch2_current},
            {&pm.has_ch3_voltage, &pm.ch3_voltage, &pm.has_ch3_current, &pm.ch3_current},
            {&pm.has_ch4_voltage, &pm.ch4_voltage, &pm.has_ch4_current, &pm.ch4_current},
            {&pm.has_ch5_voltage, &pm.ch5_voltage, &pm.has_ch5_current, &pm.ch5_current},
            {&pm.has_ch6_voltage, &pm.ch6_voltage, &pm.has_ch6_current, &pm.ch6_current},
            {&pm.has_ch7_voltage, &pm.ch7_voltage, &pm.has_ch7_current, &pm.ch7_current},
            {&pm.has_ch8_voltage, &pm.ch8_voltage, &pm.has_ch8_current, &pm.ch8_current},
        };
        const int numCh = sizeof(ch) / sizeof(ch[0]);

        struct Reading {
            const char *name;
            float volts;
            float milliamps;
            bool hasCurrent;
        };
        const Reading readings[] = {
            {"solar input", meas.vbusMv / 1000.0f, (float)meas.ibusMa, true},
            {"battery", meas.vbatMv / 1000.0f, (float)meas.ibatMa, true},
            {"system", meas.vsysMv / 1000.0f, 0.0f, false},
        };

        int next = 0;
        int used[3] = {-1, -1, -1};
        for (int r = 0; r < 3; r++) {
            while (next < numCh && (*ch[next].hasV || *ch[next].hasI))
                next++; // channel already taken by another power sensor (e.g. an INA219)
            if (next >= numCh)
                break;
            *ch[next].hasV = true;
            *ch[next].v = readings[r].volts;
            if (readings[r].hasCurrent) {
                *ch[next].hasI = true;
                *ch[next].i = readings[r].milliamps;
            }
            used[r] = next++;
        }

        if (!loggedChannels) {
            loggedChannels = true;
            LOG_INFO("BQ25798 power telemetry: %s=ch%d, %s=ch%d, %s=ch%d", readings[0].name, used[0] + 1, readings[1].name,
                     used[1] + 1, readings[2].name, used[2] + 1);
        }
        return used[0] >= 0;
    }

  private:
    BQ25798WireBus bus;
    bq25798::Charger charger;
    bq25798::Status status;
    bq25798::Measurements meas;
    bool present = false;
    bool haveReading = false;
    uint8_t errorCount = 0;
    uint32_t lastPollMs = 0;
    uint32_t lastLogMs = 0;
    bool loggedChannels = false;

    CallbackObserver<BQ25798BatteryLevel, void *> deepSleepObserver =
        CallbackObserver<BQ25798BatteryLevel, void *>(this, &BQ25798BatteryLevel::onDeepSleep);

    bool applyConfig()
    {
        bq25798::Config cfg;
        cfg.chargeVoltageMv = BQ25798_CHARGE_VOLTAGE_MV;
        cfg.chargeCurrentMa = BQ25798_CHARGE_CURRENT_MA;
        cfg.inputCurrentMa = BQ25798_INPUT_CURRENT_MA;
        cfg.inputVoltageMv = BQ25798_INPUT_VOLTAGE_MV;
        cfg.minSystemMv = BQ25798_MIN_SYS_MV;
        cfg.enableMppt = BQ25798_ENABLE_MPPT;
        cfg.ignoreTs = BQ25798_IGNORE_TS;
        if (!charger.configure(cfg)) {
            LOG_ERROR("BQ25798 configuration failed (bus error or unsafe charge voltage)");
            return false;
        }
        return true;
    }

    void refresh(bool force = false)
    {
        if (!present)
            return;
        const uint32_t now = millis();
        if (!force && lastPollMs && (now - lastPollMs) < BQ25798_POLL_INTERVAL_MS)
            return;
        lastPollMs = now;

        bool reset = false;
        if (!charger.wasReset(reset) || !charger.readStatus(status)) {
            // Keep the last good values; give up only after repeated failures
            if (++errorCount >= 5 && haveReading) {
                LOG_WARN("BQ25798 stopped responding");
                haveReading = false;
            }
            return;
        }
        errorCount = 0;

        if (reset) {
            LOG_WARN("BQ25798 lost its configuration (chip reset), re-applying");
            applyConfig();
        }

        // Read the result of the previous one-shot conversion, then start the next one,
        // so polling never blocks waiting for the ADC.
        if (charger.readMeasurements(meas) && meas.vbatMv > 0)
            haveReading = true;
        charger.startAdcConversion();

        if (!lastLogMs || (now - lastLogMs) >= BQ25798_LOG_INTERVAL_MS) {
            lastLogMs = now;
            LOG_INFO("BQ25798: bat %umV %dmA | in %umV %dmA%s | sys %umV | %s | die %dC%s", meas.vbatMv, meas.ibatMa,
                     meas.vbusMv, meas.ibusMa, status.powerGood ? "" : " (not good)", meas.vsysMv,
                     bq25798::chargeStateName(status.chargeState), (int)meas.dieTempC,
                     (status.fault0 || status.fault1) ? " FAULT" : "");
            if (status.tsFlags)
                LOG_WARN("BQ25798: battery temperature outside the normal range (TS flags 0x%x), charging limited",
                         status.tsFlags);
            if (status.fault0 || status.fault1)
                LOG_WARN("BQ25798: fault registers 0x%02x 0x%02x", status.fault0, status.fault1);
        }
    }

    int onDeepSleep(void *)
    {
#if BQ25798_SHIP_MODE_ON_LOW_BATTERY
        // Only for a low-battery shutdown, not when the user turns the node off: cut power
        // completely; the charger restores it (and the XIAO cold-boots) once the panel
        // delivers power again. The 10 s delay lets doDeepSleep() finish saving state.
        if (present && power && power->isLowBattery()) {
            LOG_WARN("BQ25798: low battery, entering ship mode; node restarts when solar power returns");
            charger.enterShipMode();
        }
#endif
        return 0;
    }
};

static BQ25798BatteryLevel bq25798Level;

/// Called from Power::setup(); claims the battery-level role if the charger is present.
static bool bq25798Init()
{
    if (!bq25798Level.init())
        return false;
    batteryLevel = &bq25798Level;
    return true;
}

bool bq25798HasTelemetry()
{
    return bq25798Level.isPresent();
}

bool bq25798GetPowerMetrics(meshtastic_Telemetry *m)
{
    return m && bq25798Level.getPowerMetrics(m->variant.power_metrics);
}

#else
static bool bq25798Init()
{
    return false;
}
bool bq25798HasTelemetry()
{
    return false;
}
bool bq25798GetPowerMetrics(meshtastic_Telemetry *)
{
    return false;
}
#endif
