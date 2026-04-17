// ihardware.hpp — Hardware abstraction interface for StateMachine
#ifndef IHARDWARE_HPP
#define IHARDWARE_HPP

#include <cstdint>

namespace beacon {

class IHardware {
  public:
    /// Current system uptime in seconds (wraps at ~136 years, not a concern).
    virtual uint32_t uptime_seconds() = 0;

    /// Initialize Bluetooth subsystem. Returns 0 on success.
    virtual int bt_enable() = 0;
    /// Disable Bluetooth subsystem (required before changing MAC address).
    virtual void bt_disable() = 0;

    /// Start AirTag non-connectable advertising.
    virtual int adv_start_airtag(int interval_min, int interval_max) = 0;
    /// Start FMDN non-connectable advertising.
    virtual int adv_start_fmdn(int interval_min, int interval_max) = 0;
    /// Stop all advertising (broadcast + settings).
    virtual int adv_stop() = 0;
    /// Update in-flight advertisement payload (AirTag Offline Finding format).
    virtual int adv_update_airtag() = 0;
    /// Update in-flight advertisement payload (FMDN Eddystone-EID format).
    virtual int adv_update_fmdn() = 0;

    /// Set the BLE public MAC address (6 bytes, LSB first).
    /// Must be called while BT is disabled; takes effect on next bt_enable().
    virtual void set_mac(const uint8_t* addr) = 0;
    /// Set TX power level: 0 = -8 dBm (low), 1 = 0 dBm (normal), 2 = +4 dBm (high).
    virtual void set_tx_power(int level) = 0;

    /// Read battery voltage in millivolts. Re-reads ADC (or BQ25121A register).
    virtual int battery_voltage() = 0;
    /// Returns true if external charging is detected (BQ25121A only).
    virtual bool is_charging() = 0;

    virtual void wdt_feed() = 0;
    /// Blink the LED. @param count number of blinks, @param fast true=100ms, false=200ms period.
    virtual void blink_led(int count, bool fast) = 0;
    /// Enter deep sleep (sys_poweroff). Configures wake-on-button before sleeping.
    virtual void power_off() = 0;
    /// Cold reboot via watchdog timeout (waits ~8s storing time, then sys_reboot).
    virtual void reboot() = 0;
    virtual bool button_pressed() = 0;
    virtual void sleep_ms(uint32_t ms) = 0;
    /// Persist current time to NVS (called hourly and before shutdown).
    virtual void store_time() = 0;

    /// Set up AirTag advertisement payload from a 28-byte public key.
    /// Copies key bytes into the Offline Finding template, preserving the status byte.
    virtual void prepare_airtag(const uint8_t* key) = 0;
    /// Set up FMDN advertisement payload from a 20-byte key.
    /// Currently only one FMDN key is supported (no rotation).
    virtual void prepare_fmdn(const uint8_t* key) = 0;

    /// Start connectable advertising for GATT settings service.
    /// Resets GATT auth state (connectedGatt, authorizedGatt, etc.).
    virtual void start_settings_adv() = 0;
    /// Stop settings advertising and disconnect all GATT clients.
    virtual void stop_settings_adv() = 0;
    /// Start iBeacon broadcast (fallback when no AirTag/FMDN keys).
    /// Interval: ~7-8 seconds (11200-12800 BLE units). Embeds battery voltage in minor field.
    virtual void broadcast_ibeacon(int battery_voltage) = 0;

    /// Read accelerometer FIFO and feed samples to MovementTracker. Returns 0 on success.
    virtual int accel_read() = 0;
    /// Initialize accelerometer (LIS2DW12). Retried up to 3 times at startup. Returns 0 on success.
    virtual int accel_init() = 0;
    /// Power down accelerometer (when accel_threshold == 0).
    virtual int accel_powerdown() = 0;

    /// (Re)initialize BQ25121A charger IC. @param force true at startup, false for periodic
    /// refresh.
    virtual void bq_reinit(bool force) = 0;
    /// Enter BQ25121A shipping mode (ultra-low-power, requires button press to wake).
    virtual void bq_shipmode() = 0;
    /// Persist turned_on flag to NVS (called after button wake/shutdown).
    virtual void update_turned_on(bool on) = 0;

    /// Enable DFU (register SMP GATT service). No-op if DFU not compiled in.
    virtual void enable_dfu() {}
    /// Disable DFU (unregister SMP GATT service). No-op if DFU not compiled in.
    virtual void disable_dfu() {}

    /// Write the computed status bytes into the advertisement data stores.
    /// Called after compute_status() to propagate results to BLE payloads.
    /// airtag_status -> airtag_data_store[6], fmdn_status -> fmdn_data_store[23]
    virtual void set_status_bytes(uint8_t airtag_status, uint8_t fmdn_status) = 0;

  protected:
    ~IHardware() = default;
};

} // namespace beacon

#endif // IHARDWARE_HPP
