// zephyr_hardware.cpp -- IHardware implementation + GATT glue bridge functions
#include "zephyr_hardware.hpp"

#ifndef HOST_TEST

#include <cstring>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/controller.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/poweroff.h>
#if defined(CONFIG_SOC_NRF54L15_CPUAPP)
#include <hal/nrf_memconf.h>
#endif
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#include "beacon_logic.hpp"
#include "beacon_state.hpp"
#include "ble_glue.h"
#include "gatt_glue.h"
#include "myboards.h"

// ---- C function externs ----
extern "C" {
#include "watchdog.h"
#ifdef USE_LIS2DW12
#include "lis2dw12.h"
#endif
#ifdef USE_BQ25121A
#include "bq2512x.h"
#endif
}

// ---- Globals needed by lis2dw12.c (was in settings.c) ----
extern "C" {
int accelThreshold = 800;
}

// ---- Battery check (ADC) ----
#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || !DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#ifndef NO_BATTERY_CHECK
#define NO_BATTERY_CHECK
#endif
#endif

#ifndef NO_BATTERY_CHECK
#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),
static const struct adc_dt_spec adc_channels[] = {
    DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};
#endif

// ---- GPIO specs ----
#if CONFIG_GPIO
#define LED_NODE DT_NODELABEL(led0)
static const struct gpio_dt_spec led_spec = GPIO_DT_SPEC_GET(LED_NODE, gpios);
#ifdef HAS_LED1_SWITCH
#define LED1_NODE DT_NODELABEL(led1)
static const struct gpio_dt_spec gate_spec = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
#endif
#endif

#ifdef HAS_I2C_KXTJ3
#define I2C0_NODE DT_NODELABEL(kxtj3)
static const struct i2c_dt_spec dev_i2c = I2C_DT_SPEC_GET(I2C0_NODE);
const struct device* const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
#endif

#ifdef USE_BUTTON
static const struct gpio_dt_spec sw0 = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
#endif

