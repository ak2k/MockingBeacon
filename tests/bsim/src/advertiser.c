/*
 * Advertiser device: broadcasts 3 different AirTag keys in sequence,
 * simulating key rotation. Uses bt_le_adv_update_data() to change
 * the payload without restarting BLE (avoids bt_disable issues on bsim).
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

#include "babblekit/testcase.h"

/* Import C++ functions via extern "C" wrappers */
extern void beacon_derive_mac(const uint8_t *key, uint8_t *mac_out);
extern void beacon_fill_template(const uint8_t *key, uint8_t *tmpl, size_t tmpl_size);

LOG_MODULE_REGISTER(advertiser, LOG_LEVEL_INF);

#define NUM_KEYS 3

/* Three test keys — each produces a different 22-byte payload + 2-bit header */
static const uint8_t test_keys[NUM_KEYS][28] = {
    /* Key 0 */
    {0xA3, 0x7B, 0x1F, 0x42, 0xE8, 0x9D, 0x56, 0xC0, 0x3A, 0x88,
     0x14, 0x6E, 0xF2, 0xD7, 0x93, 0x05, 0xBC, 0x61, 0x4F, 0xAA,
     0x27, 0x8C, 0xE5, 0x30, 0x7D, 0x19, 0xB4, 0x68},
    /* Key 1 */
    {0x55, 0x12, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89,
     0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13,
     0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B},
    /* Key 2 */
    {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66,
     0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xFE, 0xDC, 0xBA, 0x98,
     0x76, 0x54, 0x32, 0x10, 0x0F, 0x1E, 0x2D, 0x3C},
};

static uint8_t adv_template[31];
static uint8_t adv_data_store[31];

static const uint8_t template_header[] = {
    0x1e, 0xff, 0x4c, 0x00, 0x12, 0x19, 0x00,
};

static void prepare_key(int idx)
{
    /* Reset template header */
    memcpy(adv_template, template_header, sizeof(template_header));
    memset(adv_template + sizeof(template_header), 0,
           sizeof(adv_template) - sizeof(template_header));

    beacon_fill_template(test_keys[idx], adv_template, sizeof(adv_template));
    memcpy(adv_data_store, adv_template, sizeof(adv_template));
}

void entrypoint_advertiser(void)
{
    int err;

    TEST_START("advertiser");

    err = bt_enable(NULL);
    TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

    /* Prepare and start with key 0 */
    prepare_key(0);

    struct bt_data ad[] = {
        BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_data_store + 2, 29),
    };

    err = bt_le_adv_start(
        BT_LE_ADV_PARAM(BT_LE_ADV_OPT_USE_IDENTITY,
                         BT_GAP_ADV_FAST_INT_MIN_1,
                         BT_GAP_ADV_FAST_INT_MAX_1, NULL),
        ad, ARRAY_SIZE(ad), NULL, 0);
    TEST_ASSERT(err == 0, "adv_start failed (err %d)", err);
    LOG_INF("Broadcasting key 0");

    /* Let key 0 advertise for 200ms */
    k_sleep(K_MSEC(200));

    /* Rotate to key 1 — update payload in place */
    prepare_key(1);
    err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    TEST_ASSERT(err == 0, "adv_update key 1 failed (err %d)", err);
    LOG_INF("Rotated to key 1");

    k_sleep(K_MSEC(200));

    /* Rotate to key 2 */
    prepare_key(2);
    err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
    TEST_ASSERT(err == 0, "adv_update key 2 failed (err %d)", err);
    LOG_INF("Rotated to key 2");

    /* All 3 keys broadcast — advertiser's job is done.
     * The BLE controller continues advertising autonomously
     * while the scanner verifies the payload. */
    TEST_PASS("advertiser");
}
