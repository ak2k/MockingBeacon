/*
 * Scanner device: receives AirTag advertisements and verifies:
 * 1. Apple Offline Finding header present
 * 2. Sees 3 different key payloads (key rotation)
 * 3. Each payload's key bytes and top-2-bits match expected values
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

#include "babblekit/flags.h"
#include "babblekit/testcase.h"

/* Import C++ function via extern "C" wrapper */
extern void beacon_derive_mac(const uint8_t *key, uint8_t *mac_out);

LOG_MODULE_REGISTER(scanner, LOG_LEVEL_INF);

#define NUM_KEYS 3

/* Same test keys as advertiser */
static const uint8_t test_keys[NUM_KEYS][28] = {
    {0xA3, 0x7B, 0x1F, 0x42, 0xE8, 0x9D, 0x56, 0xC0, 0x3A, 0x88,
     0x14, 0x6E, 0xF2, 0xD7, 0x93, 0x05, 0xBC, 0x61, 0x4F, 0xAA,
     0x27, 0x8C, 0xE5, 0x30, 0x7D, 0x19, 0xB4, 0x68},
    {0x55, 0x12, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89,
     0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13,
     0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B},
    {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66,
     0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xFE, 0xDC, 0xBA, 0x98,
     0x76, 0x54, 0x32, 0x10, 0x0F, 0x1E, 0x2D, 0x3C},
};

static bool key_seen[NUM_KEYS];
static int keys_verified;
static bool mac_checked;

DEFINE_FLAG(all_keys_verified);

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                          struct net_buf_simple *ad)
{
    if (ad->len == 0) {
        return;
    }

    struct net_buf_simple buf;
    net_buf_simple_clone(ad, &buf);

    while (buf.len > 1) {
        uint8_t len = net_buf_simple_pull_u8(&buf);
        if (len == 0 || len > buf.len) {
            break;
        }
        uint8_t ad_type = net_buf_simple_pull_u8(&buf);
        uint8_t data_len = len - 1;

        if (ad_type == BT_DATA_MANUFACTURER_DATA && data_len == 29) {
            const uint8_t *data = buf.data;

            /* Check Apple company ID */
            if (data[0] != 0x4c || data[1] != 0x00) {
                goto next;
            }
            /* Check Offline Finding type */
            if (data[2] != 0x12 || data[3] != 0x19) {
                goto next;
            }

            /* Verify advertiser's MAC matches key-0-derived MAC (first time only) */
            if (!mac_checked) {
                uint8_t expected_mac[6];
                beacon_derive_mac(test_keys[0], expected_mac);
                TEST_ASSERT(memcmp(addr->a.val, expected_mac, 6) == 0,
                            "Advertiser MAC does not match key-derived MAC");
                LOG_INF("Advertiser MAC matches key-derived MAC — "
                        "bt_ctlr_set_public_addr verified over the air");
                mac_checked = true;
            }

            /* Try to match against each expected key */
            const uint8_t *payload_key = &data[5];

            for (int k = 0; k < NUM_KEYS; k++) {
                if (key_seen[k]) {
                    continue;
                }
                if (memcmp(payload_key, &test_keys[k][6], 22) != 0) {
                    continue;
                }

                /* Key bytes match — verify top 2 bits */
                uint8_t expected_bits = test_keys[k][0] >> 6;
                TEST_ASSERT(data[27] == expected_bits,
                            "Key %d: top bits got 0x%02x, expected 0x%02x",
                            k, data[27], expected_bits);

                key_seen[k] = true;
                keys_verified++;
                LOG_INF("Verified key %d (%d/%d) — payload correct",
                        k, keys_verified, NUM_KEYS);

                if (keys_verified >= NUM_KEYS) {
                    SET_FLAG(all_keys_verified);
                }
                break;
            }
        }
next:
        net_buf_simple_pull(&buf, data_len);
    }
}

void entrypoint_scanner(void)
{
    int err;

    TEST_START("scanner");

    err = bt_enable(NULL);
    TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

    /* No duplicate filtering — we need to see updated payloads from the same address */
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_PASSIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    err = bt_le_scan_start(&scan_param, device_found);
    TEST_ASSERT(err == 0, "Scan start failed (err %d)", err);

    LOG_INF("Scanning for %d different AirTag key advertisements...", NUM_KEYS);

    WAIT_FOR_FLAG(all_keys_verified);

    LOG_INF("All %d keys verified — key rotation produces correct payloads over the air!",
            NUM_KEYS);
    TEST_PASS_AND_EXIT("scanner");
}