namespace beacon {

ZephyrHardware::ZephyrHardware(SettingsManager& settings) : settings_(settings) {}

uint32_t ZephyrHardware::uptime_seconds() {
    return static_cast<uint32_t>(k_uptime_seconds());
}

int ZephyrHardware::bt_enable() {
    int err = ::bt_enable(nullptr);
    if (err == 0) {
        bt_addr_le_t addrs[CONFIG_BT_ID_MAX];
        size_t count = CONFIG_BT_ID_MAX;
        ::bt_id_get(addrs, &count);
        if (count > 0) {
            printk("BT identity: %02X:%02X:%02X:%02X:%02X:%02X\n", addrs[0].a.val[5],
                   addrs[0].a.val[4], addrs[0].a.val[3], addrs[0].a.val[2], addrs[0].a.val[1],
                   addrs[0].a.val[0]);
        }
    }
    return err;
}

void ZephyrHardware::bt_disable() {
    ::bt_disable();
}

int ZephyrHardware::adv_start(bool connectable, int interval_min, int interval_max) {
    uint32_t options = BT_LE_ADV_OPT_USE_IDENTITY;
    if (connectable) {
        options |= BT_LE_ADV_OPT_CONNECTABLE;
    }
    // Determine which adv data to use based on what we're broadcasting
    // The caller (StateMachine) sets broadcasting_airtag/fmdn before calling
    // We check adv_airtag vs adv_fmdn based on the data pointer
    // For simplicity, the state machine's broadcast() method knows which to use
    // and calls adv_start for the first start, then adv_update for updates.
    // The actual data array is determined by what was last prepared.
    return ::bt_le_adv_start(BT_LE_ADV_PARAM(options, static_cast<uint32_t>(interval_min),
                                             static_cast<uint32_t>(interval_max), NULL),
                             adv_airtag, ADV_AIRTAG_COUNT, NULL, 0);
}

int ZephyrHardware::adv_stop() {
    return ::bt_le_adv_stop();
}

int ZephyrHardware::adv_update_airtag() {
    return ::bt_le_adv_update_data(adv_airtag, ADV_AIRTAG_COUNT, NULL, 0);
}

int ZephyrHardware::adv_update_fmdn() {
    return ::bt_le_adv_update_data(adv_fmdn, ADV_FMDN_COUNT, NULL, 0);
}

void ZephyrHardware::set_mac(const uint8_t* addr) {
    bt_addr_le_t myaddr;
    myaddr.type = BT_ADDR_LE_PUBLIC;
    std::memcpy(myaddr.a.val, addr, sizeof(myaddr.a.val));
    ::bt_ctlr_set_public_addr(myaddr.a.val);
}

void ZephyrHardware::set_tx_power(int level) {
    int pwr_level;
    switch (level) {
    case 0:
        pwr_level = TX_POWER_LOW;
        break;
    case 2:
        pwr_level = TX_POWER_HIGH;
        break;
    default:
        pwr_level = TX_POWER_NORMAL;
        break;
    }

    struct bt_hci_cp_vs_write_tx_power_level* cp;
    struct bt_hci_rp_vs_write_tx_power_level* rp;
    struct net_buf* buf;
    struct net_buf* rsp = nullptr;

    buf = ::bt_hci_cmd_create(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, sizeof(*cp));
    if (!buf) {
        printk("Unable to allocate command buffer\n");
        return;
    }

    cp = static_cast<struct bt_hci_cp_vs_write_tx_power_level*>(net_buf_add(buf, sizeof(*cp)));
    cp->handle = sys_cpu_to_le16(0);
    cp->handle_type = BT_HCI_VS_LL_HANDLE_TYPE_ADV;
    cp->tx_power_level = static_cast<int8_t>(pwr_level);

    int err = ::bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
    if (err) {
        printk("Set Tx power err: %d\n", err);
        return;
    }

    rp = reinterpret_cast<struct bt_hci_rp_vs_write_tx_power_level*>(rsp->data);
    printk("Actual Tx Power: %d\n", rp->selected_tx_power);
    net_buf_unref(rsp);
}

int ZephyrHardware::battery_voltage() {
#ifdef USE_BQ25121A
    return bq2512x_lastVoltage;
#elif !defined(NO_BATTERY_CHECK)
    int err;
    int32_t val_mv;
    int16_t buf = 0;
    int64_t sum_mv = 0;

#if CONFIG_PM_DEVICE_RUNTIME
    err = pm_device_action_run(DEVICE_DT_GET(DT_NODELABEL(adc)), PM_DEVICE_ACTION_RESUME);
    if (err) {
        printk("Warning: Can't turn on adc0 (err %d)\n", err);
    }
#endif
    struct adc_sequence sequence = {};
    sequence.buffer = &buf;
    sequence.buffer_size = sizeof(buf);

    if (!device_is_ready(adc_channels[0].dev)) {
        printk("ADC controller device not ready\n");
        return 0;
    }
    err = adc_channel_setup_dt(&adc_channels[0]);
    if (err < 0) {
        printk("Could not setup channel #0 (%d)\n", err);
        return 0;
    }
    (void)adc_sequence_init_dt(&adc_channels[0], &sequence);

    for (int i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        err = adc_read(adc_channels[0].dev, &sequence);
        if (err < 0) {
            printk("Could not read (%d)\n", err);
            return 0;
        }
        val_mv = buf;
        err = adc_raw_to_millivolts_dt(&adc_channels[0], &val_mv);
        if (err < 0) {
            printk(" (value in mV not available)\n");
            return 0;
        }
        k_sleep(K_MSEC(5));
        sum_mv += val_mv;
    }
    val_mv = static_cast<int32_t>(BATTERY_MULTIPLIER * (sum_mv / BATTERY_ADC_SAMPLES));
    printk("voltage level: %d\n", val_mv);

#if CONFIG_PM_DEVICE_RUNTIME
    err = pm_device_action_run(DEVICE_DT_GET(DT_NODELABEL(adc)), PM_DEVICE_ACTION_SUSPEND);
    if (err) {
        printk("Warning: Can't turn off adc0 (err %d)\n", err);
    }
#endif
    return val_mv;
#else
    return 0;
#endif
}

bool ZephyrHardware::is_charging() {
#ifdef USE_BQ25121A
    return bq2512x_is_charging() != 0;
#else
    return false;
#endif
}

void ZephyrHardware::wdt_feed() {
    ::my_wdt_feed();
}

void ZephyrHardware::blink_led(int count, bool fast) {
#if CONFIG_GPIO
    int err;
#if CONFIG_PM_DEVICE_RUNTIME
    err = pm_device_action_run(DEVICE_DT_GET(DT_NODELABEL(gpio0)), PM_DEVICE_ACTION_RESUME);
    if (err) {
        printk("Warning: Can't turn on gpio0 (err %d)\n", err);
    }
#endif
    gpio_pin_configure_dt(&led_spec, GPIO_OUTPUT_INACTIVE);
    k_sleep(fast ? K_MSEC(250) : K_MSEC(1000));
    for (int i = 0; i < (2 * count); i++) {
        gpio_pin_toggle_dt(&led_spec);
        k_sleep(fast ? K_MSEC(100) : K_MSEC(200));
    }
#if CONFIG_PM_DEVICE_RUNTIME
    gpio_pin_configure_dt(&led_spec, GPIO_DISCONNECTED);
    err = pm_device_action_run(DEVICE_DT_GET(DT_NODELABEL(gpio0)), PM_DEVICE_ACTION_SUSPEND);
    if (err) {
        printk("Warning: Can't turn off gpio0 (err %d)\n", err);
    }
#endif
#else
    (void)count;
    (void)fast;
#endif
}

void ZephyrHardware::power_off() {
    ::my_wdt_disable();
#ifdef USE_BUTTON
    int err = gpio_pin_interrupt_configure_dt(&sw0, GPIO_INT_LEVEL_ACTIVE);
    if (err < 0) {
        printk("Could not configure sw0 GPIO interrupt (%d)\n", err);
    }
#endif
#ifdef USE_ACCELEROMETER
    ::accel_powerdown();
#endif
    k_sleep(K_MSEC(500));
#if defined(CONFIG_SOC_NRF54L15_CPUAPP)
    /* Disable RAM retention in system off to minimize current draw.
     * See nrf/samples/bluetooth/peripheral_power_profiling/src/main.c */
    nrf_memconf_ramblock_ret_mask_enable_set(NRF_MEMCONF, 0, BIT_MASK(8), false);
    nrf_memconf_ramblock_ret2_mask_enable_set(NRF_MEMCONF, 0, BIT_MASK(8), false);
#endif
    sys_poweroff();
}

void ZephyrHardware::reboot() {
    printk("Resetting after changing settings or mcumgr (by WDT)\n");
    for (int i = 0; i < 8; i++) {
        store_time();
        k_sleep(K_MSEC(1000));
    }
    printk("wdt not reacted, resetting by sys_reboot\n");
    k_sleep(K_MSEC(1000));
    sys_reboot(SYS_REBOOT_COLD);
}

bool ZephyrHardware::button_pressed() {
#ifdef USE_BUTTON
    return gpio_pin_get(sw0.port, sw0.pin) > 0;
#else
    return false;
#endif
}

void ZephyrHardware::sleep_ms(uint32_t ms) {
    k_sleep(K_MSEC(ms));
}

void ZephyrHardware::store_time() {
    settings_.save_time(static_cast<uint32_t>(k_uptime_seconds()));
}

void ZephyrHardware::prepare_airtag(const uint8_t* key) {
    // Derive MAC from key
    derive_mac_from_key(key, bleAddr);

    // Fill advertisement template
    fill_adv_template(key, offline_finding_adv_template, OFFLINE_FINDING_ADV_TEMPLATE_SIZE);

    // Copy template to data store, preserving status byte
    uint8_t status_save = airtag_data_store[6];
    std::memcpy(airtag_data_store, offline_finding_adv_template, OFFLINE_FINDING_ADV_TEMPLATE_SIZE);
    airtag_data_store[6] = status_save;

    // Point adv_airtag at the data store (skip length + type bytes)
    adv_airtag[0].data = airtag_data_store + 2;
}

void ZephyrHardware::prepare_fmdn(const uint8_t* key) {
    uint8_t status_save = fmdn_data_store[23];
    std::memcpy(fmdn_data_store, adv_fmdn[1].data, adv_fmdn[1].data_len);
    std::memcpy(fmdn_data_store + 3, key, 20);
    adv_fmdn[1].data = fmdn_data_store;
    fmdn_data_store[23] = status_save;
}

void ZephyrHardware::start_settings_adv() {
    ::start_settings_adv();
}

void ZephyrHardware::stop_settings_adv() {
    ::stop_settings_adv();
}

void ZephyrHardware::broadcast_ibeacon(int batt_voltage) {
    printk("Starting broadcasting iBeacon\n");
    std::memcpy(iBeacon_data, adv_ibeacon[1].data, adv_ibeacon[1].data_len);
    adv_ibeacon[1].data = iBeacon_data;
    iBeacon_data[24] = static_cast<uint8_t>((batt_voltage + 50) / 100);

    int err = ::bt_le_adv_start(BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY, 11200, 12800, NULL),
                                adv_ibeacon, ADV_IBEACON_COUNT, NULL, 0);
    if (err) {
        printk("iBeacon advertising start failed (err %d)\n", err);
    }
}

