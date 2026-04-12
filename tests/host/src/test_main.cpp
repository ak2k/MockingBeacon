// Host-native test runner for pure C++ modules
// Uses simple assert-based testing (no Zephyr dependency)

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include "beacon_logic.hpp"

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
// Original C functions for equivalence testing
// ============================================================================
namespace original_c {

// From main.c: set_addr_from_key
static void set_addr_from_key(const char *key, uint8_t *bleAddr) {
    bleAddr[5] = static_cast<uint8_t>(key[0] | 0b11000000);
    bleAddr[4] = static_cast<uint8_t>(key[1]);
    bleAddr[3] = static_cast<uint8_t>(key[2]);
    bleAddr[2] = static_cast<uint8_t>(key[3]);
    bleAddr[1] = static_cast<uint8_t>(key[4]);
    bleAddr[0] = static_cast<uint8_t>(key[5]);
}

// From main.c: fill_adv_template_from_key
static void fill_adv_template_from_key(const char *key, uint8_t *tmpl) {
    memcpy(&tmpl[7], &key[6], 22);
    tmpl[29] = static_cast<uint8_t>(key[0] >> 6);
}

// From settings.c: check_empty_key
static int check_empty_key(const unsigned char *key) {
    for (int j = 0; j < 28; j++) {
        if (key[j] != 0) {
            return 0;
        }
    }
    return 1;
}

// Battery level constants matching myboards.h (Li-Ion defaults)
#define BATTERY_LEVEL_FULL   4000
#define BATTERY_LEVEL_NORMAL 3700
#define BATTERY_LEVEL_LOW    3300

// From main.c: update_status_byte (refactored to be pure)
static void update_status_byte(int statusFlags, int lastBatteryVoltage,
                               int keys_changes, int what_in_status,
                               uint8_t accel_byte, int16_t accelTemperature,
                               uint8_t *out_airtag, uint8_t *out_fmdn) {
    uint8_t statusAirtag, statusFmdn;
    int bAirtag, bFmdn;

    statusAirtag = static_cast<uint8_t>(statusFlags & 0xff);
    statusFmdn = static_cast<uint8_t>((statusFlags & 0xff00) >> 8);
    bAirtag = (statusFlags & 0xf0000) >> 16;
    bFmdn = (statusFlags & 0xf00000) >> 20;

    // Setting status for airtag
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
        if (what_in_status == 1) {
            statusAirtag = static_cast<uint8_t>(0x80 | accel_byte);
        }
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
        else {
            if (lastBatteryVoltage > BATTERY_LEVEL_NORMAL)
                battery = 1;
            else {
                if (lastBatteryVoltage > BATTERY_LEVEL_LOW)
                    battery = 2;
                else
                    battery = 3;
            }
        }
        statusAirtag = static_cast<uint8_t>(statusAirtag | (battery << 6));
        break;
    }
    default:
        break;
    }
    *out_airtag = statusAirtag;

    // Setting status for FMDN
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
        else {
            if (lastBatteryVoltage > BATTERY_LEVEL_LOW)
                battery = 2;
            else {
                if (lastBatteryVoltage > 0)
                    battery = 3;
                else
                    battery = 0;
            }
        }
        statusFmdn = static_cast<uint8_t>(statusFmdn | (battery << 5));
        break;
    }
    default:
        break;
    }
    *out_fmdn = statusFmdn;
}

} // namespace original_c

// ============================================================================
// 1. derive_mac_from_key tests
// ============================================================================

TEST(derive_mac_known_key) {
    printf("  test: derive_mac_known_key\n");
    // A known 28-byte key where first 6 bytes are 0x01..0x06
    const uint8_t key[28] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    uint8_t mac[6] = {};

    beacon::derive_mac_from_key(key, mac);

    // Verify reverse order: mac[0]=key[5], mac[1]=key[4], ..., mac[4]=key[1]
    ASSERT_EQ(mac[0], 0x06);
    ASSERT_EQ(mac[1], 0x05);
    ASSERT_EQ(mac[2], 0x04);
    ASSERT_EQ(mac[3], 0x03);
    ASSERT_EQ(mac[4], 0x02);
    // mac[5] = key[0] | 0xC0 = 0x01 | 0xC0 = 0xC1
    ASSERT_EQ(mac[5], 0xC1);
}

