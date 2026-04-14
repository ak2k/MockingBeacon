// beacon_logic.cpp — Pure computation functions for BLE advertisement
#include "beacon_logic.hpp"

#include <cstring>

namespace beacon {

void derive_mac_from_key(const uint8_t* key, uint8_t* mac_out) {
    mac_out[5] = key[0] | 0xC0;
    mac_out[4] = key[1];
    mac_out[3] = key[2];
    mac_out[2] = key[3];
    mac_out[1] = key[4];
    mac_out[0] = key[5];
}

void fill_adv_template(const uint8_t* key, uint8_t* tmpl, size_t tmpl_size) {
    if (tmpl_size < 30) {
        return;
    }
    std::memcpy(&tmpl[7], &key[6], 22);
    tmpl[29] = static_cast<uint8_t>(key[0] >> 6);
}

bool is_key_empty(const uint8_t* key, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (key[i] != 0) {
            return false;
        }
    }
    return true;
}

// Compute status byte for one protocol (AirTag or FMDN).
// base_status is the default byte from StatusFlags, mode selects the computation.
// battery_shift is 6 for AirTag (bits 6..7), 5 for FMDN (bits 5..6).
// voltage_only_for_mode3: true for AirTag (mode 3 = voltage only),
//   false for FMDN (mode 3 cycles through telemetry, matching original C behavior).
static uint8_t compute_one_status(uint8_t base_status, StatusMode mode, const StatusInput& in,
                                  int battery_shift, int battery_default,
                                  bool voltage_only_for_mode3) {
    switch (mode) {
    case StatusMode::Fixed:
        return base_status;
    case StatusMode::Incrementing:
        return static_cast<uint8_t>(in.keys_changes & 0xFF);
    case StatusMode::Voltage:
    case StatusMode::Telemetry:
        // Start with battery voltage
        base_status = static_cast<uint8_t>(in.battery_voltage / 100);
        if ((mode == StatusMode::Voltage && voltage_only_for_mode3) || in.what_in_status == 0) {
            return base_status;
        }
        if (in.what_in_status == 1) {
            return static_cast<uint8_t>(0x80 | in.accel_byte);
        }
        if (in.what_in_status == 2) {
            // Temperature in -10..53C range as positive 6-bit integer
            int tmp_temp = (in.temperature + 5) / 10 + 10;
            if (tmp_temp < 0) {
                tmp_temp = 0;
            }
            if (tmp_temp > 63) {
                tmp_temp = 63;
            }
            return static_cast<uint8_t>(0x40 | tmp_temp);
        }
        return base_status;
    case StatusMode::BatteryLevel: {
        int battery = battery_default;
        if (battery_shift == 6) {
            // AirTag: 4-level (0=full, 1=normal, 2=low, 3=critical)
            if (in.battery_voltage > kBatteryLevelFull) {
                battery = 0;
            } else if (in.battery_voltage > kBatteryLevelNormal) {
                battery = 1;
            } else if (in.battery_voltage > kBatteryLevelLow) {
                battery = 2;
            } else {
                battery = 3;
            }
        } else {
            // FMDN: 3-level (0=unknown, 1=normal, 2=low, 3=critical)
            if (in.battery_voltage > kBatteryLevelNormal) {
                battery = 1;
            } else if (in.battery_voltage > kBatteryLevelLow) {
                battery = 2;
            } else if (in.battery_voltage > 0) {
                battery = 3;
            } else {
                battery = 0;
            }
        }
        return static_cast<uint8_t>(base_status | (battery << battery_shift));
    }
    case StatusMode::Off:
    default:
        return base_status;
    }
}

StatusOutput compute_status(const StatusInput& in) {
    StatusOutput out{};
    // AirTag: mode 3 (Voltage) returns voltage only.
    // FMDN: mode 3 cycles through telemetry (matches original C behavior where
    // the FMDN case 3/5 block had no early break for mode 3).
    out.airtag_status =
        compute_one_status(in.status.airtag_base, in.status.airtag_mode, in, 6, 3, true);
    out.fmdn_status = compute_one_status(in.status.fmdn_base, in.status.fmdn_mode, in, 5, 0, false);
    return out;
}

} // namespace beacon
