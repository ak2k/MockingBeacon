// zephyr_hardware.hpp -- IHardware implementation wrapping Zephyr APIs
#ifndef ZEPHYR_HARDWARE_HPP
#define ZEPHYR_HARDWARE_HPP

#include "beacon_config.hpp"
#include "ihardware.hpp"

namespace beacon {

class ZephyrHardware : public IHardware {
  public:
    explicit ZephyrHardware(SettingsManager& settings);

    uint32_t uptime_seconds() override;
    int bt_enable() override;
    void bt_disable() override;
    int adv_start(bool connectable, int interval_min, int interval_max, bool use_fmdn) override;
    int adv_stop() override;
    int adv_update_airtag() override;
    int adv_update_fmdn() override;
    void set_mac(const uint8_t* addr) override;
    void set_tx_power(int level) override;
    int battery_voltage() override;
    bool is_charging() override;
    void wdt_feed() override;
    void blink_led(int count, bool fast) override;
    void power_off() override;
    void reboot() override;
    bool button_pressed() override;
    void sleep_ms(uint32_t ms) override;
    void store_time() override;
    void prepare_airtag(const uint8_t* key) override;
    void prepare_fmdn(const uint8_t* key) override;
    void start_settings_adv() override;
    void stop_settings_adv() override;
    void broadcast_ibeacon(int batt_voltage) override;
    int accel_read() override;
    int accel_init() override;
    int accel_powerdown() override;
    void bq_reinit(bool force) override;
    void bq_shipmode() override;
    void update_turned_on(bool on) override;
    void set_status_bytes(uint8_t airtag_status, uint8_t fmdn_status) override;

  private:
    SettingsManager& settings_;
};

} // namespace beacon

#endif // ZEPHYR_HARDWARE_HPP
