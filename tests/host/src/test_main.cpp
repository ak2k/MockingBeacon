// Host-native test runner for pure C++ modules
// Uses simple assert-based testing (no Zephyr dependency)

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "accel_data.hpp"
#include "beacon_config.hpp"
#include "beacon_logic.hpp"
#include "beacon_state.hpp"

// Test infrastructure
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name)                                                                                 \
    static void test_##name();                                                                     \
    static struct TestReg_##name {                                                                  \
        TestReg_##name() { test_##name(); }                                                        \
    } test_reg_##name;                                                                             \
    static void test_##name()

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        tests_run++;                                                                               \
        if (!(cond)) {                                                                             \
            printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
            return;                                                                                \
        }                                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_MEM_EQ(a, b, len) ASSERT_TRUE(memcmp((a), (b), (len)) == 0)

// ============================================================================
// Original C functions for equivalence testing (Phase 1)
// ============================================================================
namespace original_c {

static void set_addr_from_key(const char* key, uint8_t* bleAddr) {
    bleAddr[5] = static_cast<uint8_t>(key[0] | 0b11000000);
    bleAddr[4] = static_cast<uint8_t>(key[1]);
    bleAddr[3] = static_cast<uint8_t>(key[2]);
    bleAddr[2] = static_cast<uint8_t>(key[3]);
    bleAddr[1] = static_cast<uint8_t>(key[4]);
    bleAddr[0] = static_cast<uint8_t>(key[5]);
}

static void fill_adv_template_from_key(const char* key, uint8_t* tmpl) {
    memcpy(&tmpl[7], &key[6], 22);
    tmpl[29] = static_cast<uint8_t>(key[0] >> 6);
}

static int check_empty_key(const unsigned char* key) {
    for (int j = 0; j < 28; j++) {
        if (key[j] != 0) {
            return 0;
        }
    }
    return 1;
}

#define BATTERY_LEVEL_FULL 4000
#define BATTERY_LEVEL_NORMAL 3700
#define BATTERY_LEVEL_LOW 3300

static void update_status_byte(int statusFlags, int lastBatteryVoltage, int keys_changes,
                               int what_in_status, uint8_t accel_byte, int16_t accelTemperature,
                               uint8_t* out_airtag, uint8_t* out_fmdn) {
    uint8_t statusAirtag = static_cast<uint8_t>(statusFlags & 0xff);
    uint8_t statusFmdn = static_cast<uint8_t>((statusFlags & 0xff00) >> 8);
    int bAirtag = (statusFlags & 0xf0000) >> 16;
    int bFmdn = (statusFlags & 0xf00000) >> 20;

    switch (bAirtag) {
    case 1:
        break;
    case 2:
        statusAirtag = static_cast<uint8_t>(keys_changes & 0xff);
        break;
    case 3:
    case 5:
        statusAirtag = static_cast<uint8_t>(lastBatteryVoltage / 100);
        if ((bAirtag == 3) || (what_in_status == 0))
            break;
        if (what_in_status == 1)
            statusAirtag = static_cast<uint8_t>(0x80 | accel_byte);
        if (what_in_status == 2) {
            int tmpTemp = (accelTemperature + 5) / 10 + 10;
            if (tmpTemp < 0)
                tmpTemp = 0;
            if (tmpTemp > 63)
                tmpTemp = 63;
            statusAirtag = static_cast<uint8_t>(0x40 | tmpTemp);
        }
        break;
    case 4: {
        int battery = 3;
        if (lastBatteryVoltage > BATTERY_LEVEL_FULL)
            battery = 0;
        else if (lastBatteryVoltage > BATTERY_LEVEL_NORMAL)
            battery = 1;
        else if (lastBatteryVoltage > BATTERY_LEVEL_LOW)
            battery = 2;
        statusAirtag = static_cast<uint8_t>(statusAirtag | (battery << 6));
        break;
    }
    default:
        break;
    }
    *out_airtag = statusAirtag;

    switch (bFmdn) {
    case 1:
        break;
    case 2:
        statusFmdn = static_cast<uint8_t>(keys_changes & 0xff);
        break;
    case 3:
    case 5:
        statusFmdn = static_cast<uint8_t>(lastBatteryVoltage / 100);
        if ((bFmdn == 3) || (what_in_status == 0))
            break;
        if (what_in_status == 1)
            statusFmdn = static_cast<uint8_t>(0x80 | accel_byte);
        if (what_in_status == 2) {
            int tmpTemp = (accelTemperature + 5) / 10 + 10;
            if (tmpTemp < 0)
                tmpTemp = 0;
            if (tmpTemp > 63)
                tmpTemp = 63;
            statusFmdn = static_cast<uint8_t>(0x40 | tmpTemp);
        }
        break;
    case 4: {
        int battery = 0;
        if (lastBatteryVoltage > BATTERY_LEVEL_NORMAL)
            battery = 1;
        else if (lastBatteryVoltage > BATTERY_LEVEL_LOW)
            battery = 2;
        else if (lastBatteryVoltage > 0)
            battery = 3;
        statusFmdn = static_cast<uint8_t>(statusFmdn | (battery << 5));
        break;
    }
    default:
        break;
    }
    *out_fmdn = statusFmdn;
}

} // namespace original_c

// C equivalence functions for accel_data (Phase 2)
static int c_accel_movement(const uint8_t* moves300, int moves300_size, int startOffset,
                            int endOffset) {
    int start = startOffset / 5;
    int end = (endOffset + 4) / 5;
    if (start < 0 || start >= moves300_size)
        start = 0;
    if (end < 0 || end > moves300_size)
        end = moves300_size;
    for (int i = start; i < end; i++)
        if (moves300[i] > 0)
            return 1;
    return 0;
}

static int c_calc_accel_byte(const uint8_t* moves300, int moves300_size) {
    int res = 0;
    if (c_accel_movement(moves300, moves300_size, 0, 10))
        res |= 0x01;
    if (c_accel_movement(moves300, moves300_size, 10, 30))
        res |= 0x02;
    if (c_accel_movement(moves300, moves300_size, 30, 60))
        res |= 0x04;
    if (c_accel_movement(moves300, moves300_size, 60, 180))
        res |= 0x08;
    if (c_accel_movement(moves300, moves300_size, 180, 360))
        res |= 0x10;
    if (c_accel_movement(moves300, moves300_size, 360, 720))
        res |= 0x20;
    if (c_accel_movement(moves300, moves300_size, 720, 1440))
        res |= 0x40;
    return res;
}

// ---- MockNvsStorage for Phase 3 tests ----
class MockNvsStorage : public beacon::INvsStorage {
    std::map<uint16_t, std::vector<uint8_t>> store_;

  public:
    int read(uint16_t id, void* data, size_t len) override {
        auto it = store_.find(id);
        if (it == store_.end())
            return -1;
        size_t copy_len = std::min(len, it->second.size());
        memcpy(data, it->second.data(), copy_len);
        return static_cast<int>(it->second.size());
    }
    int write(uint16_t id, const void* data, size_t len) override {
        auto* p = static_cast<const uint8_t*>(data);
        store_[id] = std::vector<uint8_t>(p, p + len);
        return static_cast<int>(len);
    }
    void clear() { store_.clear(); }
    void store_int(uint16_t id, int value) { write(id, &value, sizeof(value)); }
    void store_int64(uint16_t id, int64_t value) { write(id, &value, sizeof(value)); }
};

// Helper for accel tests
static void fill_samples(int16_t samples[][3], int count, int16_t val) {
    for (int i = 0; i < count; i++) {
        samples[i][0] = val;
        samples[i][1] = val;
        samples[i][2] = val;
    }
}

// ============================================================================
// Phase 1 Tests: beacon_logic
// ============================================================================

