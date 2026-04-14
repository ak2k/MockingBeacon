/*
 * SMP server: connectable BLE peripheral with MCUmgr SMP service.
 * The SMP GATT service registers automatically via SYS_INIT when
 * CONFIG_MCUMGR_TRANSPORT_BT=y — no app code needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/logging/log.h>

#include "babblekit/testcase.h"

LOG_MODULE_REGISTER(smp_server, LOG_LEVEL_INF);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_NAME_COMPLETE, 'D', 'F', 'U'),
};

void entrypoint_smp_server(void)
{
    int err;

    TEST_START("smp_server");

    err = bt_enable(NULL);
    TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

    /* Start connectable advertising — SMP GATT service is already registered */
    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
    TEST_ASSERT(err == 0, "adv_start failed (err %d)", err);

    LOG_INF("SMP server advertising (MCUmgr SMP service auto-registered)");

    /* Server's job is done — MCUmgr handles SMP requests autonomously.
     * Mark as passed and let the sim run until the client finishes. */
    TEST_PASS("smp_server");
}
