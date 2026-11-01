#pragma once
// Host stand-in for Meshtastic's nanopb-generated telemetry.pb.h: only the PowerMetrics
// fields the BQ25798 overlay touches (same names as the real generated code).
#include <stdint.h>

typedef struct _meshtastic_PowerMetrics {
    bool has_ch1_voltage;
    float ch1_voltage;
    bool has_ch1_current;
    float ch1_current;
    bool has_ch2_voltage;
    float ch2_voltage;
    bool has_ch2_current;
    float ch2_current;
    bool has_ch3_voltage;
    float ch3_voltage;
    bool has_ch3_current;
    float ch3_current;
    bool has_ch4_voltage;
    float ch4_voltage;
    bool has_ch4_current;
    float ch4_current;
    bool has_ch5_voltage;
    float ch5_voltage;
    bool has_ch5_current;
    float ch5_current;
    bool has_ch6_voltage;
    float ch6_voltage;
    bool has_ch6_current;
    float ch6_current;
    bool has_ch7_voltage;
    float ch7_voltage;
    bool has_ch7_current;
    float ch7_current;
    bool has_ch8_voltage;
    float ch8_voltage;
    bool has_ch8_current;
    float ch8_current;
} meshtastic_PowerMetrics;

typedef struct _meshtastic_Telemetry {
    uint32_t time;
    union {
        meshtastic_PowerMetrics power_metrics;
    } variant;
} meshtastic_Telemetry;
