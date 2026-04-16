// beacon_config.hpp — Settings, NVS persistence, GATT validation
#ifndef BEACON_CONFIG_HPP
#define BEACON_CONFIG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "beacon_nvs_ids.hpp"

namespace beacon {

// ---- Constants ----
inline constexpr int kMaxKeysInMemory = 40;
inline constexpr int kKeySize = 28;

// ---- Status types ----

/// Status mode for AirTag/FMDN status byte generation.
enum class StatusMode : uint8_t {
    Off = 0,          // Don't touch status byte
    Fixed = 1,        // Broadcast base byte as-is
    Incrementing = 2, // Broadcast keys_changes counter
    Voltage = 3,      // Broadcast battery voltage (mV / 100)
    BatteryLevel = 4, // Broadcast battery level (0..3) OR'd into base byte
    Telemetry = 5,    // Cycle between voltage, accel, temperature each minute
};

/// Decoded view of the packed status_flags field.
struct StatusFlags {
    uint8_t airtag_base = 0x00;
    uint8_t fmdn_base = 0x80;
    StatusMode airtag_mode = StatusMode::Telemetry;
    StatusMode fmdn_mode = StatusMode::BatteryLevel;

    uint32_t pack() const;
    static StatusFlags unpack(uint32_t raw);
};

// ---- BeaconConfig (consolidates all globals from settings.c) ----
//
// All fields are persisted in NVS as 4-byte ints (wire format).
// On load, values are validated against allowed ranges; out-of-range
// values fall back to the defaults shown here.
struct BeaconConfig {
    bool flag_fmdn = false;           // Enable Google Find My Device Network broadcasting
    bool flag_airtag = false;         // Enable Apple AirTag (Offline Finding) broadcasting
    uint8_t mult_period = 2;          // Advertising interval multiplier {1,2,4,8}
    uint8_t tx_power = 2;             // TX power index: 0 = -8 dBm, 1 = 0 dBm, 2 = +4 dBm
    uint16_t change_interval = 6000;  // Key rotation interval in seconds [32..7200], multiple of 8
    uint32_t status_flags = 0x458000; // Packed status byte config (see status_flags encoding below)
    uint16_t accel_threshold = 800;   // Accelerometer movement threshold in mg [0..16383]
    bool turned_on = false;           // Master broadcast enable (persisted across reboots)
    int64_t time_offset = 0;          // Unix time offset: real_time = time_offset + uptime_seconds
    std::array<uint8_t, 8> auth_code = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'}; // GATT auth code
    // MAC address for settings mode. If first and last bytes are zero,
    // use the chip's factory MAC from UICR instead.
    std::array<uint8_t, 6> settings_mac = {0, 0x41, 0x42, 0x43, 0x44, 0};
    std::array<uint8_t, 20> fmdn_key = {}; // Google FMDN encryption key (single key, no rotation)
    std::array<std::array<uint8_t, 28>, 40> keys = {}; // AirTag public key ring
    uint8_t num_keys = 0;                              // Number of valid keys in keys[] [0..40]
};

// ---- status_flags encoding (from main.c) ----
//
// Packed 32-bit field controlling what goes in the AirTag/FMDN status bytes:
//   bits  0..7  — base status byte for AirTag (broadcast as-is in mode 1)
//   bits  8..15 — base status byte for FMDN  (broadcast as-is in mode 1)
//   bits 16..19 — AirTag status mode:
//     0 = off (don't touch status byte)
//     1 = broadcast base byte as-is
//     2 = broadcast incrementing byte (keys_changes counter)
//     3 = broadcast battery voltage (mV / 100)
//     4 = broadcast battery level (0..3) OR'd into base byte bits 6..7
//     5 = telemetry: cycle each minute between voltage, accel byte, temperature
//   bits 20..23 — FMDN status mode (same encoding as AirTag)
//
// Default 0x458000 = AirTag mode 5 (telemetry), FMDN mode 4 (battery level),
//                    AirTag base 0x00, FMDN base 0x80

// ---- INvsStorage interface (for testing) ----
class INvsStorage {
  public:
    virtual int read(uint16_t id, void* data, size_t len) = 0;
    virtual int write(uint16_t id, const void* data, size_t len) = 0;

  protected:
    ~INvsStorage() = default;
};

// ---- SettingsManager ----
class SettingsManager {
  public:
    explicit SettingsManager(INvsStorage& nvs);

    int load();
    int save_field(uint16_t field_id);

    const BeaconConfig& config() const;

    // Validated setters — return false if value rejected, leaving config unchanged.
    bool set_mult_period(uint8_t v);      // Allowed: {1, 2, 4, 8}
    bool set_tx_power(uint8_t v);         // Allowed: 0..2
    bool set_change_interval(uint16_t v); // Range: 32..7200, auto-aligned to multiple of 8
    void set_status_flags(uint32_t v);    // Any value accepted (app-level encoding)
    bool set_accel_threshold(uint16_t v); // Range: 0..16383
    void set_flag_fmdn(bool v);
    void set_flag_airtag(bool v);
    void set_turned_on(bool v);
    void set_fmdn_key(const uint8_t* data, size_t len);     // len must be 20
    void set_settings_mac(const uint8_t* data, size_t len); // len must be 6
    void set_auth_code(const uint8_t* data, size_t len);    // len must be 8
    // Write a 14-byte half of a key. key_idx in [0..39], offset 0 or 14.
    void set_key_chunk(int key_idx, size_t offset, const uint8_t* data, size_t len);
    void set_num_keys(uint8_t n); // [0..40]

    int64_t get_time(uint32_t uptime_sec) const;
    void update_time_offset(int64_t new_time, uint32_t uptime_sec);
    void save_time(uint32_t uptime_sec);

  private:
    INvsStorage& nvs_;
    BeaconConfig config_;

    // Helper: read a bool from NVS (stored as 4-byte int); on failure, leave dest unchanged
    void read_bool(uint16_t id, bool& dest);
};

// ---- GATT validation (pure functions, no Zephyr dependency) ----
struct GattFieldSpec {
    size_t expected_len;
    int32_t min_val;
    int32_t max_val;
    const int32_t* allowed_values; // null = any in range
    size_t allowed_count;
};

enum class GattResult : uint8_t {
    Ok,
    InvalidLength,
    Unauthorized,
    OutOfRange,
};

GattResult validate_field(const uint8_t* buf, size_t len, bool authorized,
                          const GattFieldSpec& spec, int32_t& out);

GattResult validate_auth_code(const uint8_t* buf, size_t len, const uint8_t* stored,
                              size_t code_len);

GattResult validate_key_chunk(const uint8_t* buf, size_t len, bool authorized, int keys_received,
                              int max_keys);

} // namespace beacon

#endif // BEACON_CONFIG_HPP