TEST(derive_mac_or_c0) {
    printf("  test: derive_mac_or_c0\n");
    // Test that | 0xC0 works correctly: key[0] = 0x3F -> mac[5] = 0xFF
    const uint8_t key[28] = {0x3F, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint8_t mac[6] = {};

    beacon::derive_mac_from_key(key, mac);
    ASSERT_EQ(mac[5], 0xFF);

    // key[0] = 0x00 -> mac[5] = 0xC0
    const uint8_t key2[28] = {0x00};
    beacon::derive_mac_from_key(key2, mac);
    ASSERT_EQ(mac[5], 0xC0);
}

// ============================================================================
// 2. fill_adv_template tests
// ============================================================================

TEST(fill_adv_template_basic) {
    printf("  test: fill_adv_template_basic\n");
    // Create a 28-byte key with known values
    uint8_t key[28];
    for (int i = 0; i < 28; i++) {
        key[i] = static_cast<uint8_t>(i + 0x10);
    }

    uint8_t tmpl[31] = {};
    beacon::fill_adv_template(key, tmpl, sizeof(tmpl));

    // Verify key[6..27] copied into tmpl[7..28]
    ASSERT_MEM_EQ(&tmpl[7], &key[6], 22);

    // Verify tmpl[29] = key[0] >> 6 = 0x10 >> 6 = 0
    ASSERT_EQ(tmpl[29], static_cast<uint8_t>(key[0] >> 6));
}

TEST(fill_adv_template_key0_bits) {
    printf("  test: fill_adv_template_key0_bits\n");
    // key[0] = 0xC0 -> key[0] >> 6 = 3
    uint8_t key[28] = {};
    key[0] = 0xC0;

    uint8_t tmpl[31] = {};
    beacon::fill_adv_template(key, tmpl, sizeof(tmpl));
    ASSERT_EQ(tmpl[29], 3);

    // key[0] = 0x80 -> key[0] >> 6 = 2
    key[0] = 0x80;
    memset(tmpl, 0, sizeof(tmpl));
    beacon::fill_adv_template(key, tmpl, sizeof(tmpl));
    ASSERT_EQ(tmpl[29], 2);
}

TEST(fill_adv_template_too_small) {
    printf("  test: fill_adv_template_too_small\n");
    uint8_t key[28] = {};
    uint8_t tmpl[20] = {};
    // Should not crash with undersized buffer
    beacon::fill_adv_template(key, tmpl, sizeof(tmpl));
    // tmpl should be unchanged (all zeros)
    uint8_t zeros[20] = {};
    ASSERT_MEM_EQ(tmpl, zeros, sizeof(tmpl));
}

// ============================================================================
// 3. is_key_empty tests
// ============================================================================

TEST(is_key_empty_all_zeros) {
    printf("  test: is_key_empty_all_zeros\n");
    uint8_t key[28] = {};
    ASSERT_TRUE(beacon::is_key_empty(key, 28));
}

TEST(is_key_empty_one_nonzero) {
    printf("  test: is_key_empty_one_nonzero\n");
    uint8_t key[28] = {};
    key[14] = 0x01;
    ASSERT_TRUE(!beacon::is_key_empty(key, 28));
}

TEST(is_key_empty_last_byte) {
    printf("  test: is_key_empty_last_byte\n");
    uint8_t key[28] = {};
    key[27] = 0xFF;
    ASSERT_TRUE(!beacon::is_key_empty(key, 28));
}

TEST(is_key_empty_zero_length) {
    printf("  test: is_key_empty_zero_length\n");
    uint8_t key[1] = {0xFF};
    ASSERT_TRUE(beacon::is_key_empty(key, 0));
}

// ============================================================================
// 4. compute_status tests
// ============================================================================

TEST(status_fixed_airtag) {
    printf("  test: status_fixed_airtag (bAirtag=1)\n");
    // statusFlags = 0x10042 -> bAirtag=1, airtag base = 0x42
    beacon::StatusInput in = {};
    in.status_flags = 0x10042;
    in.battery_voltage = 3500;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 0x42);
}

