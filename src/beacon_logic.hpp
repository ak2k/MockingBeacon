// beacon_logic.hpp — Pure computation functions for BLE advertisement
#ifndef BEACON_LOGIC_HPP
#define BEACON_LOGIC_HPP

#include <cstddef>
#include <cstdint>

#include "beacon_config.hpp" // StatusFlags, StatusMode

namespace beacon {

/// Derive a BLE MAC address from a 28-byte public key.
/// Copies first 6 bytes of key into mac_out in reverse order,
/// OR's the last byte (mac_out[5]) with 0xC0.
void derive_mac_from_key(const uint8_t* key, uint8_t* mac_out);

/// Fill an AirTag advertisement template from a 28-byte public key.
/// Copies key[6..27] into tmpl[7..28], sets tmpl[29] = key[0] >> 6.
/// tmpl must be at least 30 bytes; tmpl_size is for safety checks.
void fill_adv_template(const uint8_t* key, uint8_t* tmpl, size_t tmpl_size);

/// Returns true if all bytes in key[0..len-1] are zero.
bool is_key_empty(const uint8_t* key, size_t len);

/// Input to the status computation (replaces all globals).
struct StatusInput {
    StatusFlags status;       // Decoded status configuration
    uint16_t battery_voltage; // Battery voltage in millivolts (e.g. 3800 = 3.8V)
    uint16_t keys_changes;    // Rolling counter of AirTag key rotations (used by mode 2)
    uint8_t what_in_status;   // Telemetry cycle selector: 0=voltage, 1=accel, 2=temperature
    uint8_t accel_byte;       // 7-bit movement summary from MovementTracker::compute_accel_byte()
    int16_t temperature;      // Accelerometer die temperature in 0.1C units (e.g. 235 = 23.5C)
};

/// Output of the status computation.
struct StatusOutput {
    uint8_t airtag_status;
    uint8_t fmdn_status;
};

// Battery level thresholds (defaults for Li-Ion, matching myboards.h)
constexpr int kBatteryLevelFull = 4000;
constexpr int kBatteryLevelNormal = 3700;
constexpr int kBatteryLevelLow = 3300;

/// Compute the AirTag and FMDN status bytes.
/// Pure function — all state is passed explicitly via StatusInput.
StatusOutput compute_status(const StatusInput& in);

} // namespace beacon

#endif // BEACON_LOGIC_HPP
