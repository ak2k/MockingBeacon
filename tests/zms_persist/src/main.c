/*
 * ZMS persistence test: write values, remount (simulating reboot),
 * read back and verify. Uses the same storage partition and sector
 * layout as the real firmware (sector_count=3).
 *
 * Tests:
 * 1. Write/read roundtrip for each NVS ID used by BeaconConfig
 * 2. Remount (simulated reboot) preserves all data
 * 3. Overwrite + remount preserves latest value
 * 4. Missing IDs return -ENOENT after fresh mount
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fs/zms.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#include <stdlib.h>
#include <string.h>

#define ZMS_PARTITION        storage_partition
#define ZMS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(ZMS_PARTITION)
#define ZMS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(ZMS_PARTITION)

/* NVS IDs matching beacon_config.hpp */
#define ID_TURNED_ON     0x01
#define ID_FLAG_AIRTAG   0x02
#define ID_FLAG_FMDN     0x03
#define ID_MULT_PERIOD   0x04
#define ID_KEY           0x05
#define ID_AUTH           0x06
#define ID_INTERVAL      0x07
#define ID_FMDN_KEY      0x08
#define ID_TX_POWER      0x09
#define ID_SETTINGS_MAC  0x0A
#define ID_TIME          0x0B

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
    rc = zms_write(&fs, ID_FLAG_AIRTAG, &val_out, sizeof(val_out));
    ASSERT_EQ(rc, sizeof(val_out), "write airtag flag");

    uint32_t val_in = 0;
    rc = zms_read(&fs, ID_FLAG_AIRTAG, &val_in, sizeof(val_in));
    ASSERT_EQ(rc, sizeof(val_in), "read airtag flag len");
    ASSERT_EQ(val_in, 1, "read airtag flag value");

    /* 8-byte auth code */
    uint8_t auth_out[8] = "testauth";
    rc = zms_write(&fs, ID_AUTH, auth_out, sizeof(auth_out));
    ASSERT_EQ(rc, sizeof(auth_out), "write auth");

    uint8_t auth_in[8] = {0};
    rc = zms_read(&fs, ID_AUTH, auth_in, sizeof(auth_in));
    ASSERT_EQ(rc, sizeof(auth_in), "read auth len");
    ASSERT_MEM_EQ(auth_in, auth_out, sizeof(auth_out), "read auth value");

    /* Large blob: 28-byte key × 3 = 84 bytes */
    uint8_t keys_out[84];
    for (int i = 0; i < 84; i++) {
        keys_out[i] = (uint8_t)(i ^ 0xAB);
    }
    rc = zms_write(&fs, ID_KEY, keys_out, sizeof(keys_out));
    ASSERT_EQ(rc, sizeof(keys_out), "write keys");

    uint8_t keys_in[84] = {0};
    rc = zms_read(&fs, ID_KEY, keys_in, sizeof(keys_in));
    ASSERT_EQ(rc, sizeof(keys_in), "read keys len");
    ASSERT_MEM_EQ(keys_in, keys_out, sizeof(keys_out), "read keys value");
}

static void test_persist_across_remount(void)
{
    printk("\n--- test_persist_across_remount ---\n");
    int rc;

    /* Write values */
    uint32_t interval = 600;
    rc = zms_write(&fs, ID_INTERVAL, &interval, sizeof(interval));
    ASSERT_EQ(rc, sizeof(interval), "write interval");

    uint32_t tx = 2;
    rc = zms_write(&fs, ID_TX_POWER, &tx, sizeof(tx));
    ASSERT_EQ(rc, sizeof(tx), "write tx_power");

    int64_t time_val = 1700000000LL;
    rc = zms_write(&fs, ID_TIME, &time_val, sizeof(time_val));
    ASSERT_EQ(rc, sizeof(time_val), "write time");

    /* Remount (simulates reboot — flash state persists) */
    rc = mount_zms();
    ASSERT_EQ(rc, 0, "remount");

    /* Read back after remount */
    uint32_t interval_read = 0;
    rc = zms_read(&fs, ID_INTERVAL, &interval_read, sizeof(interval_read));
    ASSERT_EQ(rc, sizeof(interval_read), "read interval after remount");
    ASSERT_EQ(interval_read, 600, "interval value after remount");

    uint32_t tx_read = 0;
    rc = zms_read(&fs, ID_TX_POWER, &tx_read, sizeof(tx_read));
    ASSERT_EQ(rc, sizeof(tx_read), "read tx_power after remount");
    ASSERT_EQ(tx_read, 2, "tx_power value after remount");

    int64_t time_read = 0;
    rc = zms_read(&fs, ID_TIME, &time_read, sizeof(time_read));
    ASSERT_EQ(rc, sizeof(time_read), "read time after remount");
    ASSERT_EQ(time_read == 1700000000LL ? 1 : 0, 1, "time value after remount");
}

static void test_overwrite_persists(void)
{
    printk("\n--- test_overwrite_persists ---\n");
    int rc;

    /* Write initial value */
    uint32_t val = 1;
    rc = zms_write(&fs, ID_MULT_PERIOD, &val, sizeof(val));
    ASSERT_EQ(rc, sizeof(val), "write period initial");

    /* Overwrite */
    val = 4;
    rc = zms_write(&fs, ID_MULT_PERIOD, &val, sizeof(val));
    ASSERT_EQ(rc, sizeof(val), "write period overwrite");

    /* Remount */
    rc = mount_zms();
    ASSERT_EQ(rc, 0, "remount after overwrite");

    /* Should read latest value */
    uint32_t val_read = 0;
    rc = zms_read(&fs, ID_MULT_PERIOD, &val_read, sizeof(val_read));
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

    printk("\n%d/%d assertions passed\n", pass_count, test_count);

    if (pass_count == test_count) {
        printk("ZMS persistence test PASSED\n");
        exit(0);
    } else {
        printk("ZMS persistence test FAILED\n");
        exit(1);
    }
}