TEST(status_incrementing_airtag) {
    printf("  test: status_incrementing_airtag (bAirtag=2)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x20000;
    in.keys_changes = 0x1A3;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 0xA3);
}

TEST(status_voltage_airtag) {
    printf("  test: status_voltage_airtag (bAirtag=3)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x30000;
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, 32);
}

TEST(status_battery_level_airtag_full) {
    printf("  test: status_battery_level_airtag_full (bAirtag=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x40000;  // base byte = 0x00
    in.battery_voltage = 4100;  // > FULL

    auto out = beacon::compute_status(in);
    // battery=0, status = 0x00 | (0 << 6) = 0x00
    ASSERT_EQ(out.airtag_status, 0x00);
}

TEST(status_battery_level_airtag_normal) {
    printf("  test: status_battery_level_airtag_normal (bAirtag=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x40000;
    in.battery_voltage = 3800;  // > NORMAL, <= FULL

    auto out = beacon::compute_status(in);
    // battery=1, status = 0x00 | (1 << 6) = 0x40
    ASSERT_EQ(out.airtag_status, 0x40);
}

TEST(status_battery_level_airtag_low) {
    printf("  test: status_battery_level_airtag_low (bAirtag=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x40000;
    in.battery_voltage = 3500;  // > LOW, <= NORMAL

    auto out = beacon::compute_status(in);
    // battery=2, status = 0x00 | (2 << 6) = 0x80
    ASSERT_EQ(out.airtag_status, 0x80);
}

TEST(status_battery_level_airtag_critical) {
    printf("  test: status_battery_level_airtag_critical (bAirtag=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x40000;
    in.battery_voltage = 3000;  // <= LOW

    auto out = beacon::compute_status(in);
    // battery=3, status = 0x00 | (3 << 6) = 0xC0
    ASSERT_EQ(out.airtag_status, 0xC0);
}

TEST(status_battery_level_airtag_with_base) {
    printf("  test: status_battery_level_airtag_with_base (bAirtag=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x4000F;  // base = 0x0F
    in.battery_voltage = 3800;  // battery=1

    auto out = beacon::compute_status(in);
    // status = 0x0F | (1 << 6) = 0x4F
    ASSERT_EQ(out.airtag_status, 0x4F);
}

TEST(status_accel_airtag) {
    printf("  test: status_accel_airtag (bAirtag=5, what_in_status=1)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x50000;
    in.what_in_status = 1;
    in.accel_byte = 0x37;
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x80 | 0x37));
}

TEST(status_temp_airtag) {
    printf("  test: status_temp_airtag (bAirtag=5, what_in_status=2)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x50000;
    in.what_in_status = 2;
    in.temperature = 235;  // 23.5 C -> (235 + 5)/10 + 10 = 34
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40 | 34));
}

TEST(status_temp_airtag_clamp_low) {
    printf("  test: status_temp_airtag_clamp_low\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x50000;
    in.what_in_status = 2;
    in.temperature = -200;  // -20 C -> (-200 + 5)/10 + 10 = -9.5 -> -9 (int division)
                            // Wait: (-200+5) = -195, /10 = -19 (truncation), +10 = -9 -> clamp to 0
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40 | 0));
}

TEST(status_temp_airtag_clamp_high) {
    printf("  test: status_temp_airtag_clamp_high\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x50000;
    in.what_in_status = 2;
    in.temperature = 600;  // 60 C -> (600 + 5)/10 + 10 = 70 -> clamp to 63
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.airtag_status, static_cast<uint8_t>(0x40 | 63));
}

TEST(status_voltage_mode5_airtag) {
    printf("  test: status_voltage_mode5_airtag (bAirtag=5, what_in_status=0)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x50000;
    in.what_in_status = 0;
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    // When what_in_status==0, mode 5 behaves like mode 3 (voltage)
    ASSERT_EQ(out.airtag_status, 32);
}

// --- FMDN status tests ---

TEST(status_fixed_fmdn) {
    printf("  test: status_fixed_fmdn (bFmdn=1)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x100000 | (0xAB << 8);  // bFmdn=1, fmdn base = 0xAB

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, 0xAB);
}

TEST(status_incrementing_fmdn) {
    printf("  test: status_incrementing_fmdn (bFmdn=2)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x200000;
    in.keys_changes = 0x55;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, 0x55);
}

TEST(status_voltage_fmdn) {
    printf("  test: status_voltage_fmdn (bFmdn=3)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x300000;
    in.battery_voltage = 3700;
    in.what_in_status = 0;  // voltage mode for case 3

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, 37);
}

TEST(status_battery_level_fmdn_normal) {
    printf("  test: status_battery_level_fmdn_normal (bFmdn=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x400000;
    in.battery_voltage = 3800;  // > NORMAL

    auto out = beacon::compute_status(in);
    // battery=1, status = 0x00 | (1 << 5) = 0x20
    ASSERT_EQ(out.fmdn_status, 0x20);
}

TEST(status_battery_level_fmdn_low) {
    printf("  test: status_battery_level_fmdn_low (bFmdn=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x400000;
    in.battery_voltage = 3500;  // > LOW, <= NORMAL

    auto out = beacon::compute_status(in);
    // battery=2, status = 0x00 | (2 << 5) = 0x40
    ASSERT_EQ(out.fmdn_status, 0x40);
}

TEST(status_battery_level_fmdn_critical) {
    printf("  test: status_battery_level_fmdn_critical (bFmdn=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x400000;
    in.battery_voltage = 3000;  // <= LOW, > 0

    auto out = beacon::compute_status(in);
    // battery=3, status = 0x00 | (3 << 5) = 0x60
    ASSERT_EQ(out.fmdn_status, 0x60);
}

TEST(status_battery_level_fmdn_zero) {
    printf("  test: status_battery_level_fmdn_zero (bFmdn=4)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x400000;
    in.battery_voltage = 0;  // == 0

    auto out = beacon::compute_status(in);
    // battery=0, status = 0x00 | (0 << 5) = 0x00
    ASSERT_EQ(out.fmdn_status, 0x00);
}

TEST(status_accel_fmdn) {
    printf("  test: status_accel_fmdn (bFmdn=5, what_in_status=1)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x500000;
    in.what_in_status = 1;
    in.accel_byte = 0x7F;
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, static_cast<uint8_t>(0x80 | 0x7F));
}

TEST(status_temp_fmdn) {
    printf("  test: status_temp_fmdn (bFmdn=5, what_in_status=2)\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x500000;
    in.what_in_status = 2;
    in.temperature = 200;  // 20 C -> (200 + 5)/10 + 10 = 30
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, static_cast<uint8_t>(0x40 | 30));
}

// FMDN case 3 (not case 5) always uses voltage regardless of what_in_status
TEST(status_voltage_fmdn_case3_ignores_what) {
    printf("  test: status_voltage_fmdn_case3_ignores_what\n");
    beacon::StatusInput in = {};
    in.status_flags = 0x300000;
    in.what_in_status = 2;  // would be temp, but case 3 breaks early
    in.battery_voltage = 3200;

    auto out = beacon::compute_status(in);
    ASSERT_EQ(out.fmdn_status, 32);
}

// ============================================================================
// 5. C-vs-C++ equivalence tests
// ============================================================================

TEST(equiv_derive_mac) {
    printf("  test: equiv_derive_mac\n");
    // Test with several key patterns
    const uint8_t keys[][28] = {
        {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56},
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        {0x3F, 0x80, 0x01, 0xFE, 0x77, 0x42},
    };

    for (const auto &key : keys) {
        uint8_t mac_cpp[6] = {};
        uint8_t mac_c[6] = {};

        beacon::derive_mac_from_key(key, mac_cpp);
        original_c::set_addr_from_key(reinterpret_cast<const char *>(key), mac_c);

        ASSERT_MEM_EQ(mac_cpp, mac_c, 6);
    }
}

TEST(equiv_fill_adv_template) {
    printf("  test: equiv_fill_adv_template\n");
    uint8_t key[28];
    for (int i = 0; i < 28; i++) {
        key[i] = static_cast<uint8_t>(i * 7 + 3);
    }

    uint8_t tmpl_cpp[31] = {};
    uint8_t tmpl_c[31] = {};

    beacon::fill_adv_template(key, tmpl_cpp, sizeof(tmpl_cpp));
    original_c::fill_adv_template_from_key(reinterpret_cast<const char *>(key), tmpl_c);

    // Compare bytes 7..29 (the ones that get written)
    ASSERT_MEM_EQ(&tmpl_cpp[7], &tmpl_c[7], 23);
}

TEST(equiv_is_key_empty) {
    printf("  test: equiv_is_key_empty\n");
    uint8_t key_zero[28] = {};
    ASSERT_EQ(beacon::is_key_empty(key_zero, 28), static_cast<bool>(original_c::check_empty_key(key_zero)));

    uint8_t key_nonzero[28] = {};
    key_nonzero[13] = 0x42;
    ASSERT_EQ(beacon::is_key_empty(key_nonzero, 28), static_cast<bool>(original_c::check_empty_key(key_nonzero)));
}

TEST(equiv_compute_status) {
    printf("  test: equiv_compute_status\n");
    // Test a variety of statusFlags + battery + keys_changes combinations
    struct TestCase {
        int status_flags;
        int battery_voltage;
        int keys_changes;
        int what_in_status;
        uint8_t accel_byte;
        int16_t temperature;
    };

    const TestCase cases[] = {
        // Fixed mode airtag
        {0x10042, 3500, 0, 0, 0, 0},
        // Incrementing mode airtag
        {0x20000, 3500, 0x1A3, 0, 0, 0},
        // Voltage mode airtag
        {0x30000, 3200, 0, 0, 0, 0},
        // Battery level airtag, various voltages
        {0x40000, 4100, 0, 0, 0, 0},
        {0x40000, 3800, 0, 0, 0, 0},
        {0x40000, 3500, 0, 0, 0, 0},
        {0x40000, 3000, 0, 0, 0, 0},
        // Rotating mode airtag (voltage)
        {0x50000, 3200, 0, 0, 0, 0},
        // Rotating mode airtag (accel)
        {0x50000, 3200, 0, 1, 0x37, 0},
        // Rotating mode airtag (temp)
        {0x50000, 3200, 0, 2, 0, 235},
        // Fixed mode FMDN
        {0x10AB00, 3500, 0, 0, 0, 0},
        // Incrementing mode FMDN
        {0x200000, 3500, 0x55, 0, 0, 0},
        // Voltage mode FMDN
        {0x300000, 3700, 0, 0, 0, 0},
        // Battery level FMDN
        {0x400000, 3800, 0, 0, 0, 0},
        {0x400000, 3500, 0, 0, 0, 0},
        {0x400000, 3000, 0, 0, 0, 0},
        {0x400000, 0, 0, 0, 0, 0},
        // Combined airtag + FMDN modes
        {0x350042, 3200, 5, 0, 0x1F, 200},
        {0x550042, 3200, 5, 1, 0x1F, 200},
        {0x550042, 3200, 5, 2, 0x1F, 200},
        // Temperature clamping
        {0x50000, 3200, 0, 2, 0, -200},
        {0x50000, 3200, 0, 2, 0, 600},
        // The default statusFlags from settings.c
        {0x458000, 3200, 0, 0, 0, 0},
    };

    for (const auto &tc : cases) {
        beacon::StatusInput in = {};
        in.status_flags = tc.status_flags;
        in.battery_voltage = tc.battery_voltage;
        in.keys_changes = tc.keys_changes;
        in.what_in_status = tc.what_in_status;
        in.accel_byte = tc.accel_byte;
        in.temperature = tc.temperature;

        auto cpp_out = beacon::compute_status(in);

        uint8_t c_airtag = 0, c_fmdn = 0;
        original_c::update_status_byte(
            tc.status_flags, tc.battery_voltage, tc.keys_changes,
            tc.what_in_status, tc.accel_byte, tc.temperature,
            &c_airtag, &c_fmdn);

        ASSERT_EQ(cpp_out.airtag_status, c_airtag);
        ASSERT_EQ(cpp_out.fmdn_status, c_fmdn);
    }
}

// ============================================================================
// main
// ============================================================================

int main() {
    printf("Running host tests...\n");
    // Tests auto-register via static constructors
    printf("\n%d/%d assertions passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
