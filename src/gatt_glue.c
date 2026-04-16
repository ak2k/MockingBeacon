// gatt_glue.c -- BT_GATT_SERVICE_DEFINE + connection callbacks (pure C)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/controller.h>
#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#include <string.h>

#include "gatt_glue.h"
#include "myboards.h"

// ---- Glue pointers (set by glue_init) ----
static void *g_settings_manager = NULL;
static void *g_state_machine = NULL;

void glue_init(void *settings_manager, void *state_machine) {
    g_settings_manager = settings_manager;
    g_state_machine = state_machine;
}

// ---- UUID definitions (from settings.c) ----

static const struct bt_uuid_128 fmdn_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debdb, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 switchAirtag_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debdc, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 period_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debdd, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 key_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debde, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 auth_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debdf, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 changeInterval_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe0, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 txPower_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe1, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 fmdnKey_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe2, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 timeOffset_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe3, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 settingsMAC_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe4, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 status_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe5, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

static const struct bt_uuid_128 accel_uuid = BT_UUID_INIT_128(
    BT_UUID_128_ENCODE(0x8c5debe6, 0xad8d, 0x4810, 0xa31f, 0x53862e79ee77));

#define BEACON_SERVICE_UUID_VAL \
    BT_UUID_128_ENCODE(0x5cfce313, 0xa7e3, 0x45c3, 0x933d, 0x418b8100da7f)

static const struct bt_uuid_128 beacon_svc_uuid = BT_UUID_INIT_128(BEACON_SERVICE_UUID_VAL);

// ---- Connection state (shared with C++ via glue bridge) ----
int connectedGatt = 0;
int authorizedGatt = 0;
int allowedChange = 0;
int needsReset = 0;
int pauseUpload = 0;
int keysReceived = 0;
int updateKeysAtDisconnect = 0;

// ---- GAP connection callbacks ----

