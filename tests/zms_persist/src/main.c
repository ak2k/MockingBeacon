/*
 * ZMS persistence test: write values, remount (simulating reboot),
 * read back and verify. Uses the same storage partition and sector
 * layout as the real firmware (sector_count=3).
 *
 * Uses the production NVS ID enum from src/beacon_nvs_ids.hpp. Iterating
 * every production ID catches drift between test and production — the
 * prior hand-picked subset had mismatched names + values (see plan §1e
 * and data-integrity finding #3).
 *
 * Tests:
 * 1. Write/read roundtrip for each real NVS ID
 * 2. Remount (simulated reboot) preserves all data
 * 3. Overwrite + remount preserves latest value
 * 4. Missing ID returns -ENOENT after fresh mount
 * 5. Every production ID survives one round-trip (iteration via X-macro)
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/zms.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <stdlib.h>
#include <string.h>

#include "beacon_nvs_ids.hpp"

#define ZMS_PARTITION        storage_partition
#define ZMS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(ZMS_PARTITION)
#define ZMS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(ZMS_PARTITION)

static struct zms_fs fs;
static int test_count;
static int pass_count;

#define ASSERT_EQ(a, b, msg) do { \
    test_count++; \
    if ((a) == (b)) { pass_count++; } \
    else { printk("FAIL: %s: got %d, expected %d\n", msg, (int)(a), (int)(b)); } \
} while (0)

#define ASSERT_MEM_EQ(a, b, len, msg) do { \
    test_count++; \
    if (memcmp(a, b, len) == 0) { pass_count++; } \
    else { printk("FAIL: %s: memory mismatch\n", msg); } \
} while (0)

static int mount_zms(void)
{
    struct flash_pages_info info;
    int rc;

    fs.flash_device = ZMS_PARTITION_DEVICE;
    if (!device_is_ready(fs.flash_device)) {
        printk("Flash device not ready\n");
        return -1;
    }
    fs.offset = ZMS_PARTITION_OFFSET;
    rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) {
        printk("Unable to get page info: %d\n", rc);
        return -1;
    }
    fs.sector_size = info.size;
    fs.sector_count = 3U;

    rc = zms_mount(&fs);
    if (rc) {
        printk("ZMS mount failed: %d\n", rc);
        return -1;
    }
    return 0;
}

static void test_write_read_roundtrip(void)
{
    printk("\n--- test_write_read_roundtrip ---\n");
    int rc;

    /* 4-byte integer settings */
    uint32_t val_out = 1;
    rc = zms_write(&fs, ID_airtag_NVS, &val_out, sizeof(val_out));
    ASSERT_EQ(rc, sizeof(val_out), "write airtag flag");

    uint32_t val_in = 0;
    rc = zms_read(&fs, ID_airtag_NVS, &val_in, sizeof(val_in));
    ASSERT_EQ(rc, sizeof(val_in), "read airtag flag len");
    ASSERT_EQ(val_in, 1, "read airtag flag value");

    /* 8-byte auth code */
    uint8_t auth_out[8] = "testauth";
    rc = zms_write(&fs, ID_auth_NVS, auth_out, sizeof(auth_out));
    ASSERT_EQ(rc, sizeof(auth_out), "write auth");

    uint8_t auth_in[8] = {0};
    rc = zms_read(&fs, ID_auth_NVS, auth_in, sizeof(auth_in));
    ASSERT_EQ(rc, sizeof(auth_in), "read auth len");
    ASSERT_MEM_EQ(auth_in, auth_out, sizeof(auth_out), "read auth value");

    /* Large blob: 28-byte key × 3 = 84 bytes */
    uint8_t keys_out[84];
    for (int i = 0; i < 84; i++) {
        keys_out[i] = (uint8_t)(i ^ 0xAB);
    }
    rc = zms_write(&fs, ID_key_NVS, keys_out, sizeof(keys_out));
    ASSERT_EQ(rc, sizeof(keys_out), "write keys");

    uint8_t keys_in[84] = {0};
    rc = zms_read(&fs, ID_key_NVS, keys_in, sizeof(keys_in));
    ASSERT_EQ(rc, sizeof(keys_in), "read keys len");
    ASSERT_MEM_EQ(keys_in, keys_out, sizeof(keys_out), "read keys value");
}

