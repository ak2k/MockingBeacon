// beacon_state.cpp — State machine implementation
#include "beacon_state.hpp"

#include "beacon_logic.hpp"

namespace beacon {

StateMachine::StateMachine(IHardware& hw, SettingsManager& settings, MovementTracker& accel)
    : hw_(hw), settings_(settings), accel_(accel) {}

// ---- Helpers ----

void StateMachine::set_mac_from_current_key() {
    const auto& cfg = settings_.config();
    if (current_key_ < cfg.num_keys) {
        derive_mac_from_key(cfg.keys[static_cast<size_t>(current_key_)].data(), ble_addr_);
    }
}

void StateMachine::update_battery() {
    hw_.bq_reinit(false);
    int v = hw_.battery_voltage();
    last_battery_voltage_ = (v > 0) ? static_cast<uint16_t>(v) : 0;
}

void StateMachine::update_status() {
    const auto& cfg = settings_.config();
    // Cycle through telemetry modes
    switch (what_in_status_) {
    case WhatInStatus::Voltage:
        what_in_status_ = WhatInStatus::Accel;
        break;
    case WhatInStatus::Accel:
        what_in_status_ = WhatInStatus::Temperature;
        break;
    case WhatInStatus::Temperature:
    default:
        what_in_status_ = WhatInStatus::Voltage;
        break;
    }
    StatusInput in{};
    in.status = StatusFlags::unpack(cfg.status_flags);
    in.battery_voltage = last_battery_voltage_;
    in.keys_changes = keys_changes_;
    in.what_in_status = static_cast<uint8_t>(what_in_status_);
    in.accel_byte = accel_.compute_accel_byte();
    in.temperature = accel_.temperature();
    StatusOutput status = compute_status(in);
    hw_.set_status_bytes(status.airtag_status, status.fmdn_status);
}

void StateMachine::broadcast() {
    const auto& cfg = settings_.config();
    if (broadcasting_anything_ && !broadcasting_fmdn_ && !broadcasting_airtag_ &&
        !broadcasting_settings_) {
        hw_.adv_stop();
        broadcasting_anything_ = false;
        return;
    }
    if (broadcasting_settings_) {
        return;
    }
    if (broadcasting_anything_) {
        // Update existing broadcast
        if (broadcasting_airtag_) {
            hw_.adv_update_airtag();
        }
        if (broadcasting_fmdn_) {
            hw_.adv_update_fmdn();
        }
    } else {
        // Start broadcast — only one protocol at a time (alternation handles switching)
        int mult = cfg.mult_period;
        if (broadcasting_airtag_) {
            hw_.set_mac(ble_addr_);
            hw_.adv_start(false, kBroadcastIntervalMin * mult, kBroadcastIntervalMax * mult, false);
            broadcasting_anything_ = true;
        } else if (broadcasting_fmdn_) {
            hw_.adv_start(false, kBroadcastIntervalMin * mult, kBroadcastIntervalMax * mult, true);
            broadcasting_anything_ = true;
        }
    }
}

// ---- Initialize (startup sequence, main.c lines 677-776) ----

void StateMachine::initialize() {
    const auto& cfg = settings_.config();

    hw_.wdt_feed();
    hw_.blink_led(1, true);

    // Button check at start
    bool button_at_start = hw_.button_pressed();

    hw_.wdt_feed();
    // Settings are already loaded by caller (SettingsManager::load())

    if (button_at_start) {
        settings_.set_turned_on(true);
        hw_.update_turned_on(true);
    } else {
        if (!cfg.turned_on) {
            hw_.sleep_ms(500);
            hw_.power_off();
            state_ = State::ShuttingDown;
            return;
        }
    }

    // BQ25121A init (twice at start)
    hw_.bq_reinit(true);
    hw_.sleep_ms(10);
    hw_.bq_reinit(true);

    // Accelerometer init (retry up to 3 times)
    for (int i = 0; i < 3; i++) {
        if (hw_.accel_init() == 0) {
            break;
        }
    }
    // Turn off accelerometer if threshold is zero
    if (cfg.accel_threshold <= 0) {
        hw_.accel_powerdown();
    }

    // Prepare FMDN key
    hw_.prepare_fmdn(cfg.fmdn_key.data());

    // Prepare airtag key
    current_key_ = 0;
    if (cfg.num_keys > 0) {
        hw_.prepare_airtag(cfg.keys[0].data());
        set_mac_from_current_key();
    }

    // Set MAC address
    if (cfg.flag_airtag || cfg.flag_fmdn) {
        hw_.set_mac(ble_addr_);
    } else {
        if (cfg.settings_mac[0] != 0 && cfg.settings_mac[5] != 0) {
            hw_.set_mac(cfg.settings_mac.data());
        }
    }

    broadcasting_anything_ = false;
    broadcasting_airtag_ = false;
    broadcasting_fmdn_ = false;
    broadcasting_settings_ = false;

    update_battery();
    update_status();

    // Initialize Bluetooth
    hw_.bt_enable();
    hw_.set_tx_power(cfg.tx_power);
    hw_.wdt_feed();
    hw_.blink_led(2, false);

    // Broadcast iBeacon if no airtag or fmdn
    if (!cfg.flag_airtag && !cfg.flag_fmdn) {
        hw_.broadcast_ibeacon(last_battery_voltage_);
    }

    // Wait for button release
    while (hw_.button_pressed()) {
        hw_.sleep_ms(1000);
        hw_.wdt_feed();
    }

    state_ = State::Broadcasting;
}

// ---- Settings mode exit (main.c lines 790-833) ----

void StateMachine::handle_settings_mode_exit() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (now < timers_.end_settings) {
        return;
    }

