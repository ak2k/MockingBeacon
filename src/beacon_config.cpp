// beacon_config.cpp — Settings, NVS persistence, GATT validation
#include "beacon_config.hpp"

namespace beacon {

// ---- StatusFlags ----

uint32_t StatusFlags::pack() const {
    return static_cast<uint32_t>(airtag_base) | (static_cast<uint32_t>(fmdn_base) << 8) |
           (static_cast<uint32_t>(airtag_mode) << 16) | (static_cast<uint32_t>(fmdn_mode) << 20);
}

StatusFlags StatusFlags::unpack(uint32_t raw) {
    StatusFlags f;
    f.airtag_base = static_cast<uint8_t>(raw & 0xFF);
    f.fmdn_base = static_cast<uint8_t>((raw >> 8) & 0xFF);
    f.airtag_mode = static_cast<StatusMode>((raw >> 16) & 0xF);
    f.fmdn_mode = static_cast<StatusMode>((raw >> 20) & 0xF);
    return f;
}

// ---- SettingsManager ----

SettingsManager::SettingsManager(INvsStorage& nvs) : nvs_(nvs) {}

void SettingsManager::read_bool(uint16_t id, bool& dest) {
    int tmp = 0;
    if (nvs_.read(id, &tmp, sizeof(tmp)) > 0) {
        dest = (tmp != 0);
    }
}

int SettingsManager::load() {
    // Reset to defaults first
    config_ = BeaconConfig{};

    // Read boolean settings from NVS (stored as 4-byte int)
    read_bool(ID_fmdn_NVS, config_.flag_fmdn);
    read_bool(ID_airtag_NVS, config_.flag_airtag);
    read_bool(ID_turnedOn_NVS, config_.turned_on);

    // Read narrowed scalar fields with validation.
    // NVS wire format is always 4-byte int; invalid values are rejected (keep default).
    {
        int tmp = 0;
        if (nvs_.read(ID_period_NVS, &tmp, sizeof(tmp)) > 0) {
            if (tmp == 1 || tmp == 2 || tmp == 4 || tmp == 8) {
                config_.mult_period = static_cast<uint8_t>(tmp);
            }
            // else: keep default (mult_period=0 causes busy-loop, must reject)
        }
    }
    {
        int tmp = 0;
        if (nvs_.read(ID_power_NVS, &tmp, sizeof(tmp)) > 0) {
            if (tmp >= 0 && tmp <= 2) {
                config_.tx_power = static_cast<uint8_t>(tmp);
            }
        }
    }
    {
        int tmp = 0;
        if (nvs_.read(ID_changeInterval_NVS, &tmp, sizeof(tmp)) > 0) {
            if (tmp >= 30 && tmp <= 7200) {
                config_.change_interval =
                    static_cast<uint16_t>(tmp - (tmp % 8)); // Align to multiple of 8
            }
        }
    }
    {
        int tmp = 0;
        if (nvs_.read(ID_status_NVS, &tmp, sizeof(tmp)) > 0) {
            config_.status_flags = static_cast<uint32_t>(tmp);
        }
    }
    {
        int tmp = 0;
        if (nvs_.read(ID_accel_NVS, &tmp, sizeof(tmp)) > 0) {
            if (tmp >= 0 && tmp <= 16383) {
                config_.accel_threshold = static_cast<uint16_t>(tmp);
            }
        }
    }

    // Time offset is int64_t
    {
        int64_t tmp = 0;
        if (nvs_.read(ID_timeOffset_NVS, &tmp, sizeof(tmp)) > 0) {
            config_.time_offset = tmp;
        }
    }

    // Settings MAC
    nvs_.read(ID_settingsMAC_NVS, config_.settings_mac.data(), config_.settings_mac.size());

    // Auth code
    nvs_.read(ID_auth_NVS, config_.auth_code.data(), config_.auth_code.size());

    // Load airtag keys from NVS
    if (nvs_.read(ID_key_NVS, config_.keys.data(), sizeof(config_.keys)) > 0) {
        // Count non-zero keys (BUG FIX: original compared against sizeof
        // which is 1120 bytes, not 40 elements)
        int count = 0;
        for (int i = 0; i < kMaxKeysInMemory; i++) {
            bool all_zero = true;
            for (int j = 0; j < kKeySize; j++) {
                if (config_.keys[static_cast<size_t>(i)][static_cast<size_t>(j)] != 0) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero) {
                break;
            }
            count++;
        }
        config_.num_keys = static_cast<uint8_t>(count);
    } else {
        config_.num_keys = 0;
    }

    // Load FMDN key (fall back to empty default)
    if (nvs_.read(ID_fmdnKey_NVS, config_.fmdn_key.data(), config_.fmdn_key.size()) <= 0) {
        config_.fmdn_key = {};
    }

    return 0;
}

// NVS wire format: all scalar fields stored as 4-byte int for backward compatibility.
// Narrowed C++ types are widened to int before writing.
int SettingsManager::save_field(uint16_t field_id) {
    switch (field_id) {
    case ID_fmdn_NVS: {
        int v = config_.flag_fmdn ? 1 : 0;
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_airtag_NVS: {
        int v = config_.flag_airtag ? 1 : 0;
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_period_NVS: {
        int v = static_cast<int>(config_.mult_period);
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_changeInterval_NVS: {
        int v = static_cast<int>(config_.change_interval);
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_key_NVS:
        return nvs_.write(field_id, config_.keys.data(), sizeof(config_.keys));
    case ID_auth_NVS:
        return nvs_.write(field_id, config_.auth_code.data(), config_.auth_code.size());
    case ID_power_NVS: {
        int v = static_cast<int>(config_.tx_power);
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_fmdnKey_NVS:
        return nvs_.write(field_id, config_.fmdn_key.data(), config_.fmdn_key.size());
    case ID_timeOffset_NVS:
        return nvs_.write(field_id, &config_.time_offset, sizeof(config_.time_offset));
    case ID_settingsMAC_NVS:
        return nvs_.write(field_id, config_.settings_mac.data(), config_.settings_mac.size());
    case ID_status_NVS: {
        int v = static_cast<int>(config_.status_flags);
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_accel_NVS: {
        int v = static_cast<int>(config_.accel_threshold);
        return nvs_.write(field_id, &v, sizeof(v));
    }
    case ID_turnedOn_NVS: {
        int v = config_.turned_on ? 1 : 0;
        return nvs_.write(field_id, &v, sizeof(v));
    }
    default:
        return -1;
    }
}

const BeaconConfig& SettingsManager::config() const {
    return config_;
}

// ---- Validated setters ----

bool SettingsManager::set_mult_period(uint8_t v) {
    if (v != 1 && v != 2 && v != 4 && v != 8)
        return false;
    config_.mult_period = v;
    return true;
}

bool SettingsManager::set_tx_power(uint8_t v) {
    if (v > 2)
        return false;
    config_.tx_power = v;
    return true;
}

bool SettingsManager::set_change_interval(uint16_t v) {
    if (v < 30 || v > 7200)
        return false;
    config_.change_interval = v - (v % 8); // Align to multiple of 8
    return true;
}

bool SettingsManager::set_status_flags(uint32_t v) {
    config_.status_flags = v;
    return true;
}

bool SettingsManager::set_accel_threshold(uint16_t v) {
    if (v > 16383)
        return false;
    config_.accel_threshold = v;
    return true;
}

void SettingsManager::set_flag_fmdn(bool v) {
    config_.flag_fmdn = v;
}

void SettingsManager::set_flag_airtag(bool v) {
    config_.flag_airtag = v;
}

void SettingsManager::set_turned_on(bool v) {
    config_.turned_on = v;
}

void SettingsManager::set_fmdn_key(const uint8_t* data, size_t len) {
    if (len == config_.fmdn_key.size()) {
        std::memcpy(config_.fmdn_key.data(), data, len);
    }
}

void SettingsManager::set_settings_mac(const uint8_t* data, size_t len) {
    if (len == config_.settings_mac.size()) {
        std::memcpy(config_.settings_mac.data(), data, len);
    }
}

void SettingsManager::set_auth_code(const uint8_t* data, size_t len) {
    if (len == config_.auth_code.size()) {
        std::memcpy(config_.auth_code.data(), data, len);
    }
}

int64_t SettingsManager::get_time(uint32_t uptime_sec) const {
    return config_.time_offset + static_cast<int64_t>(uptime_sec);
}

void SettingsManager::update_time_offset(int64_t new_time, uint32_t uptime_sec) {
    config_.time_offset = new_time - static_cast<int64_t>(uptime_sec);
}

void SettingsManager::save_time(uint32_t uptime_sec) {
    int64_t current = get_time(uptime_sec);
    nvs_.write(ID_timeOffset_NVS, &current, sizeof(current));
}

// ---- GATT validators ----

GattResult validate_field(const uint8_t* buf, size_t len, bool authorized,
                          const GattFieldSpec& spec, int32_t& out) {
    if (len != spec.expected_len) {
        return GattResult::InvalidLength;
    }
    if (!authorized) {
        return GattResult::Unauthorized;
    }

    int32_t value = 0;
    std::memcpy(&value, buf, sizeof(value));

    if (spec.allowed_values != nullptr) {
        bool found = false;
        for (size_t i = 0; i < spec.allowed_count; i++) {
            if (value == spec.allowed_values[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return GattResult::OutOfRange;
        }
    } else {
        if (value < spec.min_val || value > spec.max_val) {
            return GattResult::OutOfRange;
        }
    }

    out = value;
    return GattResult::Ok;
}

GattResult validate_auth_code(const uint8_t* buf, size_t len, const uint8_t* stored,
                              size_t code_len) {
    if (len != code_len) {
        return GattResult::InvalidLength;
    }

    // Constant-time comparison
    uint8_t diff = 0;
    for (size_t i = 0; i < code_len; i++) {
        diff |= static_cast<uint8_t>(buf[i] ^ stored[i]);
    }

    return (diff == 0) ? GattResult::Ok : GattResult::Unauthorized;
}

GattResult validate_key_chunk(const uint8_t* /*buf*/, size_t len, bool authorized,
                              int keys_received, int max_keys) {
    if (len != 14) {
        return GattResult::InvalidLength;
    }
    if (!authorized) {
        return GattResult::Unauthorized;
    }
    if (keys_received >= 2 * max_keys) {
        return GattResult::OutOfRange;
    }
    return GattResult::Ok;
}

} // namespace beacon
