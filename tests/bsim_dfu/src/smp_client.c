/*
 * SMP client: connects to the server, discovers the MCUmgr SMP GATT
 * service, sends an SMP echo request, and verifies the response.
 *
 * This validates that the MCUmgr BLE transport works end-to-end
 * over a simulated radio without requiring MCUboot or image uploads.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "babblekit/flags.h"
#include "babblekit/testcase.h"

LOG_MODULE_REGISTER(smp_client, LOG_LEVEL_INF);

/* MCUmgr SMP service UUID: 8D53DC1D-1DB7-4CD3-868B-8A527460AA84 */
static struct bt_uuid_128 smp_svc_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8D53DC1D, 0x1DB7, 0x4CD3, 0x868B, 0x8A527460AA84));

/* MCUmgr SMP characteristic UUID: DA2E7828-FBCE-4E01-AE9E-261174997C48 */
static struct bt_uuid_128 smp_chr_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0xDA2E7828, 0xFBCE, 0x4E01, 0xAE9E, 0x261174997C48));

/*
 * SMP echo request (hand-crafted):
 *   Header (8 bytes): op=2 (write), flags=0, len=8, group=1 (OS), seq=0, id=0 (echo)
 *   CBOR payload (8 bytes): {"d":"test"} = a1 61 64 64 74 65 73 74
 */
static const uint8_t smp_echo_req[] = {
    0x02, 0x00, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00,
    0xa1, 0x61, 0x64, 0x64, 0x74, 0x65, 0x73, 0x74,
};

/* Expected CBOR in echo response: {"r":"test"} = a1 61 72 64 74 65 73 74 */
static const uint8_t expected_cbor[] = {
    0xa1, 0x61, 0x72, 0x64, 0x74, 0x65, 0x73, 0x74,
};

static struct bt_conn *default_conn;
static uint16_t smp_handle;

DEFINE_FLAG(connected);
DEFINE_FLAG(svc_discovered);
DEFINE_FLAG(subscribed);
DEFINE_FLAG(echo_verified);

/* ---- Connection callbacks ---- */

static void conn_cb(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        TEST_FAIL("Connection failed (err %u)", err);
        return;
    }
    default_conn = bt_conn_ref(conn);
    LOG_INF("Connected");
    SET_FLAG(connected);
}

static void disconn_cb(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason %u)", reason);
    bt_conn_unref(default_conn);
    default_conn = NULL;
}

BT_CONN_CB_DEFINE(conn_cbs) = {
    .connected = conn_cb,
    .disconnected = disconn_cb,
};

/* ---- GATT discovery ---- */

