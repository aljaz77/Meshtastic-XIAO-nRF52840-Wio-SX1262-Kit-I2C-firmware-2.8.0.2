#pragma once
/**
 * Power-telemetry hooks for the BQ25798 solar charger (see BQ25798BatteryLevel.h).
 *
 * Included into src/modules/Telemetry/PowerTelemetry.cpp by scripts/apply_overlay.py.
 * The functions are defined in BQ25798BatteryLevel.h (compiled as part of Power.cpp) and
 * return false when no charger was found, so the power telemetry module behaves exactly
 * like stock firmware without the add-on board.
 */

#include "mesh/generated/meshtastic/telemetry.pb.h"

/// True if a BQ25798 was detected at boot (keeps the power telemetry module enabled even
/// when no INA power sensor is present).
bool bq25798HasTelemetry();

/// Add the charger's readings to m->variant.power_metrics, using the first free channels so
/// that values from INA sensors are never overwritten. Without other power sensors:
///   ch1 = solar panel input (V, mA drawn from the panel)
///   ch2 = battery (V, mA; positive = charging, negative = discharging)
///   ch3 = system rail feeding the XIAO (V)
/// Returns true if at least one channel was filled.
bool bq25798GetPowerMetrics(meshtastic_Telemetry *m);
