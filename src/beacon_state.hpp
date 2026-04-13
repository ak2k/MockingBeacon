// beacon_state.hpp — Explicit state machine for beacon operation
#ifndef BEACON_STATE_HPP
#define BEACON_STATE_HPP

#include <cstdint>

#include "accel_data.hpp"
#include "beacon_config.hpp"
#include "ihardware.hpp"

namespace beacon {

// ---- State enum ----
enum class State : uint8_t {
    Initializing,
    Broadcasting,
    SettingsMode,
    FirmwareUpload,
    ShuttingDown,
};

// ---- Timer state (replaces global tLast* variables from main.c) ----
// All values are uptime in seconds (from k_uptime_seconds()).
struct Timers {
    uint32_t time_save = 0;      // Last NVS time persistence (every kIntervalTimeSave)
    uint32_t battery_check = 0;  // Last battery + status update (every kIntervalBatteryCheck)
    uint32_t settings_mode = 0;  // Last settings mode entry (every kIntervalSettings)
    uint32_t max_power = 0;      // Last max-power burst start (every kIntervalMaxPower)
    uint32_t end_max_power = 0;  // Deadline to end max-power burst (0 = no burst active)
    uint32_t airtag_switch = 0;  // Last AirTag key rotation (every change_interval)
    uint32_t end_settings = 0;   // Deadline to exit settings mode
    uint32_t accel_read = 0;     // Last accelerometer FIFO read (every kIntervalAccelerometer)
    uint32_t accel_reset = 0;    // Last accelerometer re-init (every 30 read cycles)
};

// ---- Constants (from myboards.h / lis2dw12.h) ----
// BLE advertising intervals are in BLE units (1 unit = 0.625 ms).
inline constexpr int kBroadcastIntervalMin = 1500; // 1500 * 0.625 = 937.5 ms
inline constexpr int kBroadcastIntervalMax = 1590; // 1590 * 0.625 = 993.75 ms
// The remaining constants are in seconds unless noted.
inline constexpr int kIntervalSettings = 60;       // Enter settings mode every 60s
inline constexpr int kSettingsWait = 2;             // Stay in settings mode for 2s (+ GATT delays)
inline constexpr int kIntervalTimeSave = 3600;      // Persist time to NVS every hour
inline constexpr int kIntervalBatteryCheck = 60;    // Re-read battery + update status every 60s
inline constexpr int kIntervalMaxPower = 68;        // Max-power burst every 68s (when tx_power < 2)
inline constexpr int kIntervalAccelerometer = 20;   // Read accelerometer FIFO every 20s
inline constexpr int kBatteryLowVoltage = 2800;     // UVLO threshold in mV (Li-Ion cutoff)
inline constexpr int kSettingsAdvInterval = 400;     // Settings mode adv interval (BLE units)
inline constexpr int kShutdownNokeys = 300;          // Shutdown after 5 min if no keys and not charging
inline constexpr int kAccelResetMultiplier = 30;     // Re-init accelerometer every 30 read cycles
inline constexpr int kChargeLockDuration = 3600;     // Skip UVLO for 1 hour if charging detected
inline constexpr int kUvloBadPowerThreshold = 5;     // Shutdown after 5 consecutive low-voltage checks

// ---- StateMachine ----
class StateMachine {
  public:
    StateMachine(IHardware& hw, SettingsManager& settings, MovementTracker& accel);

    /// Run the startup sequence (main() lines 677-776).
    void initialize();

    /// Run one iteration of the main loop (main() lines 778-1018).
    void tick();

    /// Current state.
    State state() const { return state_; }

    // Expose internals for testing
    const Timers& timers() const { return timers_; }
    int current_key() const { return current_key_; }
    int bad_power() const { return bad_power_; }
    int charge_lock_counter() const { return charge_lock_counter_; }
    bool broadcasting_airtag() const { return broadcasting_airtag_; }
    bool broadcasting_fmdn() const { return broadcasting_fmdn_; }
    bool broadcasting_settings() const { return broadcasting_settings_; }
    bool broadcasting_anything() const { return broadcasting_anything_; }
    int keys_changes() const { return keys_changes_; }
    int what_in_status() const { return what_in_status_; }
    int last_battery_voltage() const { return last_battery_voltage_; }

    // Allow tests to inject GATT connection/auth state
    void set_connected_gatt(bool v) { connected_gatt_ = v; }
    void set_authorized_gatt(bool v) { authorized_gatt_ = v; }
    void set_pause_upload(bool v) { pause_upload_ = v; }
    void set_needs_reset(bool v) { needs_reset_ = v; }

  private:
    IHardware& hw_;
    SettingsManager& settings_;
    MovementTracker& accel_;

    State state_ = State::Initializing;
    Timers timers_;

    // Broadcast state
    bool broadcasting_anything_ = false;
    bool broadcasting_airtag_ = false;
    bool broadcasting_fmdn_ = false;
    bool broadcasting_settings_ = false;

    // Key rotation
    int current_key_ = 0;       // Index into config.keys[] for current AirTag key
    int keys_changes_ = 0;      // Rolling counter of key switches (used by status mode 2)
    int what_in_status_ = 2;    // Telemetry cycle: 0=voltage, 1=accel, 2=temperature

    // Battery / UVLO (under-voltage lockout)
    int last_battery_voltage_ = 0;   // Most recent battery reading in mV
    int bad_power_ = 0;              // Consecutive low-voltage checks (shutdown at >5)
    // If charging is detected, skip UVLO shutdown for 1 hour.
    // Decremented by mult_period each tick.
    int charge_lock_counter_ = 0;

    // GATT state (set externally via callbacks in real firmware)
    bool connected_gatt_ = false;
    bool authorized_gatt_ = false;
    bool pause_upload_ = false;
    bool needs_reset_ = false;

    // MAC address derived from current key
    uint8_t ble_addr_[6] = {};

    // Helpers
    void update_battery();
    void update_status();
    void broadcast();
    void handle_settings_mode_exit();
    void handle_battery_check();
    void handle_key_rotation();
    void handle_settings_mode_entry();
    void handle_initial_broadcast();
    void handle_airtag_fmdn_alternation();
    void handle_accelerometer();
    void handle_max_power_burst();
    bool handle_button_longpress();
    bool handle_uvlo_shutdown();
    bool handle_nokeys_shutdown();
    void set_mac_from_current_key();
};

} // namespace beacon

#endif // BEACON_STATE_HPP