static uint8_t discover_cb(struct bt_conn *conn,
                           const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    if (!attr) {
        LOG_INF("Discovery complete");
        return BT_GATT_ITER_STOP;
    }

    if (bt_uuid_cmp(params->uuid, &smp_svc_uuid.uuid) == 0) {
        LOG_INF("Found SMP service at handle %u", attr->handle);
        /* Now discover the SMP characteristic within this service */
        static struct bt_gatt_discover_params chr_params;
        chr_params.uuid = &smp_chr_uuid.uuid;
        chr_params.start_handle = attr->handle + 1;
        chr_params.end_handle = 0xFFFF;
        chr_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        chr_params.func = discover_cb;
        bt_gatt_discover(conn, &chr_params);
        return BT_GATT_ITER_STOP;
    }

    if (bt_uuid_cmp(params->uuid, &smp_chr_uuid.uuid) == 0) {
        /* Value handle is one past the characteristic declaration */
        smp_handle = bt_gatt_attr_value_handle(attr);
        LOG_INF("Found SMP characteristic, value handle %u", smp_handle);
        SET_FLAG(svc_discovered);
        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_CONTINUE;
}

/* ---- SMP notification handler ---- */

static uint8_t notify_cb(struct bt_conn *conn,
                         struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length)
{
    if (!data) {
        LOG_INF("Unsubscribed");
        return BT_GATT_ITER_STOP;
    }

    LOG_INF("SMP response received (%u bytes)", length);

    /* Response = 8-byte SMP header + CBOR payload */
    TEST_ASSERT(length >= 8 + sizeof(expected_cbor),
                "Response too short: %u bytes", length);

    const uint8_t *hdr = data;
    TEST_ASSERT(hdr[0] == 0x03, "Expected op=3 (write response), got %u", hdr[0]);
    TEST_ASSERT(hdr[4] == 0x00 && hdr[5] == 0x01, "Expected group=1 (OS)");
    TEST_ASSERT(hdr[7] == 0x00, "Expected id=0 (echo)");

    /* Verify CBOR payload: {"r":"test"} */
    const uint8_t *cbor = hdr + 8;
    TEST_ASSERT(memcmp(cbor, expected_cbor, sizeof(expected_cbor)) == 0,
                "Echo response payload mismatch");

    LOG_INF("SMP echo response verified — MCUmgr BLE transport works!");
    SET_FLAG(echo_verified);
    return BT_GATT_ITER_CONTINUE;
}

static struct bt_gatt_subscribe_params sub_params;

static void subscribe_cb(struct bt_conn *conn, uint8_t err,
                         struct bt_gatt_subscribe_params *params)
{
    TEST_ASSERT(err == 0, "Subscribe failed (err %u)", err);
    LOG_INF("Subscribed to SMP notifications");
    SET_FLAG(subscribed);
}

/* ---- Scanner ---- */

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad)
{
    int err;

    if (type != BT_GAP_ADV_TYPE_ADV_IND) {
        return;
    }

    err = bt_le_scan_stop();
    if (err) {
        return;
    }

    struct bt_conn_le_create_param create_param = BT_CONN_LE_CREATE_PARAM_INIT(
        BT_CONN_LE_OPT_NONE, BT_GAP_SCAN_FAST_INTERVAL, BT_GAP_SCAN_FAST_WINDOW);
    struct bt_le_conn_param conn_param = *BT_LE_CONN_PARAM_DEFAULT;

    err = bt_conn_le_create(addr, &create_param, &conn_param, &default_conn);
    if (err) {
        TEST_FAIL("Create conn failed (err %d)", err);
    }
}

/* ---- Entry point ---- */

void entrypoint_smp_client(void)
{
    int err;

    TEST_START("smp_client");

    err = bt_enable(NULL);
    TEST_ASSERT(err == 0, "bt_enable failed (err %d)", err);

    /* Scan for the server */
    struct bt_le_scan_param scan_param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .options = BT_LE_SCAN_OPT_NONE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    err = bt_le_scan_start(&scan_param, device_found);
    TEST_ASSERT(err == 0, "Scan start failed (err %d)", err);
    LOG_INF("Scanning for SMP server...");

    WAIT_FOR_FLAG(connected);

    /* Discover SMP service */
    static struct bt_gatt_discover_params disc_params;
    disc_params.uuid = &smp_svc_uuid.uuid;
    disc_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    disc_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    disc_params.type = BT_GATT_DISCOVER_PRIMARY;
    disc_params.func = discover_cb;

    err = bt_gatt_discover(default_conn, &disc_params);
    TEST_ASSERT(err == 0, "Discovery start failed (err %d)", err);

    WAIT_FOR_FLAG(svc_discovered);

    /* Subscribe to SMP notifications */
    sub_params.value_handle = smp_handle;
    sub_params.ccc_handle = 0; /* auto-discover CCC */
    sub_params.notify = notify_cb;
    sub_params.subscribe = subscribe_cb;
    sub_params.value = BT_GATT_CCC_NOTIFY;

    err = bt_gatt_subscribe(default_conn, &sub_params);
    TEST_ASSERT(err == 0, "Subscribe failed (err %d)", err);

    WAIT_FOR_FLAG(subscribed);

    /* Send SMP echo request */
    LOG_INF("Sending SMP echo request...");
    err = bt_gatt_write_without_response(default_conn, smp_handle,
                                         smp_echo_req, sizeof(smp_echo_req), false);
    TEST_ASSERT(err == 0, "GATT write failed (err %d)", err);

    WAIT_FOR_FLAG(echo_verified);

    TEST_PASS_AND_EXIT("smp_client");
}
