/**
 * Host unit tests for the BQ25798 overlay: runs the real driver and the real Meshtastic
 * adapter code against a simulated chip. No hardware needed.
 *
 *   g++ -std=c++17 -Wall -Wextra -Werror -I tests/stubs -I overlay/src tests/test_bq25798.cpp -o test_bq25798
 *   ./test_bq25798
 *
 * CI also builds it with the board's real variant.h force-included (-include variant.h),
 * so a macro in the board headers that clashes with a driver name fails here, fast.
 */
#include <cmath>
#include <cstring>

#include "Arduino.h"
#include "Wire.h"

// ---------------------------------------------------------------------------------------
// Recreate the bits of src/Power.cpp that the adapter header expects to find around it
// ---------------------------------------------------------------------------------------
class HasBatteryLevel
{
  public:
    virtual ~HasBatteryLevel() = default;
    virtual int getBatteryPercent() { return -1; }
    virtual uint16_t getBattVoltage() { return 0; }
    virtual bool isBatteryConnect() { return false; }
    virtual bool isVbusIn() { return false; }
    virtual bool isCharging() { return false; }
};
static HasBatteryLevel *batteryLevel;

class Power
{
  public:
    bool lowBattery = false;
    bool isLowBattery() { return lowBattery; }
};
Power fakePower;
Power *power = &fakePower;

uint32_t fakeMillis = 1000;
std::vector<std::string> logLines;
TwoWire Wire;
#include "sleep.h"
Observable<void *> notifyDeepSleep;

#ifndef SEEED_XIAO_NRF_KIT_I2C
#define SEEED_XIAO_NRF_KIT_I2C // as in the real env: driver enabled by default
#endif
#include "power/BQ25798BatteryLevel.h"

// ---------------------------------------------------------------------------------------
// Simulated chip
// ---------------------------------------------------------------------------------------
struct FakeBQ25798 : TwoWire::Device {
    uint8_t regs[256];
    bool online = true;
    std::vector<std::pair<uint8_t, uint8_t>> writes;

    FakeBQ25798() { powerOnReset(); }
    void powerOnReset()
    {
        memset(regs, 0, sizeof(regs));
        regs[0x00] = 0x04;                   // VSYSMIN 3.5 V
        set16(0x01, 420);                    // VREG 4.20 V
        set16(0x03, 100);                    // ICHG 1 A
        regs[0x05] = 36;                     // VINDPM 3.6 V
        set16(0x06, 300);                    // IINDPM 3 A
        regs[0x0A] = 0x23;                   // CELL = 1S
        regs[0x0F] = 0xA2;                   // EN_CHG set
        regs[0x10] = 0x85;                   // WATCHDOG = 40 s (default)
        regs[0x11] = 0x78;                   // SDRV_CTRL idle
        regs[0x14] = 0x16;                   // EN_IBAT off
        regs[0x15] = 0xAA;                   // MPPT off
        regs[0x18] = 0x54;                   // TS_IGNORE off
        regs[0x2E] = 0x30;
        regs[0x48] = 0x19;                   // PN = 3 (BQ25798), rev 1
    }
    void set16(uint8_t r, uint16_t v)
    {
        regs[r] = v >> 8;
        regs[r + 1] = v & 0xFF;
    }
    uint16_t get16(uint8_t r) const { return (uint16_t)((regs[r] << 8) | regs[r + 1]); }
    bool ack() override { return online; }
    uint8_t readReg(uint8_t r) override { return regs[r]; }
    void writeReg(uint8_t r, uint8_t v) override
    {
        writes.push_back({r, v});
        regs[r] = v;
    }
};

/// bq25798::Bus straight onto the fake, for driver-level tests
struct DirectBus : bq25798::Bus {
    FakeBQ25798 &chip;
    explicit DirectBus(FakeBQ25798 &c) : chip(c) {}
    bool readRegs(uint8_t addr, uint8_t r, uint8_t *buf, size_t len) override
    {
        if (!chip.online || addr != 0x6B)
            return false;
        for (size_t i = 0; i < len; i++)
            buf[i] = chip.readReg((uint8_t)(r + i));
        return true;
    }
    bool writeRegs(uint8_t addr, uint8_t r, const uint8_t *buf, size_t len) override
    {
        if (!chip.online || addr != 0x6B)
            return false;
        for (size_t i = 0; i < len; i++)
            chip.writeReg((uint8_t)(r + i), buf[i]);
        return true;
    }
};

// ---------------------------------------------------------------------------------------
static int failures = 0, checks = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) {                                                                                                 \
            failures++;                                                                                                \
            printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                  \
        }                                                                                                              \
    } while (0)