int ZephyrHardware::accel_read() {
#ifdef USE_LIS2DW12
    return ::accel_read();
#else
    return -1;
#endif
}

int ZephyrHardware::accel_init() {
#ifdef USE_LIS2DW12
    return ::accel_init();
#else
    return -1;
#endif
}

int ZephyrHardware::accel_powerdown() {
#ifdef USE_LIS2DW12
    return ::accel_powerdown();
#else
    return -1;
#endif
}

void ZephyrHardware::bq_reinit(bool force) {
#ifdef USE_BQ25121A
    bq2512x_reinit(force ? 1 : 0, SYS_VOLTAGE, CHARGE_CURRENT, PRECHARGE_CURRENT);
#else
    (void)force;
#endif
}

void ZephyrHardware::bq_shipmode() {
#ifdef USE_BQ25121A
    bq2512x_shipmode();
#endif
}

void ZephyrHardware::update_turned_on(bool on) {
    settings_.set_turned_on(on);
    settings_.save_field(ID_turnedOn_NVS);
}

void ZephyrHardware::set_status_bytes(uint8_t airtag_status, uint8_t fmdn_status) {
    airtag_data_store[6] = airtag_status;
    fmdn_data_store[23] = fmdn_status;
}

} // namespace beacon

// ====================================================================
// GATT glue bridge functions (extern "C", called from gatt_glue.c)
// ====================================================================