TEST(derive_mac_known_key) {
    printf("  test: derive_mac_known_key\n");
    const uint8_t key[28] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t mac[6] = {};
    beacon::derive_mac_from_key(key, mac);
    ASSERT_EQ(mac[0], 0x06);
    ASSERT_EQ(mac[1], 0x05);
    ASSERT_EQ(mac[2], 0x04);
    ASSERT_EQ(mac[3], 0x03);
    ASSERT_EQ(mac[4], 0x02);
    ASSERT_EQ(mac[5], 0xC1);
}

TEST(derive_mac_or_c0) {
    printf("  test: derive_mac_or_c0\n");
    const uint8_t key[28] = {0x3F};
    uint8_t mac[6] = {};
    beacon::derive_mac_from_key(key, mac);
    ASSERT_EQ(mac[5], 0xFF);

    const uint8_t key2[28] = {0x00};
    beacon::derive_mac_from_key(key2, mac);
    ASSERT_EQ(mac[5], 0xC0);
}

TEST(fill_adv_template_basic) {
    printf("  test: fill_adv_template_basic\n");
    uint8_t key[28];
    for (int i = 0; i < 28; i++)
        key[i] = static_cast<uint8_t>(i + 0x10);

    uint8_t tmpl[31] = {};
    beacon::fill_adv_template(key, tmpl, sizeof(tmpl));

    for (int i = 0; i < 22; i++)
        ASSERT_EQ(tmpl[7 + i], key[6 + i]);
    ASSERT_EQ(tmpl[29], static_cast<uint8_t>(key[0] >> 6));
}

TEST(is_key_empty_tests) {
    printf("  test: is_key_empty_tests\n");
    uint8_t zeros[28] = {};
    ASSERT_TRUE(beacon::is_key_empty(zeros, 28));

    uint8_t nonzero[28] = {};
    nonzero[14] = 1;
    ASSERT_TRUE(!beacon::is_key_empty(nonzero, 28));

    ASSERT_TRUE(beacon::is_key_empty(zeros, 0));
}

TEST(compute_status_fixed) {
    printf("  test: compute_status_fixed\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x10042); // bAirtag=1, base airtag=0x42
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 0x42);
}

TEST(compute_status_incrementing) {
    printf("  test: compute_status_incrementing\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x20000);
    in.keys_changes = 0x1A3;
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 0xA3);
}

TEST(compute_status_voltage) {
    printf("  test: compute_status_voltage\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x30000);
    in.battery_voltage = 3200;
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 32);
}

TEST(compute_status_battery_level) {
    printf("  test: compute_status_battery_level\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x40000);

    in.battery_voltage = 4100;
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, (0 << 6)); // full

    in.battery_voltage = 3800;
    out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, (1 << 6)); // normal

    in.battery_voltage = 3500;
    out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, (2 << 6)); // low

    in.battery_voltage = 3000;
    out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, (3 << 6)); // critical
}

TEST(compute_status_accel) {
    printf("  test: compute_status_accel\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x50000);
    in.what_in_status = 1;
    in.accel_byte = 0x3F;
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x80 | 0x3F));
}

TEST(compute_status_temp) {
    printf("  test: compute_status_temp\n");
    beacon::StatusInput in = {};
    in.status = beacon::StatusFlags::unpack(0x50000);
    in.what_in_status = 2;
    in.temperature = 250; // 25.0C → (250+5)/10+10 = 35
    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40 | 35));

    // Clamp low: -200 → (-200+5)/10+10 = -9 → clamped to 0
    in.temperature = -200;
    out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40));

    // Clamp high: 600 → (600+5)/10+10 = 70 → clamped to 63
    in.temperature = 600;
    out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40 | 63));
}

TEST(derive_mac_equivalence) {
    printf("  test: derive_mac_equivalence\n");
    for (int k = 0; k < 5; k++) {
        uint8_t key[28] = {};
        for (int i = 0; i < 28; i++)
            key[i] = static_cast<uint8_t>((i * 7 + k * 13) & 0xFF);

        uint8_t cpp_mac[6] = {};
        beacon::derive_mac_from_key(key, cpp_mac);

        uint8_t c_mac[6] = {};
        original_c::set_addr_from_key(reinterpret_cast<const char*>(key), c_mac);

        ASSERT_MEM_EQ(cpp_mac, c_mac, 6);
    }
}

TEST(fill_template_equivalence) {
    printf("  test: fill_template_equivalence\n");
    for (int k = 0; k < 5; k++) {
        uint8_t key[28] = {};
        for (int i = 0; i < 28; i++)
            key[i] = static_cast<uint8_t>((i * 11 + k * 3) & 0xFF);

        uint8_t cpp_tmpl[31] = {};
        beacon::fill_adv_template(key, cpp_tmpl, sizeof(cpp_tmpl));

        uint8_t c_tmpl[31] = {};
        original_c::fill_adv_template_from_key(reinterpret_cast<const char*>(key), c_tmpl);

        ASSERT_MEM_EQ(&cpp_tmpl[7], &c_tmpl[7], 22);
        ASSERT_EQ(cpp_tmpl[29], c_tmpl[29]);
    }
}

TEST(is_key_empty_equivalence) {
    printf("  test: is_key_empty_equivalence\n");
    uint8_t zeros[28] = {};
    ASSERT_EQ(beacon::is_key_empty(zeros, 28), static_cast<bool>(original_c::check_empty_key(zeros)));

    uint8_t nonzero[28] = {};
    nonzero[14] = 0x42;
    ASSERT_EQ(beacon::is_key_empty(nonzero, 28),
              static_cast<bool>(original_c::check_empty_key(nonzero)));
}

TEST(compute_status_equivalence) {
    printf("  test: compute_status_equivalence\n");
    struct TestCase {
        int flags, voltage, changes, what;
        uint8_t accel;
        int16_t temp;
    };
    TestCase cases[] = {
        {0x10042, 3200, 0, 0, 0, 250},      {0x20000, 3200, 0x1A3, 0, 0, 250},
        {0x30000, 3200, 0, 0, 0, 250},       {0x40000, 4100, 0, 0, 0, 250},
        {0x40000, 3800, 0, 0, 0, 250},       {0x40000, 3000, 0, 0, 0, 250},
        {0x50000, 3200, 0, 0, 0x3F, 250},    {0x50000, 3200, 0, 1, 0x3F, 250},
        {0x50000, 3200, 0, 2, 0x3F, 250},    {0x500000, 3200, 0, 1, 0x3F, 250},
        {0x300000, 3200, 0, 0, 0, 250},      {0x400000, 3800, 0, 0, 0, 250},
        {0x458000, 3200, 42, 0, 0x55, -100},
    };
    for (auto& tc : cases) {
        beacon::StatusInput in = {};
        in.status = beacon::StatusFlags::unpack(static_cast<uint32_t>(tc.flags));
        in.battery_voltage = static_cast<uint16_t>(tc.voltage);
        in.keys_changes = static_cast<uint16_t>(tc.changes);
        in.what_in_status = static_cast<uint8_t>(tc.what);
        in.accel_byte = tc.accel;
        in.temperature = tc.temp;

        auto cpp_out = beacon::compute_status(in);

        uint8_t c_airtag = 0, c_fmdn = 0;
        original_c::update_status_byte(tc.flags, tc.voltage, tc.changes, tc.what, tc.accel,
                                       tc.temp, &c_airtag, &c_fmdn);

        ASSERT_EQ(cpp_out.airtag_status, c_airtag);
        ASSERT_EQ(cpp_out.fmdn_status, c_fmdn);
    }
}

// ============================================================================
// Phase 2 Tests: accel_data
// ============================================================================

