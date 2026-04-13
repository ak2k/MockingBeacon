// beacon_config.hpp — Settings, NVS persistence, GATT validation
#ifndef BEACON_CONFIG_HPP
#define BEACON_CONFIG_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace beacon {

// ---- NVS field IDs (match settings.c) ----
enum NvsFieldId : uint16_t {
    ID_fmdn_NVS = 0x01,
    ID_airtag_NVS = 0x02,
    ID_period_NVS = 0x03,
    ID_changeInterval_NVS = 0x04,
    ID_key_NVS = 0x05,
    ID_auth_NVS = 0x06,
    ID_power_NVS = 0x07,
    ID_fmdnKey_NVS = 0x08,
    ID_timeOffset_NVS = 0x09,
    ID_settingsMAC_NVS = 0x0a,
    ID_status_NVS = 0x0b,
    ID_accel_NVS = 0x0c,
    ID_turnedOn_NVS = 0x0d,
};

// ---- Constants ----
inline constexpr int kMaxKeysInMemory = 40;
inline constexpr int kKeySize = 28;

// ---- BeaconConfig (consolidates all globals from settings.c) ----
//
// All fields are persisted in NVS as 4-byte ints (wire format).
// On load, values are validated against allowed ranges; out-of-range
// values fall back to the defaults shown here.
struct BeaconConfig {
    bool flag_fmdn = false;           // Enable Google Find My Device Network broadcasting
    bool flag_airtag = false;         // Enable Apple AirTag (Offline Finding) broadcasting
    int mult_period = 2;              // Advertising interval multiplier {1,2,4,8}
    int tx_power = 2;                 // TX power index: 0 = -8 dBm, 1 = 0 dBm, 2 = +4 dBm
    int change_interval = 6000;       // Key rotation interval in seconds [30..7200], aligned to 8
    int status_flags = 0x458000;      // Packed status byte config (see status_flags encoding below)
    int accel_threshold = 800;        // Accelerometer movement threshold in mg [0..16383]
    bool turned_on = false;           // Master broadcast enable (persisted across reboots)
    int64_t time_offset = 0;          // Unix time offset: real_time = time_offset + uptime_seconds
    std::array<uint8_t, 8> auth_code = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'}; // GATT auth code
    // MAC address for settings mode. If first and last bytes are zero,
    // use the chip's factory MAC from UICR instead.
    std::array<uint8_t, 6> settings_mac = {0, 0x41, 0x42, 0x43, 0x44, 0};
    std::array<uint8_t, 20> fmdn_key = {};   // Google FMDN encryption key (single key, no rotation)
    std::array<std::array<uint8_t, 28>, 40> keys = {};  // AirTag public key ring
    int num_keys = 0;                 // Number of valid keys in keys[] [0..40]
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
    BeaconConfig& config_mut();

    int64_t get_time(uint32_t uptime_sec) const;
    void update_time_offset(int64_t new_time, uint32_t uptime_sec);
    void save_time(uint32_t uptime_sec);

  private:
    INvsStorage& nvs_;
    BeaconConfig config_;

    // Helper: read an int from NVS into dest; on failure, leave dest unchanged
    void read_int(uint16_t id, int& dest);
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