    // Check if someone connected, wait for auth
    if (connected_gatt_ || authorized_gatt_) {
        hw_.sleep_ms(2000);
        hw_.wdt_feed();
    }
    if (authorized_gatt_) {
        hw_.sleep_ms(4000);
        hw_.wdt_feed();
    }

    // Check if firmware upload is requested
    if (pause_upload_) {
        state_ = State::FirmwareUpload;
        for (int i = 0; i < 15; i++) {
            hw_.sleep_ms(4000);
            hw_.store_time();
            hw_.wdt_feed();
        }
        hw_.reboot();
        return;
    }

    hw_.stop_settings_adv();
    broadcasting_settings_ = false;
    broadcasting_anything_ = false;
    broadcasting_fmdn_ = false;
    broadcasting_airtag_ = false;

    // Restore MAC address — only use AirTag-derived MAC if AirTag is active.
    // FMDN uses the controller's default MAC (or settings MAC if configured).
    hw_.bt_disable();
    if (cfg.flag_airtag) {
        hw_.set_mac(ble_addr_);
    }
    hw_.bt_enable();
    hw_.set_tx_power(cfg.tx_power);

    // Check if reset needed
    if (needs_reset_) {
        hw_.reboot();
        return;
    }

    // Broadcast iBeacon if no airtag or fmdn
    if (!cfg.flag_airtag && !cfg.flag_fmdn) {
        hw_.broadcast_ibeacon(last_battery_voltage_);
    }

    state_ = State::Broadcasting;
}

// ---- Battery check + UVLO (main.c lines 842-884) ----

void StateMachine::handle_battery_check() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (now < timers_.battery_check + static_cast<uint32_t>(kIntervalBatteryCheck)) {
        return;
    }
    timers_.battery_check = now;
    update_battery();
    update_status();

    if (cfg.flag_airtag || cfg.flag_fmdn) {
        broadcast();
    }
}

bool StateMachine::handle_uvlo_shutdown() {
    if (last_battery_voltage_ >= kBatteryLowVoltage) {
        bad_power_ = 0;
        return false;
    }

    // Skip UVLO for one hour if charging is detected
    if (hw_.is_charging()) {
        charge_lock_counter_ = kChargeLockDuration;
    }

    if (charge_lock_counter_ <= 0) {
        bad_power_++;
        if (bad_power_ > kUvloBadPowerThreshold) {
            hw_.adv_stop();
            hw_.bq_shipmode();
            hw_.power_off();
            state_ = State::ShuttingDown;
            return true;
        }
    } else {
        bad_power_ = 0;
    }
    return false;
}