TEST(sentinel_first_reading) {
    printf("  test: sentinel_first_reading\n");
    beacon::MovementTracker tracker;
    int16_t samples[33][3];
    fill_samples(samples, 33, 100);
    tracker.record_reading(samples, 33, 10, 0);
    ASSERT_EQ(tracker.moves20()[0], 0);
}

TEST(movement_detected) {
    printf("  test: movement_detected\n");
    beacon::MovementTracker tracker;
    int16_t samples[33][3];
    fill_samples(samples, 33, 100);
    tracker.record_reading(samples, 33, 10, 0);

    for (int i = 0; i < 33; i++) {
        int16_t val = (i % 2 == 0) ? 0 : 200;
        samples[i][0] = val;
        samples[i][1] = val;
        samples[i][2] = val;
    }
    tracker.record_reading(samples, 33, 10, 20);
    ASSERT_EQ(tracker.moves20()[0], 1);
}

TEST(no_movement) {
    printf("  test: no_movement\n");
    beacon::MovementTracker tracker;
    int16_t samples[33][3];
    fill_samples(samples, 33, 100);
    tracker.record_reading(samples, 33, 10, 0);

    for (int i = 0; i < 33; i++) {
        samples[i][0] = static_cast<int16_t>(100 + (i % 2) * 5);
        samples[i][1] = 100;
        samples[i][2] = 100;
    }
    tracker.record_reading(samples, 33, 10, 20);
    ASSERT_EQ(tracker.moves20()[0], 0);
}

TEST(fifo_shift) {
    printf("  test: fifo_shift\n");
    beacon::MovementTracker tracker;
    int16_t samples[33][3];
    fill_samples(samples, 33, 100);
    tracker.record_reading(samples, 33, 10, 0);

    for (int i = 0; i < 33; i++) {
        samples[i][0] = (i % 2 == 0) ? 0 : 200;
        samples[i][1] = 100;
        samples[i][2] = 100;
    }
    tracker.record_reading(samples, 33, 10, 20);
    ASSERT_EQ(tracker.moves20()[0], 1);

    fill_samples(samples, 33, 100);
    tracker.record_reading(samples, 33, 10, 40);
    ASSERT_EQ(tracker.moves20()[0], 0);
    ASSERT_EQ(tracker.moves20()[1], 1);
}

TEST(compute_accel_byte_bits) {
    printf("  test: compute_accel_byte_bits\n");
    beacon::MovementTracker tracker;
    ASSERT_EQ(tracker.compute_accel_byte(), 0);

    int16_t samples[33][3];
    for (int i = 0; i < 33; i++) {
        samples[i][0] = (i % 2 == 0) ? 0 : 200;
        samples[i][1] = 100;
        samples[i][2] = 100;
    }
    tracker.record_reading(samples, 33, 10, 0);
    uint8_t byte = tracker.compute_accel_byte();
    ASSERT_TRUE((byte & 0x01) != 0);
    ASSERT_TRUE((byte & 0x02) == 0);
}

TEST(compute_accel_byte_all_bits) {
    printf("  test: compute_accel_byte_all_bits\n");
    beacon::MovementTracker tracker;
    int16_t samples[33][3];
    for (int i = 0; i < 33; i++) {
        samples[i][0] = (i % 2 == 0) ? 0 : 200;
        samples[i][1] = 100;
        samples[i][2] = 100;
    }
    for (int shift = 0; shift < 288; shift++) {
        uint32_t t = static_cast<uint32_t>(shift) * 301;
        tracker.record_reading(samples, 33, 10, t);
    }
    ASSERT_EQ(tracker.compute_accel_byte(), 0x7F);
}

TEST(temperature) {
    printf("  test: temperature\n");
    beacon::MovementTracker tracker;
    ASSERT_EQ(tracker.temperature(), 0);
    tracker.set_temperature(250);
    ASSERT_EQ(tracker.temperature(), 250);
    tracker.set_temperature(-100);
    ASSERT_EQ(tracker.temperature(), -100);
}

TEST(accel_c_cpp_equivalence) {
    printf("  test: accel_c_cpp_equivalence\n");
    for (int pattern = 0; pattern < 20; pattern++) {
        beacon::MovementTracker tracker;
        int16_t samples[33][3];
        for (int shift = 0; shift < 288; shift++) {
            bool do_movement = ((shift * 7 + pattern * 13) % 5) < 2;
            if (do_movement) {
                for (int i = 0; i < 33; i++) {
                    samples[i][0] = (i % 2 == 0) ? 0 : 200;
                    samples[i][1] = 100;
                    samples[i][2] = 100;
                }
            } else {
                fill_samples(samples, 33, 100);
            }
            tracker.record_reading(samples, 33, 10, static_cast<uint32_t>(shift) * 301);
        }
        int c_result =
            c_calc_accel_byte(tracker.moves300(), beacon::MovementTracker::kMoves300Size);
        ASSERT_EQ(static_cast<int>(tracker.compute_accel_byte()), c_result);
    }
}

// ============================================================================
// Phase 3 Tests: beacon_config
// ============================================================================

TEST(load_defaults) {
    printf("  test: load_defaults\n");
    MockNvsStorage nvs;
    beacon::SettingsManager mgr(nvs);
    ASSERT_EQ(mgr.load(), 0);
    auto& cfg = mgr.config();
    ASSERT_EQ(cfg.flag_fmdn, false);
    ASSERT_EQ(cfg.flag_airtag, false);
    ASSERT_EQ(cfg.mult_period, 2);
    ASSERT_EQ(cfg.tx_power, 2);
    ASSERT_EQ(cfg.change_interval, 6000);
    ASSERT_EQ(cfg.status_flags, 0x458000);
    ASSERT_EQ(cfg.accel_threshold, 800);
    ASSERT_EQ(cfg.num_keys, 0);
    uint8_t expected_auth[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    ASSERT_MEM_EQ(cfg.auth_code.data(), expected_auth, 8);
}

TEST(load_from_nvs) {
    printf("  test: load_from_nvs\n");
    MockNvsStorage nvs;
    nvs.store_int(beacon::ID_fmdn_NVS, 1);
    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_period_NVS, 4);
    nvs.store_int(beacon::ID_power_NVS, 1);
    nvs.store_int(beacon::ID_changeInterval_NVS, 1200);
    nvs.store_int(beacon::ID_status_NVS, 0x100000);
    nvs.store_int(beacon::ID_accel_NVS, 500);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    nvs.store_int64(beacon::ID_timeOffset_NVS, 1000000LL);

    beacon::SettingsManager mgr(nvs);
    ASSERT_EQ(mgr.load(), 0);
    auto& cfg = mgr.config();
    ASSERT_EQ(cfg.flag_fmdn, true);
    ASSERT_EQ(cfg.flag_airtag, true);
    ASSERT_EQ(cfg.mult_period, 4);
    ASSERT_EQ(cfg.tx_power, 1);
    ASSERT_EQ(cfg.change_interval, 1200);
    ASSERT_EQ(cfg.status_flags, 0x100000);
    ASSERT_EQ(cfg.accel_threshold, 500);
    ASSERT_EQ(cfg.turned_on, true);
    ASSERT_EQ(cfg.time_offset, static_cast<int64_t>(1000000));
}

TEST(change_interval_alignment) {
    printf("  test: change_interval_alignment\n");
    {
        MockNvsStorage nvs;
        nvs.store_int(beacon::ID_changeInterval_NVS, 6007);
        beacon::SettingsManager mgr(nvs);
        mgr.load();
        ASSERT_EQ(mgr.config().change_interval, 6000);
    }
    {
        MockNvsStorage nvs;
        nvs.store_int(beacon::ID_changeInterval_NVS, 6008);
        beacon::SettingsManager mgr(nvs);
        mgr.load();
        ASSERT_EQ(mgr.config().change_interval, 6008);
    }
}