static void test_persist_across_remount(void)
{
    printk("\n--- test_persist_across_remount ---\n");
    int rc;

    /* Write values */
    uint32_t interval = 600;
    rc = zms_write(&fs, ID_changeInterval_NVS, &interval, sizeof(interval));
    ASSERT_EQ(rc, sizeof(interval), "write interval");

    uint32_t tx = 2;
    rc = zms_write(&fs, ID_power_NVS, &tx, sizeof(tx));
    ASSERT_EQ(rc, sizeof(tx), "write tx_power");

    int64_t time_val = 1700000000LL;
    rc = zms_write(&fs, ID_timeOffset_NVS, &time_val, sizeof(time_val));
    ASSERT_EQ(rc, sizeof(time_val), "write time");

    /* Remount (simulates reboot — flash state persists) */
    rc = mount_zms();
    ASSERT_EQ(rc, 0, "remount");

    /* Read back after remount */
    uint32_t interval_read = 0;
    rc = zms_read(&fs, ID_changeInterval_NVS, &interval_read, sizeof(interval_read));
    ASSERT_EQ(rc, sizeof(interval_read), "read interval after remount");
    ASSERT_EQ(interval_read, 600, "interval value after remount");

    uint32_t tx_read = 0;
    rc = zms_read(&fs, ID_power_NVS, &tx_read, sizeof(tx_read));
    ASSERT_EQ(rc, sizeof(tx_read), "read tx_power after remount");
    ASSERT_EQ(tx_read, 2, "tx_power value after remount");

    int64_t time_read = 0;
    rc = zms_read(&fs, ID_timeOffset_NVS, &time_read, sizeof(time_read));
    ASSERT_EQ(rc, sizeof(time_read), "read time after remount");
    ASSERT_EQ(time_read == 1700000000LL ? 1 : 0, 1, "time value after remount");
}

static void test_overwrite_persists(void)
{
    printk("\n--- test_overwrite_persists ---\n");
    int rc;

    /* Write initial value */
    uint32_t val = 1;
    rc = zms_write(&fs, ID_period_NVS, &val, sizeof(val));
    ASSERT_EQ(rc, sizeof(val), "write period initial");

    /* Overwrite */
    val = 4;
    rc = zms_write(&fs, ID_period_NVS, &val, sizeof(val));
    ASSERT_EQ(rc, sizeof(val), "write period overwrite");

    /* Remount */
    rc = mount_zms();
    ASSERT_EQ(rc, 0, "remount after overwrite");

    /* Should read latest value */
    uint32_t val_read = 0;
    rc = zms_read(&fs, ID_period_NVS, &val_read, sizeof(val_read));
    ASSERT_EQ(rc, sizeof(val_read), "read period after remount");
    ASSERT_EQ(val_read, 4, "period value is latest");
}

static void test_missing_id(void)
{
    printk("\n--- test_missing_id ---\n");

    /* ID 0xFF was never written — should return -ENOENT */
    uint32_t dummy = 0;
    int rc = zms_read(&fs, 0xFF, &dummy, sizeof(dummy));
    ASSERT_EQ(rc < 0 ? 1 : 0, 1, "unwritten ID returns error");
}

/* Exercises every ID in the production enum. Catches drift where the
 * test (above) might use an alias, while production code uses a freshly
 * added ID. X-macro expansion means adding an ID to beacon_nvs_ids.hpp
 * automatically adds coverage here — no test edit needed. */
static void test_all_production_ids_roundtrip(void)
{
    printk("\n--- test_all_production_ids_roundtrip ---\n");
    int rc;

    /* Write a distinctive pattern per ID */
#define X(name, val) do { \
        uint32_t v_out = 0xC0DE0000u | (uint32_t)ID_##name; \
        rc = zms_write(&fs, ID_##name, &v_out, sizeof(v_out)); \
        ASSERT_EQ(rc, sizeof(v_out), "write " #name); \
    } while (0);
    BEACON_NVS_IDS(X)
#undef X

    /* Remount to force read-from-flash path */
    rc = mount_zms();
    ASSERT_EQ(rc, 0, "remount after all-IDs write");

    /* Read back and verify each */
#define X(name, val) do { \
        uint32_t v_in = 0; \
        uint32_t v_expect = 0xC0DE0000u | (uint32_t)ID_##name; \
        rc = zms_read(&fs, ID_##name, &v_in, sizeof(v_in)); \
        ASSERT_EQ(rc, sizeof(v_in), "read " #name " len"); \
        ASSERT_EQ(v_in, v_expect, "read " #name " value"); \
    } while (0);
    BEACON_NVS_IDS(X)
#undef X
}

int main(void)
{
    printk("ZMS persistence test\n");

    int rc = mount_zms();
    if (rc) {
        printk("FATAL: initial mount failed\n");
        return 1;
    }
    printk("ZMS mounted: sector_size=%u, sector_count=%u\n",
           fs.sector_size, fs.sector_count);

    test_write_read_roundtrip();
    test_persist_across_remount();
    test_overwrite_persists();
    test_missing_id();
    test_all_production_ids_roundtrip();

    printk("\n%d/%d assertions passed\n", pass_count, test_count);

    if (pass_count == test_count) {
        printk("ZMS persistence test PASSED\n");
        exit(0);
    } else {
        printk("ZMS persistence test FAILED\n");
        exit(1);
    }
}