bool StateMachine::handle_nokeys_shutdown() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (!cfg.flag_airtag && !cfg.flag_fmdn && !hw_.is_charging() &&
        now > static_cast<uint32_t>(kShutdownNokeys)) {
        hw_.blink_led(2, true);
        hw_.bq_shipmode();
        hw_.power_off();
        state_ = State::ShuttingDown;
        return true;
    }
    return false;
}

// ---- Settings mode entry (main.c lines 904-920) ----

void StateMachine::handle_settings_mode_entry() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (now < timers_.settings_mode + static_cast<uint32_t>(kIntervalSettings)) {
        return;
    }
    timers_.settings_mode = now;
    timers_.end_settings = now + static_cast<uint32_t>(kSettingsWait);

    hw_.adv_stop();
    hw_.bt_disable();

    // Use settings MAC in settings mode
    if (cfg.settings_mac[0] != 0 && cfg.settings_mac[5] != 0) {
        hw_.set_mac(cfg.settings_mac.data());
    }
    hw_.bt_enable();
    hw_.set_tx_power(0); // low power in settings mode

    hw_.start_settings_adv();
    broadcasting_settings_ = true;
    broadcasting_anything_ = true;
    broadcasting_fmdn_ = false;
    broadcasting_airtag_ = false;

    state_ = State::SettingsMode;
}

// ---- Max power burst (main.c lines 923-933) ----

void StateMachine::handle_max_power_burst() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    // Only burst if not already at max power, and both protocols active
    if (cfg.tx_power != 2 && cfg.flag_fmdn && cfg.flag_airtag) {
        if (now >= timers_.max_power + static_cast<uint32_t>(kIntervalMaxPower)) {
            timers_.max_power = now;
            timers_.end_max_power = now + static_cast<uint32_t>(2U * cfg.mult_period);
            hw_.set_tx_power(2);
        }
    }

    // Return to normal power after burst
    if (timers_.end_max_power > 0 && now >= timers_.end_max_power) {
        timers_.end_max_power = 0;
        hw_.set_tx_power(cfg.tx_power);
    }
}

// ---- Key rotation (main.c lines 936-955) ----

void StateMachine::handle_key_rotation() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (!cfg.flag_airtag || cfg.num_keys == 0) {
        return;
    }
    if (now < timers_.airtag_switch + static_cast<uint32_t>(cfg.change_interval)) {
        return;
    }
    timers_.airtag_switch = now;

    if (cfg.num_keys > 1) {
        current_key_++;
        if (current_key_ >= cfg.num_keys) {
            current_key_ = 0;
        }
    } else {
        current_key_ = 0;
    }
    keys_changes_++;

    hw_.adv_stop();
    hw_.bt_disable();
    broadcasting_anything_ = false;

    hw_.prepare_airtag(cfg.keys[static_cast<size_t>(current_key_)].data());
    set_mac_from_current_key();
    hw_.set_mac(ble_addr_);
    hw_.bt_enable();
    hw_.set_tx_power(cfg.tx_power);
    broadcast();
}

// ---- Button long-press (main.c lines 958-979) ----

bool StateMachine::handle_button_longpress() {
    if (!hw_.button_pressed()) {
        return false;
    }
    hw_.sleep_ms(1000);
    if (!hw_.button_pressed()) {
        return false;
    }
    hw_.wdt_feed();
    hw_.blink_led(2, true);

    // Wait for button release
    while (hw_.button_pressed()) {
        hw_.sleep_ms(500);
        hw_.wdt_feed();
    }

    settings_.set_turned_on(false);
    hw_.update_turned_on(false);
    hw_.sleep_ms(500);
    hw_.adv_stop();
    hw_.bt_disable();
    hw_.power_off();
    state_ = State::ShuttingDown;
    return true;
}

// ---- Initial broadcast start (main.c lines 983-989) ----