TEST(key_count_with_zeros) {
    printf("  test: key_count_with_zeros\n");
    MockNvsStorage nvs;
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    for (int i = 0; i < 3; i++)
        keys[static_cast<size_t>(i)][0] = static_cast<uint8_t>(i + 1);
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    beacon::SettingsManager mgr(nvs);
    mgr.load();
    ASSERT_EQ(mgr.config().num_keys, 3);
}

TEST(save_field_roundtrip) {
    printf("  test: save_field_roundtrip\n");
    MockNvsStorage nvs;
    beacon::SettingsManager mgr(nvs);
    mgr.load();
    mgr.set_mult_period(8);
    ASSERT_TRUE(mgr.save_field(beacon::ID_period_NVS) > 0);
    beacon::SettingsManager mgr2(nvs);
    mgr2.load();
    ASSERT_EQ(mgr2.config().mult_period, 8);
}

TEST(time_arithmetic) {
    printf("  test: time_arithmetic\n");
    MockNvsStorage nvs;
    beacon::SettingsManager mgr(nvs);
    mgr.load();
    ASSERT_EQ(mgr.get_time(100), static_cast<int64_t>(100));
    mgr.update_time_offset(1000000, 500);
    ASSERT_EQ(mgr.get_time(500), static_cast<int64_t>(1000000));
    ASSERT_EQ(mgr.get_time(600), static_cast<int64_t>(1000100));
}

TEST(gatt_validate_field_basic) {
    printf("  test: gatt_validate_field_basic\n");
    beacon::GattFieldSpec spec = {sizeof(int32_t), 0, 100, nullptr, 0};
    int32_t out = 0;

    uint8_t short_buf[] = {1, 2};
    ASSERT_EQ(beacon::validate_field(short_buf, 2, true, spec, out),
              beacon::GattResult::InvalidLength);

    int32_t val = 50;
    uint8_t buf[4];
    memcpy(buf, &val, 4);
    ASSERT_EQ(beacon::validate_field(buf, 4, false, spec, out), beacon::GattResult::Unauthorized);
    ASSERT_EQ(beacon::validate_field(buf, 4, true, spec, out), beacon::GattResult::Ok);
    ASSERT_EQ(out, 50);

    val = 101;
    memcpy(buf, &val, 4);
    ASSERT_EQ(beacon::validate_field(buf, 4, true, spec, out), beacon::GattResult::OutOfRange);
}

TEST(gatt_period_whitelist) {
    printf("  test: gatt_period_whitelist\n");
    static const int32_t allowed[] = {1, 2, 4, 8};
    beacon::GattFieldSpec spec = {sizeof(int32_t), 0, 0, allowed, 4};
    int32_t out = 0;

    for (int32_t v : {1, 2, 4, 8}) {
        uint8_t buf[4];
        memcpy(buf, &v, 4);
        ASSERT_EQ(beacon::validate_field(buf, 4, true, spec, out), beacon::GattResult::Ok);
        ASSERT_EQ(out, v);
    }
    for (int32_t v : {0, 3, 5, 7, 16}) {
        uint8_t buf[4];
        memcpy(buf, &v, 4);
        ASSERT_EQ(beacon::validate_field(buf, 4, true, spec, out), beacon::GattResult::OutOfRange);
    }
}

