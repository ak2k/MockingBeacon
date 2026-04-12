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

StatusOutput compute_status(const StatusInput& in) {
    StatusOutput out{};

    auto status_airtag = static_cast<uint8_t>(in.status_flags & 0xFF);
    auto status_fmdn = static_cast<uint8_t>((in.status_flags & 0xFF00) >> 8);
    int b_airtag = (in.status_flags & 0xF0000) >> 16;
    int b_fmdn = (in.status_flags & 0xF00000) >> 20;

    // --- AirTag status ---
    switch (b_airtag) {
    case 1:
        // Broadcast first byte from statusFlags as is
        break;
    case 2:
        // Broadcast incrementing byte as status byte
        status_airtag = static_cast<uint8_t>(in.keys_changes & 0xFF);
        break;
    case 3:
    case 5:
        // Broadcast battery voltage in status byte
        status_airtag = static_cast<uint8_t>(in.battery_voltage / 100);
        if ((b_airtag == 3) || (in.what_in_status == 0)) {
            break;
        }
        if (in.what_in_status == 1) {
            status_airtag = static_cast<uint8_t>(0x80 | in.accel_byte);
        }
        if (in.what_in_status == 2) {
            // Send temperature in -10..53C range as positive 6-bit integer
            int tmp_temp = (in.temperature + 5) / 10 + 10;
            if (tmp_temp < 0) {
                tmp_temp = 0;
            }
            if (tmp_temp > 63) {
                tmp_temp = 63;
            }
            status_airtag = static_cast<uint8_t>(0x40 | tmp_temp);
        }
        break;
    case 4: {
        // Broadcast battery status
        int battery = 3;
        if (in.battery_voltage > kBatteryLevelFull) {
            battery = 0;
        } else {
            if (in.battery_voltage > kBatteryLevelNormal) {
                battery = 1;
            } else {
                if (in.battery_voltage > kBatteryLevelLow) {
                    battery = 2;
                } else {
                    battery = 3;
                }
            }
        }
        status_airtag = static_cast<uint8_t>(status_airtag | (battery << 6));
        break;
    }
    default:
        break;
    }
    out.airtag_status = status_airtag;

    // --- FMDN status ---
    switch (b_fmdn) {
    case 1:
        // Broadcast first byte from statusFlags as is
        break;
    case 2:
        // Broadcast incrementing byte as status byte
        status_fmdn = static_cast<uint8_t>(in.keys_changes & 0xFF);
        break;
    case 3:
    case 5:
        // Broadcast battery voltage in status byte
        status_fmdn = static_cast<uint8_t>(in.battery_voltage / 100);
        if ((b_fmdn == 3) || (in.what_in_status == 0)) {
            break;
        }
        if (in.what_in_status == 1) {
            status_fmdn = static_cast<uint8_t>(0x80 | in.accel_byte);
        }
        if (in.what_in_status == 2) {
            // Send temperature in -10..53C range as positive 6-bit integer
            int tmp_temp = (in.temperature + 5) / 10 + 10;
            if (tmp_temp < 0) {
                tmp_temp = 0;
            }
            if (tmp_temp > 63) {
                tmp_temp = 63;
            }
            status_fmdn = static_cast<uint8_t>(0x40 | tmp_temp);
        }
        break;
    case 4: {
        // Broadcast battery status
        int battery = 0;
        if (in.battery_voltage > kBatteryLevelNormal) {
            battery = 1;
        } else {
            if (in.battery_voltage > kBatteryLevelLow) {
                battery = 2;
            } else {
                if (in.battery_voltage > 0) {
                    battery = 3;
                } else {
                    battery = 0;
                }
            }
        }
        status_fmdn = static_cast<uint8_t>(status_fmdn | (battery << 5));
        break;
    }
    default:
        break;
    }
    out.fmdn_status = status_fmdn;

    return out;
}

} // namespace beacon