extern "C" {

void glue_sync_gatt_state(void* state_machine) {
    auto* sm = static_cast<beacon::StateMachine*>(state_machine);
    if (!sm) return;
    sm->set_connected_gatt(connectedGatt != 0);
    sm->set_authorized_gatt(authorizedGatt != 0);
    sm->set_pause_upload(pauseUpload != 0);
    sm->set_needs_reset(needsReset != 0);
}

int beacon_glue_handle_auth(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    const auto& cfg = settings->config();

    if (len != cfg.auth_code.size()) {
        return -1;
    }

    // Check if this is an auth code update (already authorized)
    // The allowedChange flag is managed in gatt_glue.c

    // Check auth code
    auto result = beacon::validate_auth_code(buf, len, cfg.auth_code.data(), cfg.auth_code.size());
    if (result == beacon::GattResult::Ok) {
        return 1; // success
    }
    return 0; // failed
}

int beacon_glue_handle_key(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;

    // Called with NULL buf at disconnect to save keys
    if (buf == NULL) {
        settings->save_field(beacon::ID_key_NVS);
        return 0;
    }

    if (len != 14) {
        return -1;
    }

    // keysReceived is tracked in gatt_glue.c; we receive half-keys sequentially
    // We store the chunk into the keys array
    // The keysReceived counter in gatt_glue.c tells us which half
    extern int keysReceived;
    if (keysReceived >= 2 * beacon::kMaxKeysInMemory) {
        return -1;
    }

    int key_idx = keysReceived / 2;
    int half = keysReceived % 2;
    size_t offset = static_cast<size_t>(14 * half);

    settings->set_key_chunk(key_idx, offset, buf, 14);

    if (half == 1) {
        settings->set_num_keys(static_cast<uint8_t>(key_idx + 1));
    }
    return 0;
}

int beacon_glue_handle_period(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    int32_t value = 0;

    static const int32_t allowed[] = {1, 2, 4, 8};
    beacon::GattFieldSpec spec = {sizeof(int32_t), 0, 0, allowed, 4};
    auto result = beacon::validate_field(buf, len, true, spec, value);
    if (result == beacon::GattResult::Ok) {
        settings->set_mult_period(static_cast<uint8_t>(value));
        settings->save_field(beacon::ID_period_NVS);
    }
    return static_cast<int>(len);
}

int beacon_glue_handle_fmdn(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != sizeof(int32_t)) {
        return static_cast<int>(len);
    }
    int32_t value = 0;
    std::memcpy(&value, buf, sizeof(value));
    settings->set_flag_fmdn(value != 0);
    settings->save_field(beacon::ID_fmdn_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_airtag(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != sizeof(int32_t)) {
        return static_cast<int>(len);
    }
    int32_t value = 0;
    std::memcpy(&value, buf, sizeof(value));
    settings->set_flag_airtag(value != 0);
    settings->save_field(beacon::ID_airtag_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_change_interval(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    int32_t value = 0;
    beacon::GattFieldSpec spec = {sizeof(int32_t), 30, 7200, nullptr, 0};
    auto result = beacon::validate_field(buf, len, true, spec, value);
    if (result == beacon::GattResult::Ok) {
        settings->set_change_interval(static_cast<uint16_t>(value));
        settings->save_field(beacon::ID_changeInterval_NVS);
    }
    return static_cast<int>(len);
}

int beacon_glue_handle_tx_power(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    int32_t value = 0;
    beacon::GattFieldSpec spec = {sizeof(int32_t), 0, 2, nullptr, 0};
    auto result = beacon::validate_field(buf, len, true, spec, value);
    if (result == beacon::GattResult::Ok) {
        settings->set_tx_power(static_cast<uint8_t>(value));
        settings->save_field(beacon::ID_power_NVS);
    }
    return static_cast<int>(len);
}

int beacon_glue_handle_fmdn_key(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != 20) {
        return -1;
    }
    settings->set_fmdn_key(buf, len);
    settings->save_field(beacon::ID_fmdnKey_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_time_offset_write(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != sizeof(int64_t)) {
        return -1;
    }
    int64_t new_time = 0;
    std::memcpy(&new_time, buf, sizeof(new_time));
    settings->update_time_offset(new_time, static_cast<uint32_t>(k_uptime_seconds()));
    settings->save_field(beacon::ID_timeOffset_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_time_offset_read(void* sm, int64_t* out_time) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings || !out_time) return -1;
    *out_time = settings->get_time(static_cast<uint32_t>(k_uptime_seconds()));
    return 0;
}

int beacon_glue_handle_settings_mac(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != 6) {
        return -1;
    }
    settings->set_settings_mac(buf, len);
    settings->save_field(beacon::ID_settingsMAC_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_status(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    if (len != sizeof(int32_t)) {
        return -1;
    }
    int32_t value = 0;
    std::memcpy(&value, buf, sizeof(value));
    settings->set_status_flags(static_cast<uint32_t>(value));
    settings->save_field(beacon::ID_status_NVS);
    return static_cast<int>(len);
}

int beacon_glue_handle_accel(void* sm, const uint8_t* buf, uint16_t len) {
    auto* settings = static_cast<beacon::SettingsManager*>(sm);
    if (!settings) return -1;
    int32_t value = 0;
    beacon::GattFieldSpec spec = {sizeof(int32_t), 0, 16383, nullptr, 0};
    auto result = beacon::validate_field(buf, len, true, spec, value);
    if (result == beacon::GattResult::Ok) {
        settings->set_accel_threshold(static_cast<uint16_t>(value));
        // Sync the C global used by lis2dw12.c
        accelThreshold = static_cast<int>(value);
        settings->save_field(beacon::ID_accel_NVS);
    }
    return static_cast<int>(len);
}

} // extern "C"

#endif // !HOST_TEST