void StateMachine::handle_initial_broadcast() {
    const auto& cfg = settings_.config();

    if (!broadcasting_airtag_ && !broadcasting_fmdn_ && (cfg.flag_airtag || cfg.flag_fmdn)) {
        if (cfg.flag_airtag) {
            broadcasting_airtag_ = true;
        } else {
            broadcasting_fmdn_ = true;
        }
        broadcast();
    }
}

// ---- AirTag/FMDN alternation (main.c lines 993-1003) ----

void StateMachine::handle_airtag_fmdn_alternation() {
    const auto& cfg = settings_.config();

    if (!(cfg.flag_airtag && cfg.flag_fmdn)) {
        return;
    }
    if (broadcasting_airtag_) {
        broadcasting_airtag_ = false;
        broadcasting_fmdn_ = true;
    } else {
        broadcasting_airtag_ = true;
        broadcasting_fmdn_ = false;
    }
    broadcast();
}

// ---- Accelerometer read (main.c lines 1007-1015) ----

void StateMachine::handle_accelerometer() {
    const auto& cfg = settings_.config();
    uint32_t now = hw_.uptime_seconds();

    if (cfg.accel_threshold <= 0) {
        return;
    }
    if (now < timers_.accel_read + static_cast<uint32_t>(kIntervalAccelerometer)) {
        return;
    }
    hw_.accel_read();
    timers_.accel_read = now;

    // Re-init accelerometer every 30 read cycles
    uint32_t reset_interval = static_cast<uint32_t>(kIntervalAccelerometer) *
                              static_cast<uint32_t>(kAccelResetMultiplier);
    if (now >= timers_.accel_reset + reset_interval) {
        hw_.accel_init();
        timers_.accel_reset = now;
    }
}

// ---- Main tick (one iteration of while(1) loop) ----

void StateMachine::tick() {
    if (state_ == State::ShuttingDown) {
        return;
    }

    const auto& cfg = settings_.config();

    // Sleep based on state
    if (broadcasting_settings_) {
        hw_.sleep_ms(1000);
    } else {
        hw_.sleep_ms(static_cast<uint32_t>(1000 * cfg.mult_period));
    }
    hw_.wdt_feed();

    // Decrement charge lock counter
    if (charge_lock_counter_ > 0) {
        charge_lock_counter_ -= cfg.mult_period;
    }

    // ---- Settings mode handling ----
    if (state_ == State::SettingsMode || broadcasting_settings_) {
        handle_settings_mode_exit();
        return;
    }

    // ---- Normal broadcasting mode ----

    // Time save
    {
        uint32_t now = hw_.uptime_seconds();
        if (now >= timers_.time_save + static_cast<uint32_t>(kIntervalTimeSave)) {
            timers_.time_save = now;
            hw_.store_time();
        }
    }

    // Battery check
    handle_battery_check();

    // UVLO shutdown
    if (handle_uvlo_shutdown()) {
        return;
    }

    // No-keys shutdown
    if (handle_nokeys_shutdown()) {
        return;
    }

    // Settings mode entry (switches state and returns early like `continue`)
    {
        uint32_t now = hw_.uptime_seconds();
        if (now >= timers_.settings_mode + static_cast<uint32_t>(kIntervalSettings)) {
            handle_settings_mode_entry();
            return;
        }
    }

    // Max power burst
    handle_max_power_burst();

    // Key rotation
    handle_key_rotation();

    // Button long-press
    if (handle_button_longpress()) {
        return;
    }

    // Accelerometer — must run regardless of protocol alternation
    handle_accelerometer();

    // Initial broadcast start
    if (!broadcasting_airtag_ && !broadcasting_fmdn_ && (cfg.flag_airtag || cfg.flag_fmdn)) {
        handle_initial_broadcast();
        return; // mirrors `continue` in original
    }

    // AirTag/FMDN alternation
    if (cfg.flag_airtag && cfg.flag_fmdn) {
        handle_airtag_fmdn_alternation();
        return; // mirrors `continue` in original
    }
}

} // namespace beacon