#define CHECK_EQ(a, b)                                                                                                 \
    do {                                                                                                               \
        checks++;                                                                                                      \
        long _a = (long)(a), _b = (long)(b);                                                                           \
        if (_a != _b) {                                                                                                \
            failures++;                                                                                                \
            printf("  FAIL  %s:%d  %s == %s  (got %ld / 0x%lx, expected %ld / 0x%lx)\n", __FILE__, __LINE__, #a, #b, _a, \
                   _a, _b, _b);                                                                                        \
        }                                                                                                              \
    } while (0)
#define SECTION(name) printf("- %s\n", name)

static void setAdc(FakeBQ25798 &c, uint16_t vbat, int16_t ibat, uint16_t vbus, int16_t ibus)
{
    c.set16(0x3B, vbat);
    c.set16(0x33, (uint16_t)ibat);
    c.set16(0x35, vbus);
    c.set16(0x31, (uint16_t)ibus);
    c.set16(0x37, vbus); // VAC1
    c.set16(0x3D, vbat + 10);
    c.set16(0x41, 63);   // 31.5 degC
    c.set16(0x3F, 512);  // 50 %
}

// ---------------------------------------------------------------------------------------
static void testDriver()
{
    using namespace bq25798;

    SECTION("probe / part number");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        uint8_t info = 0;
        CHECK(ch.probe(&info));
        CHECK_EQ(info, 0x19);
        c.regs[0x48] = 0x00; // what a BQ25896 / other 0x6B device could return
        CHECK(!ch.probe());
        c.regs[0x48] = 0x19;
        c.online = false;
        CHECK(!ch.probe());
    }

    SECTION("cell count from PROG");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        CHECK_EQ(ch.cellCount(), 1);
        c.regs[0x0A] = 0x40 | 0x23;
        CHECK_EQ(ch.cellCount(), 2);
        c.regs[0x0A] = 0xC0 | 0x23;
        CHECK_EQ(ch.cellCount(), 4);
    }

    SECTION("configure with defaults touches only what it must");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        CHECK(ch.configure(Config{}));
        CHECK_EQ(c.regs[0x10], 0x80);       // watchdog off, other bits kept
        CHECK_EQ(c.regs[0x15], 0xAB);       // EN_MPPT on, VOC settings kept
        CHECK_EQ(c.regs[0x14], 0x36);       // EN_IBAT on
        CHECK_EQ(c.regs[0x30] & 0xC0, 0xC0); // D+/D- ADC off
        CHECK_EQ(c.regs[0x0F] & 0x20, 0x20); // charging enabled
        CHECK_EQ(c.regs[0x18], 0x54);       // TS not ignored
        CHECK_EQ(c.get16(0x01), 420);        // VREG untouched
        CHECK_EQ(c.get16(0x03), 100);        // ICHG untouched
        CHECK_EQ(c.regs[0x00], 0x04);        // VSYSMIN untouched
        for (auto &w : c.writes)
            CHECK(w.first != 0x01 && w.first != 0x02 && w.first != 0x03 && w.first != 0x04);
    }

    SECTION("configure explicit values: register encoding");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        Config cfg;
        cfg.chargeVoltageMv = 4100;
        cfg.chargeCurrentMa = 500;
        cfg.inputCurrentMa = 1500;
        cfg.inputVoltageMv = 5000;
        cfg.minSystemMv = 3250;
        cfg.enableMppt = false;
        cfg.ignoreTs = true;
        c.regs[0x00] = 0xC4; // reserved upper bits must survive
        CHECK(ch.configure(cfg));
        CHECK_EQ(c.get16(0x01), 410);
        CHECK_EQ(c.get16(0x03), 50);
        CHECK_EQ(c.get16(0x06), 150);
        CHECK_EQ(c.regs[0x05], 50);
        CHECK_EQ(c.regs[0x00], 0xC3);
        CHECK_EQ(c.regs[0x15] & 0x01, 0);
        CHECK_EQ(c.regs[0x18] & 0x01, 1);
    }

    SECTION("out-of-range values are clamped");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        Config cfg;
        cfg.chargeCurrentMa = 9000;
        cfg.inputCurrentMa = 20;
        CHECK(ch.configure(cfg));
        CHECK_EQ(c.get16(0x03), 500); // 5 A max
        CHECK_EQ(c.get16(0x06), 10);  // 100 mA min
    }

    SECTION("refuses an over-voltage charge setting for the cell count");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        Config cfg;
        cfg.chargeVoltageMv = 8400; // 2S value on a 1S-configured chip
        CHECK(!ch.configure(cfg));
        CHECK_EQ(c.get16(0x01), 420);
        CHECK(c.writes.empty());
        c.regs[0x0A] = 0x40 | 0x23; // now 2S
        CHECK(ch.configure(cfg));
        CHECK_EQ(c.get16(0x01), 840);
    }

    SECTION("reset detection via watchdog bits");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        bool reset = false;
        CHECK(ch.wasReset(reset));
        CHECK(reset); // fresh chip = defaults
        CHECK(ch.configure(Config{}));
        CHECK(ch.wasReset(reset));
        CHECK(!reset);
        c.powerOnReset();
        CHECK(ch.wasReset(reset));
        CHECK(reset);
    }

    SECTION("ADC readout (big-endian, signed currents, scaling)");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        setAdc(c, 3912, -120, 18230, 210);
        Measurements m;
        CHECK(ch.readMeasurements(m));
        CHECK_EQ(m.vbatMv, 3912);
        CHECK_EQ(m.ibatMa, -120);
        CHECK_EQ(m.vbusMv, 18230);
        CHECK_EQ(m.ibusMa, 210);
        CHECK_EQ(m.vac1Mv, 18230);
        CHECK_EQ(m.vsysMv, 3922);
        CHECK(std::fabs(m.dieTempC - 31.5f) < 0.01f);
        CHECK(std::fabs(m.tsPercent - 50.0f) < 0.05f);
        c.set16(0x41, (uint16_t)-20); // -10 degC
        CHECK(ch.readMeasurements(m));
        CHECK(std::fabs(m.dieTempC + 10.0f) < 0.01f);
    }

    SECTION("status decoding");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        c.regs[0x1B] = 0x09;                    // PG + VBUS present
        c.regs[0x1C] = (3 << 5) | (0x0B << 1); // fast charge CC, VBUS_STAT 0xB
        c.regs[0x1D] = 0x01;                    // battery present
        c.regs[0x1F] = 0x08;                    // TS cold
        c.regs[0x20] = 0x40;
        Status s;
        CHECK(ch.readStatus(s));
        CHECK(s.vbusPresent);
        CHECK(s.powerGood);
        CHECK(s.batteryPresent);
        CHECK(s.chargeState == ChargeState::FastCC);
        CHECK_EQ(s.vbusType, 0x0B);
        CHECK_EQ(s.tsFlags, 0x08);
        CHECK_EQ(s.fault0, 0x40);
        CHECK(isChargingState(ChargeState::TaperCV));
        CHECK(!isChargingState(ChargeState::Done));
        CHECK(!isChargingState(ChargeState::NotCharging));
        CHECK(strcmp(chargeStateName(ChargeState::Done), "Charge done") == 0);
    }

    SECTION("ADC one-shot trigger and ship mode");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        CHECK(ch.startAdcConversion());
        CHECK_EQ(c.regs[0x2E], 0xD0); // ADC_EN | one-shot | 14-bit
        CHECK(ch.enterShipMode());
        CHECK_EQ(c.regs[0x11], 0x7C); // SDRV_CTRL = ship, SDRV_DLY = 10 s, upper bits kept
    }

    SECTION("bus error during configure is reported");
    {
        FakeBQ25798 c;
        DirectBus b(c);
        Charger ch(b);
        c.online = false;
        CHECK(!ch.configure(Config{}));
    }

    SECTION("OCV percent (Meshtastic default Li-ion curve)");
    {
        const uint16_t ocv[11] = {4190, 4050, 3990, 3890, 3800, 3720, 3630, 3530, 3420, 3300, 3100};
        CHECK_EQ(ocvPercent(4250, ocv, 11), 100);
        CHECK_EQ(ocvPercent(4190, ocv, 11), 100);
        CHECK_EQ(ocvPercent(3800, ocv, 11), 60);
        CHECK_EQ(ocvPercent(3850, ocv, 11), 66);
        CHECK_EQ(ocvPercent(3100, ocv, 11), 0);
        CHECK_EQ(ocvPercent(2900, ocv, 11), 0);
        int last = 101;
        for (uint16_t mv = 3000; mv <= 4300; mv += 5) {
            int p = ocvPercent(mv, ocv, 11);
            CHECK(p >= 0 && p <= 100);
            if (last != 101)
                CHECK(p >= last); // monotonic
            last = p;
        }
    }
}