static void connected(struct bt_conn *conn, uint8_t err) {
    allowedChange = 0;
    if (!err) {
        connectedGatt = 1;
        printk("Connected\n");
    } else {
        printk("Connection failed (err 0x%02x)\n", err);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    connectedGatt = 0;
    authorizedGatt = 0;
    allowedChange = 0;
    printk("Disconnected (reason 0x%02x)\n", reason);
    (void)reason;
    // Keys write at disconnect is handled by the bridge functions
    if (updateKeysAtDisconnect) {
        beacon_glue_handle_key(g_settings_manager, NULL, 0);
        updateKeysAtDisconnect = 0;
    }
}

// Zephyr 4.0+ .recycled callback: fires after the conn object is released
// (post-disconnect). This is where we trigger adv restart — bt_host forbids
// calling bt_le_adv_start from .recycled context, so we submit work to the
// dedicated adv_wq (see zephyr_hardware.cpp).
//
// Also clears authorizedGatt here (belt-and-suspenders). Without this, a
// workqueue race where .recycled fires before .disconnected cleanup
// completes could let a new client inherit prior auth.
extern void beacon_adv_recycled_cb(void);
static void recycled(void) {
    authorizedGatt = 0;
    allowedChange = 0;
    beacon_adv_recycled_cb();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

// ---- GATT write callbacks (thin wrappers calling C++ bridge) ----

static ssize_t chrc_write_auth(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    int rc = beacon_glue_handle_auth(g_settings_manager, buf, len);
    if (rc > 0) {
        pauseUpload = 1;
        allowedChange = 1;
        authorizedGatt = 1;
    } else {
        allowedChange = 0;
        needsReset = 0;
        pauseUpload = 0;
    }
    return len;
}

static ssize_t chrc_write_key(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    int rc = beacon_glue_handle_key(g_settings_manager, buf, len);
    if (rc < 0)
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    keysReceived++;
    if (keysReceived % 2 == 0) {
        needsReset = 1;
        updateKeysAtDisconnect = 1;
        pauseUpload = 0;
    }
    return len;
}

static ssize_t chrc_write_period(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_period(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_fmdn(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_fmdn(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_airtag(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_airtag(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_changeInterval(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_change_interval(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_txPower(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_tx_power(g_settings_manager, buf, len);
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_fmdnKey(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_fmdn_key(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

static int64_t timeValue;

static ssize_t chrc_read_timeOffset(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, void *buf,
    uint16_t len, uint16_t offset) {
    (void)conn;
    if (len < sizeof(int64_t))
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    if (!allowedChange)
        return 0;
    beacon_glue_handle_time_offset_read(g_settings_manager, &timeValue);
    pauseUpload = 0;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &timeValue, sizeof(timeValue));
}

static ssize_t chrc_write_timeOffset(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_time_offset_write(g_settings_manager, buf, len);
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_settingsMAC(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_settings_mac(g_settings_manager, buf, len);
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_status(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_status(g_settings_manager, buf, len);
    pauseUpload = 0;
    return len;
}

static ssize_t chrc_write_accel(struct bt_conn *conn,
    const struct bt_gatt_attr *attr, const void *buf,
    uint16_t len, uint16_t offset, uint8_t flags) {
    (void)conn; (void)attr; (void)offset; (void)flags;
    if (!allowedChange)
        return len;
    beacon_glue_handle_accel(g_settings_manager, buf, len);
    needsReset = 1;
    pauseUpload = 0;
    return len;
}

// ---- Authorization callbacks ----

static bool write_authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr) {
    (void)conn;
    if (authorizedGatt)
        return true;
    if (!bt_uuid_cmp(attr->uuid, &auth_uuid.uuid))
        return true;
    return false;
}

static bool read_authorize(struct bt_conn *conn, const struct bt_gatt_attr *attr) {
    (void)conn; (void)attr;
    return true;
}

static const struct bt_gatt_authorization_cb auth_callbacks = {
    .read_authorize = read_authorize,
    .write_authorize = write_authorize,
};

// ---- GATT service definition ----

BT_GATT_SERVICE_DEFINE(
    beacon_svc, BT_GATT_PRIMARY_SERVICE(&beacon_svc_uuid),
    BT_GATT_CHARACTERISTIC(&auth_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_auth, NULL),
    BT_GATT_CHARACTERISTIC(&key_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_key, NULL),
    BT_GATT_CHARACTERISTIC(&period_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_period, NULL),
    BT_GATT_CHARACTERISTIC(&fmdn_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_fmdn, NULL),
    BT_GATT_CHARACTERISTIC(&switchAirtag_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_airtag, NULL),
    BT_GATT_CHARACTERISTIC(&changeInterval_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_changeInterval, NULL),
    BT_GATT_CHARACTERISTIC(&txPower_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_txPower, NULL),
    BT_GATT_CHARACTERISTIC(&fmdnKey_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_fmdnKey, NULL),
    BT_GATT_CHARACTERISTIC(&timeOffset_uuid.uuid,
        BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ,
        BT_GATT_PERM_WRITE | BT_GATT_PERM_READ,
        chrc_read_timeOffset, chrc_write_timeOffset, &timeValue),
    BT_GATT_CHARACTERISTIC(&settingsMAC_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_settingsMAC, NULL),
    BT_GATT_CHARACTERISTIC(&status_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_status, NULL),
    BT_GATT_CHARACTERISTIC(&accel_uuid.uuid,
        BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
        NULL, chrc_write_accel, NULL),
);

// ---- Settings advertisement data (C99 compound literals) ----

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_LIMITED | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_UDS_VAL)),
};

// Disconnect callback function
static void disconnect_cb(struct bt_conn *conn, void *data) {
    (void)data;
    (void)bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

void start_settings_adv(void) {
    int err;

    connectedGatt = 0;
    authorizedGatt = 0;
    allowedChange = 0;
    keysReceived = 0;
    updateKeysAtDisconnect = 0;
    err = bt_gatt_authorization_cb_register(&auth_callbacks);
    if (err) {
        printk("Error at setting auth callbacks (err %d)\n", err);
    }
    err = bt_le_adv_start(BT_LE_ADV_PARAM(
            BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_CONN,
            SETTINGS_ADV_INTERVAL * 1.6 - 20,
            SETTINGS_ADV_INTERVAL * 1.6 + 20, NULL),
        ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        printk("Settings Advertising failed to start (err %d)\n", err);
        return;
    }
    printk("Settings Advertising successfully started\n");
}

void stop_settings_adv(void) {
    bt_le_adv_stop();
    bt_conn_foreach(BT_CONN_TYPE_ALL, disconnect_cb, NULL);
}
