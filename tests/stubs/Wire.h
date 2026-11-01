#pragma once
// Host TwoWire with the same signatures as the Adafruit nRF52 core, backed by a simulated
// register file (see FakeBQ25798 in test_bq25798.cpp).
#include "Arduino.h"

class TwoWire
{
  public:
    struct Device {
        virtual ~Device() = default;
        virtual bool ack() = 0;
        virtual uint8_t readReg(uint8_t reg) = 0;
        virtual void writeReg(uint8_t reg, uint8_t val) = 0;
    };
    Device *dev = nullptr;
    uint8_t devAddr = 0;

    void beginTransmission(uint8_t addr)
    {
        txAddr = addr;
        tx.clear();
    }
    size_t write(uint8_t b)
    {
        tx.push_back(b);
        return 1;
    }
    uint8_t endTransmission(bool = true)
    {
        if (!dev || txAddr != devAddr || !dev->ack())
            return 2; // NACK on address
        if (!tx.empty()) {
            ptr = tx[0];
            for (size_t i = 1; i < tx.size(); i++)
                dev->writeReg(ptr++, tx[i]);
        }
        return 0;
    }
    uint8_t requestFrom(uint8_t addr, uint8_t len)
    {
        rx.clear();
        rxPos = 0;
        if (!dev || addr != devAddr || !dev->ack())
            return 0;
        for (uint8_t i = 0; i < len; i++)
            rx.push_back(dev->readReg(ptr++));
        return len;
    }
    int read() { return rxPos < rx.size() ? rx[rxPos++] : -1; }

  private:
    uint8_t txAddr = 0, ptr = 0;
    std::vector<uint8_t> tx, rx;
    size_t rxPos = 0;
};

extern TwoWire Wire;