// ---------------------------------------------------------------------------------------
static void testMeshtasticAdapter()
{
    SECTION("adapter: not detected -> falls through to stock behaviour");
    {
        FakeBQ25798 other;
        other.regs[0x48] = 0x00; // some other device at 0x6B
        Wire.dev = &other;
        Wire.devAddr = 0x6B;
        BQ25798BatteryLevel probeOnly;
        CHECK(!probeOnly.init());
        CHECK(other.writes.empty()); // never touches a chip it doesn't recognise
        Wire.dev = nullptr;
        CHECK(!probeOnly.init());
    }

    FakeBQ25798 chip;
    Wire.dev = &chip;
    Wire.devAddr = 0x6B;
    setAdc(chip, 3912, 250, 18230, 90);
    chip.regs[0x1B] = 0x09;
    chip.regs[0x1C] = 3 << 5;
    chip.regs[0x1D] = 0x01;

    SECTION("adapter: init claims the battery-level role and configures the chip");
    batteryLevel = nullptr;
    CHECK(bq25798Init());
    CHECK(batteryLevel == &bq25798Level);
    CHECK_EQ(chip.regs[0x10] & 0x07, 0);
    CHECK_EQ(chip.regs[0x15] & 0x01, 1);
    CHECK_EQ(chip.regs[0x2E], 0xD0);
    CHECK_EQ(notifyDeepSleep.observers.size(), 1);

    SECTION("adapter: readings");
    CHECK_EQ(batteryLevel->getBattVoltage(), 3912);
    CHECK(batteryLevel->isBatteryConnect());
    CHECK(batteryLevel->isVbusIn());
    CHECK(batteryLevel->isCharging());
    CHECK_EQ(batteryLevel->getBatteryPercent(), 72);
    bool logged = false;
    for (auto &l : logLines)
        logged |= l.find("BQ25798: bat 3912mV 250mA | in 18230mV 90mA | sys 3922mV | Fast charge (CC)") != std::string::npos;
    CHECK(logged);

    SECTION("adapter: polling is throttled");
    setAdc(chip, 3700, -40, 0, 0);
    chip.regs[0x1B] = 0x00;
    chip.regs[0x1C] = 0;
    fakeMillis += 1000;
    CHECK_EQ(batteryLevel->getBattVoltage(), 3912); // cached
    fakeMillis += BQ25798_POLL_INTERVAL_MS;
    CHECK_EQ(batteryLevel->getBattVoltage(), 3700);
    CHECK(!batteryLevel->isVbusIn());
    CHECK(!batteryLevel->isCharging());

    SECTION("adapter: input present but not power-good is not external power");
    chip.regs[0x1B] = 0x01;
    fakeMillis += BQ25798_POLL_INTERVAL_MS;
    CHECK(!batteryLevel->isVbusIn());
    chip.regs[0x1B] = 0x00;

    SECTION("adapter: charge done reports 100 %");
    chip.regs[0x1C] = 7 << 5;
    fakeMillis += BQ25798_POLL_INTERVAL_MS;
    CHECK_EQ(batteryLevel->getBatteryPercent(), 100);
    chip.regs[0x1C] = 0;
    fakeMillis += BQ25798_POLL_INTERVAL_MS;

    SECTION("adapter: I2C glitches never produce a 0 mV battery");
    chip.online = false;
    for (int i = 0; i < 8; i++) {
        fakeMillis += BQ25798_POLL_INTERVAL_MS;
        uint16_t mv = batteryLevel->getBattVoltage();
        bool connected = batteryLevel->isBatteryConnect();
        CHECK(!(connected && mv == 0));
        if (i < 3) {
            CHECK(connected);
            CHECK_EQ(mv, 3700); // last good value kept through short glitches
        }
    }
    CHECK(!batteryLevel->isBatteryConnect()); // gave up after repeated failures
    chip.online = true;
    fakeMillis += BQ25798_POLL_INTERVAL_MS;
    CHECK(batteryLevel->isBatteryConnect());
    CHECK_EQ(batteryLevel->getBattVoltage(), 3700);

    SECTION("adapter: re-applies config after a chip reset");
    chip.powerOnReset();
    setAdc(chip, 3690, -40, 0, 0);
    chip.regs[0x1D] = 0x01;
    logLines.clear();
    fakeMillis += BQ25798_POLL_INTERVAL_MS;
    batteryLevel->getBattVoltage();
    CHECK_EQ(chip.regs[0x10] & 0x07, 0);
    CHECK_EQ(chip.regs[0x15] & 0x01, 1);
    bool warned = false;
    for (auto &l : logLines)
        warned |= l.find("re-applying") != std::string::npos;
    CHECK(warned);

    SECTION("adapter: ship mode only on low-battery shutdown");
    uint8_t before = chip.regs[0x11];
    fakePower.lowBattery = false;
    notifyDeepSleep.notifyObservers(nullptr);
    CHECK_EQ(chip.regs[0x11], before); // user shutdown: leave power alone
    fakePower.lowBattery = true;
    notifyDeepSleep.notifyObservers(nullptr);
    CHECK_EQ(chip.regs[0x11] & 0x07, 0x04);
}

int main()
{
    printf("BQ25798 driver tests\n");
    testDriver();
    testMeshtasticAdapter();
    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures)
        printf("BQ25798 tests FAILED\n");
    else
        printf("BQ25798 tests passed\n");
    return failures ? 1 : 0;
}