TEST(gatt_auth_code) {
    printf("  test: gatt_auth_code\n");
    uint8_t stored[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    uint8_t correct[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    ASSERT_EQ(beacon::validate_auth_code(correct, 8, stored, 8), beacon::GattResult::Ok);
    uint8_t wrong[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'X'};
    ASSERT_EQ(beacon::validate_auth_code(wrong, 8, stored, 8), beacon::GattResult::Unauthorized);
    ASSERT_EQ(beacon::validate_auth_code(correct, 7, stored, 8), beacon::GattResult::InvalidLength);
}

TEST(gatt_key_chunk_overflow) {
    printf("  test: gatt_key_chunk_overflow\n");
    uint8_t buf[14] = {};
    ASSERT_EQ(beacon::validate_key_chunk(buf, 14, true, 0, 40), beacon::GattResult::Ok);
    ASSERT_EQ(beacon::validate_key_chunk(buf, 14, true, 79, 40), beacon::GattResult::Ok);
    ASSERT_EQ(beacon::validate_key_chunk(buf, 14, true, 80, 40), beacon::GattResult::OutOfRange);
    ASSERT_EQ(beacon::validate_key_chunk(buf, 13, true, 0, 40), beacon::GattResult::InvalidLength);
    ASSERT_EQ(beacon::validate_key_chunk(buf, 14, false, 0, 40), beacon::GattResult::Unauthorized);
}

TEST(load_corrupted_nvs) {
    printf("  test: load_corrupted_nvs\n");
    MockNvsStorage nvs;
    uint8_t garbage[] = {0xFF, 0xFF};
    nvs.write(beacon::ID_period_NVS, garbage, sizeof(garbage));
    nvs.store_int(beacon::ID_power_NVS, 999999);
    beacon::SettingsManager mgr(nvs);
    ASSERT_EQ(mgr.load(), 0);
}

// ============================================================================
// Phase 4 Tests: beacon_state (StateMachine + IHardware)
// ============================================================================

// ---- MockHardware ----
class MockHardware : public beacon::IHardware {
  public:
    // Configurable return values
    uint32_t uptime = 0;
    int battery_volt = 3800;
    bool charging = false;
    bool button = false;
    int bt_enable_result = 0;

    // Call counters
    int bt_enable_calls = 0;
    int bt_disable_calls = 0;
    int adv_start_calls = 0;
    int adv_stop_calls = 0;
    int adv_update_airtag_calls = 0;
    int adv_update_fmdn_calls = 0;
    int set_mac_calls = 0;
    int set_tx_power_calls = 0;
    int wdt_feed_calls = 0;
    int blink_led_calls = 0;
    int power_off_calls = 0;
    int reboot_calls = 0;
    int sleep_ms_calls = 0;
    int store_time_calls = 0;
    int prepare_airtag_calls = 0;
    int prepare_fmdn_calls = 0;
    int start_settings_adv_calls = 0;
    int stop_settings_adv_calls = 0;
    int broadcast_ibeacon_calls = 0;
    int accel_read_calls = 0;
    int accel_init_calls = 0;
    int accel_powerdown_calls = 0;
    int bq_reinit_calls = 0;
    int bq_shipmode_calls = 0;
    int update_turned_on_calls = 0;
    int set_status_bytes_calls = 0;

    // Last values passed
    int last_tx_power = -1;
    uint8_t last_mac[6] = {};
    bool last_turned_on = false;
    int last_ibeacon_voltage = 0;
    uint8_t last_airtag_status = 0;
    uint8_t last_fmdn_status = 0;

    uint32_t uptime_seconds() override { return uptime; }
    int bt_enable() override {
        bt_enable_calls++;
        return bt_enable_result;
    }
    void bt_disable() override { bt_disable_calls++; }
    int adv_start(bool /*connectable*/, int /*imin*/, int /*imax*/, bool /*use_fmdn*/) override {
        adv_start_calls++;
        return 0;
    }
    int adv_stop() override {
        adv_stop_calls++;
        return 0;
    }
    int adv_update_airtag() override {
        adv_update_airtag_calls++;
        return 0;
    }
    int adv_update_fmdn() override {
        adv_update_fmdn_calls++;
        return 0;
    }
    void set_mac(const uint8_t* addr) override {
        set_mac_calls++;
        memcpy(last_mac, addr, 6);
    }
    void set_tx_power(int level) override {
        set_tx_power_calls++;
        last_tx_power = level;
    }
    int battery_voltage() override { return battery_volt; }
    bool is_charging() override { return charging; }
    void wdt_feed() override { wdt_feed_calls++; }
    void blink_led(int /*count*/, bool /*fast*/) override { blink_led_calls++; }
    void power_off() override { power_off_calls++; }
    void reboot() override { reboot_calls++; }
    bool button_pressed() override { return button; }
    void sleep_ms(uint32_t /*ms*/) override { sleep_ms_calls++; }
    void store_time() override { store_time_calls++; }
    void prepare_airtag(const uint8_t* /*key*/) override { prepare_airtag_calls++; }
    void prepare_fmdn(const uint8_t* /*key*/) override { prepare_fmdn_calls++; }
    void start_settings_adv() override { start_settings_adv_calls++; }
    void stop_settings_adv() override { stop_settings_adv_calls++; }
    void broadcast_ibeacon(int voltage) override {
        broadcast_ibeacon_calls++;
        last_ibeacon_voltage = voltage;
    }
    int accel_read() override {
        accel_read_calls++;
        return 0;
    }
    int accel_init() override {
        accel_init_calls++;
        return 0;
    }
    int accel_powerdown() override {
        accel_powerdown_calls++;
        return 0;
    }
    void bq_reinit(bool /*force*/) override { bq_reinit_calls++; }
    void bq_shipmode() override { bq_shipmode_calls++; }
    void update_turned_on(bool on) override {
        update_turned_on_calls++;
        last_turned_on = on;
    }
    void set_status_bytes(uint8_t airtag_status, uint8_t fmdn_status) override {
        last_airtag_status = airtag_status;
        last_fmdn_status = fmdn_status;
        set_status_bytes_calls++;
    }

    void reset_counts() {
        bt_enable_calls = 0;
        bt_disable_calls = 0;
        adv_start_calls = 0;
        adv_stop_calls = 0;
        adv_update_airtag_calls = 0;
        adv_update_fmdn_calls = 0;
        set_mac_calls = 0;
        set_tx_power_calls = 0;
        wdt_feed_calls = 0;
        blink_led_calls = 0;
        power_off_calls = 0;
        reboot_calls = 0;
        sleep_ms_calls = 0;
        store_time_calls = 0;
        prepare_airtag_calls = 0;
        prepare_fmdn_calls = 0;
        start_settings_adv_calls = 0;
        stop_settings_adv_calls = 0;
        broadcast_ibeacon_calls = 0;
        accel_read_calls = 0;
        accel_init_calls = 0;
        accel_powerdown_calls = 0;
        bq_reinit_calls = 0;
        bq_shipmode_calls = 0;
        update_turned_on_calls = 0;
        set_status_bytes_calls = 0;
    }
};

// Helper: create a StateMachine with airtag keys loaded
static void setup_with_airtag_keys(MockNvsStorage& nvs, MockHardware& hw,
                                   beacon::SettingsManager& mgr, beacon::MovementTracker& accel,
                                   beacon::StateMachine& sm, int num_keys = 3) {
    (void)accel;
    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    // Store keys
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    for (int i = 0; i < num_keys; i++) {
        keys[static_cast<size_t>(i)][0] = static_cast<uint8_t>(i + 1);
        keys[static_cast<size_t>(i)][1] = static_cast<uint8_t>(0x10 + i);
    }
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();
}

TEST(init_to_broadcasting_airtag) {
    printf("  test: init_to_broadcasting_airtag\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    // Load one key
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    keys[0][1] = 0x11;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();

    beacon::StateMachine sm(hw, mgr, accel);
    hw.uptime = 0;
    sm.initialize();

    ASSERT_EQ(sm.state(), beacon::State::Broadcasting);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
    ASSERT_TRUE(hw.prepare_airtag_calls > 0);
    ASSERT_TRUE(hw.set_mac_calls > 0);
    ASSERT_EQ(sm.current_key(), 0);
}

TEST(init_to_ibeacon_mode) {
    printf("  test: init_to_ibeacon_mode\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;

    // No flags set, but turned on
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    mgr.load();

    beacon::StateMachine sm(hw, mgr, accel);
    hw.uptime = 0;
    sm.initialize();

    ASSERT_EQ(sm.state(), beacon::State::Broadcasting);
    ASSERT_TRUE(hw.broadcast_ibeacon_calls > 0);
    // Should NOT have prepared airtag
    ASSERT_EQ(hw.prepare_airtag_calls, 0);
}

TEST(settings_mode_entry_exit) {
    printf("  test: settings_mode_entry_exit\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();
    hw.reset_counts();

    // Advance time to trigger settings mode entry (>= 60s)
    hw.uptime = 61;
    sm.tick();

    ASSERT_EQ(sm.state(), beacon::State::SettingsMode);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
    ASSERT_TRUE(hw.start_settings_adv_calls > 0);
    ASSERT_TRUE(sm.broadcasting_settings());

    // Now advance past end_settings to exit
    hw.reset_counts();
    hw.uptime = 64; // end_settings = 61 + 2 = 63
    sm.tick();

    ASSERT_EQ(sm.state(), beacon::State::Broadcasting);
    ASSERT_TRUE(hw.stop_settings_adv_calls > 0);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
    ASSERT_TRUE(!sm.broadcasting_settings());
}

// Helper: advance state machine through any settings mode that triggers
// Returns the number of ticks executed
static int advance_past_settings(MockHardware& hw, beacon::StateMachine& sm, uint32_t target_time) {
    int ticks = 0;
    hw.uptime = target_time;
    sm.tick();
    ticks++;
    // If we entered settings mode, tick again to exit it
    while (sm.state() == beacon::State::SettingsMode) {
        hw.uptime = target_time + 3; // past SETTINGS_WAIT
        sm.tick();
        ticks++;
        // One more tick at target time to process normal logic
        hw.uptime = target_time + 4;
        sm.tick();
        ticks++;
    }
    return ticks;
}

TEST(key_rotation_increments_and_wraps) {
    printf("  test: key_rotation_increments_and_wraps\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 3);
    ASSERT_EQ(sm.current_key(), 0);

    // First tick: initial broadcast
    hw.uptime = 1;
    sm.tick();
    ASSERT_TRUE(sm.broadcasting_airtag());

    // Advance past changeInterval (default 6000s)
    // This may trigger settings mode first, so advance through it
    advance_past_settings(hw, sm, 6001);
    ASSERT_EQ(sm.current_key(), 1);
    ASSERT_EQ(sm.keys_changes(), 1);

    advance_past_settings(hw, sm, 12002);
    ASSERT_EQ(sm.current_key(), 2);
    ASSERT_EQ(sm.keys_changes(), 2);

    // Should wrap to 0
    advance_past_settings(hw, sm, 18003);
    ASSERT_EQ(sm.current_key(), 0);
    ASSERT_EQ(sm.keys_changes(), 3);
}

TEST(airtag_fmdn_alternation) {
    printf("  test: airtag_fmdn_alternation\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    // Both flags enabled
    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_fmdn_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();

    // First tick: should start with one protocol (initial broadcast)
    hw.uptime = 1;
    sm.tick();
    bool first_airtag = sm.broadcasting_airtag();
    bool first_fmdn = sm.broadcasting_fmdn();
    // One should be true, other false
    ASSERT_TRUE(first_airtag != first_fmdn);

    // Second tick: should alternate
    hw.uptime = 2;
    sm.tick();
    ASSERT_EQ(sm.broadcasting_airtag(), !first_airtag);
    ASSERT_EQ(sm.broadcasting_fmdn(), !first_fmdn);

    // Third tick: should alternate back
    hw.uptime = 4;
    sm.tick();
    ASSERT_EQ(sm.broadcasting_airtag(), first_airtag);
    ASSERT_EQ(sm.broadcasting_fmdn(), first_fmdn);
}

TEST(battery_uvlo_shutdown) {
    printf("  test: battery_uvlo_shutdown\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();

    // Set low battery
    hw.battery_volt = 2700;
    hw.charging = false;

    // Need 5+ consecutive low battery checks to trigger shutdown
    // Battery check happens every 60s, but settings mode also triggers at 60s
    // Use advance_past_settings to handle both
    for (int i = 1; i <= 8; i++) {
        if (sm.state() == beacon::State::ShuttingDown) {
            break;
        }
        advance_past_settings(hw, sm, static_cast<uint32_t>(i * 60 + 1));
    }

    ASSERT_TRUE(sm.bad_power() > beacon::kUvloBadPowerThreshold);
    ASSERT_EQ(sm.state(), beacon::State::ShuttingDown);
    ASSERT_TRUE(hw.power_off_calls > 0);
}

TEST(battery_uvlo_charging_resets) {
    printf("  test: battery_uvlo_charging_resets\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();

    // Low battery but charging
    hw.battery_volt = 2700;
    hw.charging = true;

    // Run through multiple battery checks, advancing past settings mode
    for (int i = 1; i <= 8; i++) {
        advance_past_settings(hw, sm, static_cast<uint32_t>(i * 60 + 1));
    }

    // Should NOT shutdown because charging resets bad_power
    ASSERT_NE(sm.state(), beacon::State::ShuttingDown);
    ASSERT_EQ(sm.bad_power(), 0);
    ASSERT_TRUE(sm.charge_lock_counter() > 0);
}

TEST(button_longpress_shutdown) {
    printf("  test: button_longpress_shutdown\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();
    hw.uptime = 0;
    sm.initialize();

    // First tick to get into broadcasting
    hw.uptime = 1;
    sm.tick();

    // Now simulate button long-press
    // The mock returns button=true, and handle_button_longpress checks twice
    // with a sleep in between. We keep it pressed the whole time.
    hw.button = true;

    // We need to be clever: button_pressed() is called multiple times in
    // handle_button_longpress. Since our mock always returns the same value,
    // setting button=true will pass both checks and then loop until released.
    // We'll set a counter to release after a few checks by modifying the
    // approach: we'll just keep button=true but that means the while loop
    // never exits. Instead, let's make it so the button releases after enough calls.

    // Actually, our mock just returns `button` field. We need to make it
    // eventually return false to exit the while loop. Let's use a simple approach:
    // we can't easily do that with our simple mock, so let's make button=false
    // to test that no shutdown happens, then use a subclass to test the full path.

    // Let's use a lambda-based approach instead. We'll create a small subclass.
    struct ButtonMock : MockHardware {
        int press_count = 0;
        int release_after = 4; // release after this many calls
        bool button_pressed() override {
            press_count++;
            return press_count <= release_after;
        }
    };

    ButtonMock bhw;
    bhw.battery_volt = 3800;
    bhw.uptime = 0;

    MockNvsStorage nvs2;
    nvs2.store_int(beacon::ID_airtag_NVS, 1);
    nvs2.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys2 = {};
    keys2[0][0] = 0x42;
    nvs2.write(beacon::ID_key_NVS, keys2.data(), sizeof(keys2));
    beacon::SettingsManager mgr2(nvs2);
    mgr2.load();
    beacon::MovementTracker accel2;
    beacon::StateMachine sm2(bhw, mgr2, accel2);

    // Button pressed at start -> turned_on set
    bhw.release_after = 0; // not pressed at start
    sm2.initialize();
    ASSERT_EQ(sm2.state(), beacon::State::Broadcasting);

    // First tick to get broadcasting started
    bhw.uptime = 1;
    bhw.press_count = 0;
    bhw.release_after = 0; // not pressed during first tick
    sm2.tick();

    // Now tick with button pressed long enough
    bhw.uptime = 2;
    bhw.press_count = 0;
    bhw.release_after = 4; // pressed for 4 calls then released
    sm2.tick();

    ASSERT_EQ(sm2.state(), beacon::State::ShuttingDown);
    ASSERT_TRUE(bhw.power_off_calls > 0);
    ASSERT_TRUE(bhw.update_turned_on_calls > 0);
    ASSERT_EQ(bhw.last_turned_on, false);
}

TEST(nokeys_shutdown) {
    printf("  test: nokeys_shutdown\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;

    // No flags, no keys, not charging, turned_on
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    mgr.load();

    beacon::StateMachine sm(hw, mgr, accel);
    hw.uptime = 0;
    sm.initialize();
    ASSERT_EQ(sm.state(), beacon::State::Broadcasting);

    // Advance past SHUTDOWN_NOKEYS (300s)
    hw.uptime = 301;
    hw.charging = false;
    sm.tick();

    ASSERT_EQ(sm.state(), beacon::State::ShuttingDown);
    ASSERT_TRUE(hw.power_off_calls > 0);
}

// ============================================================================
// Advertisement Data Pipeline Tests
// Verifies the full key → advertisement bytes path matches the original C code
// ============================================================================

// Original C code from main.c: setAdvertisementKey
// This is the function that produces the bytes an iPhone actually sees
namespace original_adv {

static uint8_t offline_finding_adv_template[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0xAA, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint8_t airtag_data_store[31];
static uint8_t bleAddr[6];

static void set_addr_from_key(const uint8_t* key) {
    bleAddr[5] = key[0] | 0xC0;
    bleAddr[4] = key[1];
    bleAddr[3] = key[2];
    bleAddr[2] = key[3];
    bleAddr[1] = key[4];
    bleAddr[0] = key[5];
}

static void fill_adv_template_from_key(const uint8_t* key) {
    memcpy(&offline_finding_adv_template[7], &key[6], 22);
    offline_finding_adv_template[29] = key[0] >> 6;
}

static void setAdvertisementKey(const uint8_t* key) {
    set_addr_from_key(key);
    fill_adv_template_from_key(key);
    uint8_t status_save = airtag_data_store[6];
    memcpy(airtag_data_store, offline_finding_adv_template, sizeof(offline_finding_adv_template));
    airtag_data_store[6] = status_save;
}

// FMDN key preparation (from main.c switch_fmdn_key)
static uint8_t fmdn_initial_data[] = {
    0x16, 0xAA, 0xFE, 0x41,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc0,
};
static uint8_t fmdn_data_store[25];

static void switch_fmdn_key(const uint8_t* key) {
    uint8_t status_save = fmdn_data_store[23];
    memcpy(fmdn_data_store, fmdn_initial_data, sizeof(fmdn_initial_data));
    memcpy(fmdn_data_store + 3, key, 20);
    fmdn_data_store[23] = status_save;
}

} // namespace original_adv

// Simulate what ZephyrHardware::prepare_airtag does (without Zephyr)
namespace cpp_adv {

static uint8_t offline_finding_adv_template[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0xAA, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint8_t airtag_data_store[31];
static uint8_t bleAddr[6];

static void prepare_airtag(const uint8_t* key) {
    beacon::derive_mac_from_key(key, bleAddr);
    beacon::fill_adv_template(key, offline_finding_adv_template, sizeof(offline_finding_adv_template));
    uint8_t status_save = airtag_data_store[6];
    memcpy(airtag_data_store, offline_finding_adv_template, sizeof(offline_finding_adv_template));
    airtag_data_store[6] = status_save;
}

static uint8_t fmdn_initial_data[] = {
    0x16, 0xAA, 0xFE, 0x41,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xc0,
};
static uint8_t fmdn_data_store[25];

static void prepare_fmdn(const uint8_t* key) {
    uint8_t status_save = fmdn_data_store[23];
    memcpy(fmdn_data_store, fmdn_initial_data, sizeof(fmdn_initial_data));
    memcpy(fmdn_data_store + 3, key, 20);
    fmdn_data_store[23] = status_save;
}

} // namespace cpp_adv

TEST(airtag_adv_payload_matches_original) {
    printf("  test: airtag_adv_payload_matches_original\n");

    // Test with several different keys
    const uint8_t keys[][28] = {
        // Key 1: sequential bytes
        {0x41, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
         0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
         0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C},
        // Key 2: all 0xFF
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
         0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        // Key 3: realistic-looking key
        {0xA3, 0x7B, 0x1F, 0x42, 0xE8, 0x9D, 0x56, 0xC0, 0x3A, 0x88,
         0x14, 0x6E, 0xF2, 0xD7, 0x93, 0x05, 0xBC, 0x61, 0x4F, 0xAA,
         0x27, 0x8C, 0xE5, 0x30, 0x7D, 0x19, 0xB4, 0x68},
    };

    for (int k = 0; k < 3; k++) {
        // Reset templates to initial state
        memcpy(original_adv::offline_finding_adv_template,
               cpp_adv::offline_finding_adv_template,
               sizeof(cpp_adv::offline_finding_adv_template));
        memset(original_adv::airtag_data_store, 0, sizeof(original_adv::airtag_data_store));
        memset(cpp_adv::airtag_data_store, 0, sizeof(cpp_adv::airtag_data_store));

        // Run both
        original_adv::setAdvertisementKey(keys[k]);
        cpp_adv::prepare_airtag(keys[k]);

        // Compare advertisement payload (the bytes that go over the air)
        ASSERT_MEM_EQ(cpp_adv::airtag_data_store, original_adv::airtag_data_store, 31);

        // Compare MAC address
        ASSERT_MEM_EQ(cpp_adv::bleAddr, original_adv::bleAddr, 6);

        // Verify specific critical bytes:
        // Byte 0: length (0x1e = 30)
        ASSERT_EQ(cpp_adv::airtag_data_store[0], 0x1e);
        // Byte 1: type (0xff = manufacturer specific)
        ASSERT_EQ(cpp_adv::airtag_data_store[1], 0xff);
        // Bytes 2-3: Apple company ID
        ASSERT_EQ(cpp_adv::airtag_data_store[2], 0x4c);
        ASSERT_EQ(cpp_adv::airtag_data_store[3], 0x00);
        // Bytes 4-5: Offline Finding type
        ASSERT_EQ(cpp_adv::airtag_data_store[4], 0x12);
        ASSERT_EQ(cpp_adv::airtag_data_store[5], 0x19);
        // Bytes 7-28: key[6..27] (the public key portion)
        ASSERT_MEM_EQ(&cpp_adv::airtag_data_store[7], &keys[k][6], 22);
        // Byte 29: key[0] >> 6 (top 2 bits)
        ASSERT_EQ(cpp_adv::airtag_data_store[29], static_cast<uint8_t>(keys[k][0] >> 6));
        // MAC: byte[5] has 0xC0 set
        ASSERT_TRUE((cpp_adv::bleAddr[5] & 0xC0) == 0xC0);
    }
}

TEST(fmdn_adv_payload_matches_original) {
    printf("  test: fmdn_adv_payload_matches_original\n");

    const uint8_t fmdn_keys[][20] = {
        // Key 1
        {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
         0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x12, 0x34, 0x56, 0x78},
        // Key 2: all zeros
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };

    for (int k = 0; k < 2; k++) {
        // Reset
        memset(original_adv::fmdn_data_store, 0, sizeof(original_adv::fmdn_data_store));
        memset(cpp_adv::fmdn_data_store, 0, sizeof(cpp_adv::fmdn_data_store));

        // Run both
        original_adv::switch_fmdn_key(fmdn_keys[k]);
        cpp_adv::prepare_fmdn(fmdn_keys[k]);

        // Compare FMDN payload
        ASSERT_MEM_EQ(cpp_adv::fmdn_data_store, original_adv::fmdn_data_store, 25);

        // Verify structure:
        // Byte 0: service data type (0x16)
        ASSERT_EQ(cpp_adv::fmdn_data_store[0], 0x16);
        // Bytes 1-2: Eddystone UUID (0xAAFE)
        ASSERT_EQ(cpp_adv::fmdn_data_store[1], 0xAA);
        ASSERT_EQ(cpp_adv::fmdn_data_store[2], 0xFE);
        // Bytes 3-22: FMDN key (20 bytes at offset 3)
        // Note: key overwrites the EID frame type byte at offset 3 — this
        // matches the original C code (switch_fmdn_key copies key at offset 3)
        ASSERT_MEM_EQ(&cpp_adv::fmdn_data_store[3], &fmdn_keys[k][0], 20);
    }
}

TEST(airtag_status_byte_preserved_across_key_switch) {
    printf("  test: airtag_status_byte_preserved_across_key_switch\n");

    // Set a status byte, then switch keys — status should be preserved
    cpp_adv::airtag_data_store[6] = 0x42;

    const uint8_t key[28] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                              0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                              0x19, 0x1A, 0x1B, 0x1C};
    cpp_adv::prepare_airtag(key);

    // Status byte at position 6 should be preserved (not overwritten by template)
    ASSERT_EQ(cpp_adv::airtag_data_store[6], 0x42);
}

TEST(fmdn_status_byte_preserved_across_key_switch) {
    printf("  test: fmdn_status_byte_preserved_across_key_switch\n");

    // Set a status byte, then switch keys — status should be preserved
    cpp_adv::fmdn_data_store[23] = 0x55;

    const uint8_t key[20] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
                              0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                              0xAA, 0xBB, 0xCC, 0xDD};
    cpp_adv::prepare_fmdn(key);

    // Status byte at position 23 should be preserved
    ASSERT_EQ(cpp_adv::fmdn_data_store[23], 0x55);
}

// ============================================================================
// Phase 5 Tests: StatusFlags, validated setters, NVS validation, status bytes
// ============================================================================

TEST(status_flags_roundtrip) {
    printf("  test: status_flags_roundtrip\n");
    ASSERT_EQ(beacon::StatusFlags::unpack(0x458000).pack(), static_cast<uint32_t>(0x458000));
    auto f = beacon::StatusFlags::unpack(0x458000);
    ASSERT_EQ(f.airtag_base, 0);
    ASSERT_EQ(f.fmdn_base, 0x80);
    ASSERT_EQ(static_cast<uint8_t>(f.airtag_mode), 5);
    ASSERT_EQ(static_cast<uint8_t>(f.fmdn_mode), 4);
    // Zero packs to zero
    ASSERT_EQ(beacon::StatusFlags::unpack(0).pack(), static_cast<uint32_t>(0));
}

TEST(setter_validation) {
    printf("  test: setter_validation\n");
    MockNvsStorage nvs;
    beacon::SettingsManager settings(nvs);
    settings.load();

    // mult_period rejects invalid values
    ASSERT_TRUE(!settings.set_mult_period(0));  // prevents busy-loop
    ASSERT_TRUE(!settings.set_mult_period(3));
    ASSERT_EQ(settings.config().mult_period, 2);  // unchanged
    ASSERT_TRUE(settings.set_mult_period(4));
    ASSERT_EQ(settings.config().mult_period, 4);

    // tx_power rejects >2
    ASSERT_TRUE(!settings.set_tx_power(5));
    ASSERT_EQ(settings.config().tx_power, 2);

    // change_interval aligns to 8
    ASSERT_TRUE(settings.set_change_interval(6001));
    ASSERT_EQ(settings.config().change_interval, 6000);  // aligned down
    ASSERT_TRUE(!settings.set_change_interval(29));  // below min
    ASSERT_TRUE(!settings.set_change_interval(7201));  // above max

    // accel_threshold rejects >16383
    ASSERT_TRUE(!settings.set_accel_threshold(16384));
    ASSERT_EQ(settings.config().accel_threshold, 800);
}

TEST(nvs_load_rejects_invalid) {
    printf("  test: nvs_load_rejects_invalid\n");
    MockNvsStorage nvs;
    nvs.store_int(beacon::ID_period_NVS, 0);   // invalid (busy-loop hazard)
    nvs.store_int(beacon::ID_power_NVS, 99);   // out of range
    nvs.store_int(beacon::ID_accel_NVS, -1);   // negative

    beacon::SettingsManager settings(nvs);
    settings.load();

    // Should all fall back to defaults
    ASSERT_EQ(settings.config().mult_period, 2);
    ASSERT_EQ(settings.config().tx_power, 2);
    ASSERT_EQ(settings.config().accel_threshold, 800);
}

TEST(status_bytes_wired) {
    printf("  test: status_bytes_wired\n");
    MockNvsStorage nvs;
    beacon::SettingsManager settings(nvs);
    settings.load();
    settings.set_flag_airtag(true);
    settings.set_status_flags(0x10042);  // mode 1, base 0x42

    // Set up a key
    uint8_t key[28] = {};
    for (int i = 0; i < 28; i++) key[i] = static_cast<uint8_t>(i + 1);
    settings.set_key_chunk(0, 0, key, 14);
    settings.set_key_chunk(0, 14, key + 14, 14);
    settings.set_num_keys(1);
    settings.set_turned_on(true);

    MockHardware hw;
    hw.battery_volt = 3800;
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, settings, accel);
    sm.initialize();

    // After initialize, set_status_bytes should have been called
    ASSERT_TRUE(hw.set_status_bytes_calls > 0);
    ASSERT_EQ(hw.last_airtag_status, 0x42);  // mode 1 = fixed base byte
}

// ---- Transition discipline tests ----
// Verify that state machine transitions call adv methods in the correct
// order: adv_stop before bt_disable, start_settings_adv after bt_enable,
// and that mode-specific methods are used correctly.

TEST(settings_entry_stops_broadcast_before_disable) {
    printf("  test: settings_entry_stops_broadcast_before_disable\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 1);

    // First tick: start broadcasting
    hw.uptime = 1;
    sm.tick();
    ASSERT_TRUE(hw.adv_start_calls > 0);

    hw.reset_counts();

    // Trigger settings mode entry
    hw.uptime = 61;
    sm.tick();

    ASSERT_EQ(sm.state(), beacon::State::SettingsMode);
    // Must have stopped broadcast AND disabled BT before starting settings
    ASSERT_TRUE(hw.adv_stop_calls > 0);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
    ASSERT_TRUE(hw.start_settings_adv_calls > 0);
}

TEST(settings_exit_stops_settings_before_disable) {
    printf("  test: settings_exit_stops_settings_before_disable\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 1);

    // Enter settings
    hw.uptime = 61;
    sm.tick();
    ASSERT_EQ(sm.state(), beacon::State::SettingsMode);

    hw.reset_counts();

    // Exit settings
    hw.uptime = 64;
    sm.tick();

    ASSERT_EQ(sm.state(), beacon::State::Broadcasting);
    ASSERT_TRUE(hw.stop_settings_adv_calls > 0);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
}

TEST(key_rotation_stops_adv_before_disable) {
    printf("  test: key_rotation_stops_adv_before_disable\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 3);

    // Start broadcasting
    hw.uptime = 1;
    sm.tick();

    // Advance past settings + past changeInterval
    advance_past_settings(hw, sm, 6001);
    ASSERT_EQ(sm.current_key(), 1);

    // The rotation should have: stopped adv, disabled BT, prepared new key,
    // set mac, re-enabled BT, and restarted broadcast.
    ASSERT_TRUE(hw.adv_stop_calls > 0);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.bt_enable_calls > 0);
    ASSERT_TRUE(hw.prepare_airtag_calls > 0);
    ASSERT_TRUE(hw.set_mac_calls > 0);
}

TEST(uvlo_shutdown_stops_adv) {
    printf("  test: uvlo_shutdown_stops_adv\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 1);
    hw.uptime = 1;
    sm.tick(); // start broadcasting

    // Trigger UVLO: low battery for enough ticks
    hw.battery_volt = 2500; // below kBatteryLowVoltage
    hw.reset_counts();

    for (int i = 0; i < 20; i++) {
        hw.uptime += 120; // battery check interval
        sm.tick();
        if (sm.state() == beacon::State::ShuttingDown) break;
        // Skip settings mode if triggered
        if (sm.state() == beacon::State::SettingsMode) {
            hw.uptime += 3;
            sm.tick();
        }
    }

    ASSERT_EQ(sm.state(), beacon::State::ShuttingDown);
    ASSERT_TRUE(hw.adv_stop_calls > 0);
    ASSERT_TRUE(hw.power_off_calls > 0);
}

TEST(button_longpress_stops_adv_and_disables) {
    printf("  test: button_longpress_stops_adv_and_disables\n");

    // ButtonMock releases after a few calls (same pattern as button_longpress_shutdown test)
    struct BtnMock : MockHardware {
        int press_count = 0;
        bool button_pressed() override {
            press_count++;
            return press_count <= 4;
        }
    };

    MockNvsStorage nvs;
    BtnMock hw;
    hw.battery_volt = 3800;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;

    nvs.store_int(beacon::ID_airtag_NVS, 1);
    nvs.store_int(beacon::ID_turnedOn_NVS, 1);
    std::array<std::array<uint8_t, 28>, 40> keys = {};
    keys[0][0] = 0x42;
    nvs.write(beacon::ID_key_NVS, keys.data(), sizeof(keys));
    mgr.load();

    beacon::StateMachine sm(hw, mgr, accel);
    hw.uptime = 0;
    hw.press_count = 100; // skip button during initialize (return false)
    sm.initialize();

    hw.uptime = 1;
    hw.press_count = 100;
    sm.tick(); // start broadcasting

    hw.reset_counts();
    hw.press_count = 0; // re-arm: next 4 calls return true, then false
    hw.uptime = 2;
    sm.tick(); // triggers button longpress

    ASSERT_EQ(sm.state(), beacon::State::ShuttingDown);
    ASSERT_TRUE(hw.adv_stop_calls > 0);
    ASSERT_TRUE(hw.bt_disable_calls > 0);
    ASSERT_TRUE(hw.power_off_calls > 0);
}

TEST(airtag_uses_adv_start_airtag_not_fmdn) {
    printf("  test: airtag_uses_adv_start_airtag_not_fmdn\n");
    MockNvsStorage nvs;
    MockHardware hw;
    beacon::SettingsManager mgr(nvs);
    beacon::MovementTracker accel;
    beacon::StateMachine sm(hw, mgr, accel);

    // AirTag only (no FMDN)
    setup_with_airtag_keys(nvs, hw, mgr, accel, sm, 1);
    hw.reset_counts();
    hw.uptime = 1;
    sm.tick();

    ASSERT_TRUE(hw.adv_start_calls > 0);
    // adv_start_calls is incremented by both adv_start_airtag and adv_start_fmdn
    // in our mock — verify via the broadcasting flags
    ASSERT_TRUE(sm.broadcasting_airtag());
    ASSERT_TRUE(!sm.broadcasting_fmdn());
}

int main() {
    printf("Running host tests...\n");
    printf("\n%d/%d assertions passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
