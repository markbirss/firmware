// NRF54L15Bluetooth.cpp — Zephyr BLE GATT peripheral for Meshtastic nRF54L15
//
// GATT profile (identical UUIDs to the nRF52 / NimBLE implementations):
//   Service:   6ba1b218-15a8-461f-9fa8-5dcae273eafd
//   fromNum:   ed9da18c-a800-4f66-a670-aa7547e34453  READ | NOTIFY
//   fromRadio: 2c55e69e-4993-11ed-b878-0242ac120002  READ
//   toRadio:   f75c76d2-129e-4dad-a1dd-7866124401e7  WRITE
//   logRadio:  5a3d6e49-06e6-4423-9944-e9de8cdf9547  READ | NOTIFY | INDICATE
//
// Threading model:
//   - BT RX thread calls connected_cb / disconnected_cb / GATT write_toradio
//   - Meshtastic main thread calls onNowHasData → bt_gatt_notify (thread-safe in Zephyr)
//   - active_conn and CCC flags protected by ble_mutex where needed

#include "NRF54L15Bluetooth.h"
#include "BluetoothCommon.h"
#include "BluetoothStatus.h"
#include "PowerFSM.h"
#include "configuration.h"
#include "main.h"
#include "mesh/PhoneAPI.h"
#include "mesh/mesh-pb-constants.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

// ── UUID definitions (little-endian per Bluetooth spec) ───────────────────────
// Syntax: replace hyphens with commas, prefix 0x — matches BT_UUID_128_ENCODE doc.

#define MESH_SVC_UUID_VAL \
    BT_UUID_128_ENCODE(0x6ba1b218, 0x15a8, 0x461f, 0x9fa8, 0x5dcae273eafd)
#define FROMNUM_UUID_VAL \
    BT_UUID_128_ENCODE(0xed9da18c, 0xa800, 0x4f66, 0xa670, 0xaa7547e34453)
#define FROMRADIO_UUID_VAL \
    BT_UUID_128_ENCODE(0x2c55e69e, 0x4993, 0x11ed, 0xb878, 0x0242ac120002)
#define TORADIO_UUID_VAL \
    BT_UUID_128_ENCODE(0xf75c76d2, 0x129e, 0x4dad, 0xa1dd, 0x7866124401e7)
#define LOGRADIO_UUID_VAL \
    BT_UUID_128_ENCODE(0x5a3d6e49, 0x06e6, 0x4423, 0x9944, 0xe9de8cdf9547)

static const struct bt_uuid_128 mesh_svc_uuid  = BT_UUID_INIT_128(MESH_SVC_UUID_VAL);
static const struct bt_uuid_128 fromnum_uuid   = BT_UUID_INIT_128(FROMNUM_UUID_VAL);
static const struct bt_uuid_128 fromradio_uuid = BT_UUID_INIT_128(FROMRADIO_UUID_VAL);
static const struct bt_uuid_128 toradio_uuid   = BT_UUID_INIT_128(TORADIO_UUID_VAL);
static const struct bt_uuid_128 logradio_uuid  = BT_UUID_INIT_128(LOGRADIO_UUID_VAL);

// ── Module state ─────────────────────────────────────────────────────────────

static struct bt_conn *active_conn = nullptr;
static K_MUTEX_DEFINE(ble_mutex);

static bool bt_initialized = false; // bt_enable() called at most once
static bool ble_enabled    = false; // set by setup(), cleared by shutdown()

// CCC state: 0=off, BT_GATT_CCC_NOTIFY=notify, BT_GATT_CCC_INDICATE=indicate
static uint16_t fromnum_ccc_val  = 0;
static uint16_t logradio_ccc_val = 0;

// Scratch buffers — only one BLE operation at a time
static uint8_t fromRadioBytes[meshtastic_FromRadio_size];
static size_t  fromRadioLen  = 0;
static uint8_t toRadioBytes[meshtastic_ToRadio_size];
static uint8_t lastToRadio[MAX_TO_FROM_RADIO_SIZE];
static uint32_t fromNumValue = 0;

// ── BluetoothPhoneAPI ─────────────────────────────────────────────────────────

class BluetoothPhoneAPI : public PhoneAPI
{
    virtual void onNowHasData(uint32_t fromRadioNum) override;
    virtual bool checkIsConnected() override;

  public:
    BluetoothPhoneAPI() { api_type = TYPE_BLE; }
};

static BluetoothPhoneAPI *phoneAPI = nullptr;

// ── CCC change callbacks ──────────────────────────────────────────────────────

static void fromnum_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    fromnum_ccc_val = value;
    LOG_INFO("BLE fromNum CCC: %u", value);
}

static void logradio_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    logradio_ccc_val = value;
    LOG_INFO("BLE logRadio CCC: %u", value);
}

// ── GATT attribute callbacks ──────────────────────────────────────────────────

static ssize_t read_fromnum(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                             void *buf, uint16_t len, uint16_t offset)
{
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &fromNumValue, sizeof(fromNumValue));
}

static ssize_t read_fromradio(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               void *buf, uint16_t len, uint16_t offset)
{
    if (offset == 0) {
        // First chunk: pull the next packet from the queue
        fromRadioLen = phoneAPI ? phoneAPI->getFromRadio(fromRadioBytes) : 0;
    }
    // bt_gatt_attr_read handles slicing for long reads (ATT_READ_BLOB)
    return bt_gatt_attr_read(conn, attr, buf, len, offset, fromRadioBytes, fromRadioLen);
}

static ssize_t write_toradio(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                              const void *buf, uint16_t len, uint16_t offset,
                              uint8_t flags)
{
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    if (len > sizeof(toRadioBytes)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    // Deduplicate — drop packet if identical to the last one we processed
    if (len <= MAX_TO_FROM_RADIO_SIZE && memcmp(lastToRadio, buf, len) != 0) {
        memcpy(lastToRadio, buf, len);
        if (len < MAX_TO_FROM_RADIO_SIZE) {
            memset(lastToRadio + len, 0, MAX_TO_FROM_RADIO_SIZE - len);
        }
        if (phoneAPI) {
            phoneAPI->handleToRadio((uint8_t *)buf, len);
        }
    }
    return (ssize_t)len;
}

// ── GATT service definition (static, linked at compile time) ──────────────────
//
// Attribute indices (0-based):
//   [0]  Primary Service declaration
//   [1]  fromNum characteristic declaration
//   [2]  fromNum value            ← notify target (FROMNUM_ATTR_IDX)
//   [3]  fromNum CCC descriptor
//   [4]  fromRadio characteristic declaration
//   [5]  fromRadio value
//   [6]  toRadio characteristic declaration
//   [7]  toRadio value
//   [8]  logRadio characteristic declaration
//   [9]  logRadio value           ← notify target (LOGRADIO_ATTR_IDX)
//   [10] logRadio CCC descriptor

#define FROMNUM_ATTR_IDX  2
#define LOGRADIO_ATTR_IDX 9

BT_GATT_SERVICE_DEFINE(mesh_svc,
    BT_GATT_PRIMARY_SERVICE(&mesh_svc_uuid.uuid),

    // fromNum: READ | NOTIFY — packet-counter triggers phone to read fromRadio
    BT_GATT_CHARACTERISTIC(&fromnum_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ,
                           read_fromnum, NULL, &fromNumValue),
    BT_GATT_CCC(fromnum_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // fromRadio: READ — phone polls this after receiving a fromNum notification
    BT_GATT_CHARACTERISTIC(&fromradio_uuid.uuid,
                           BT_GATT_CHRC_READ,
                           BT_GATT_PERM_READ,
                           read_fromradio, NULL, NULL),

    // toRadio: WRITE — phone sends protobuf packets to the device
    BT_GATT_CHARACTERISTIC(&toradio_uuid.uuid,
                           BT_GATT_CHRC_WRITE,
                           BT_GATT_PERM_WRITE,
                           NULL, write_toradio, NULL),

    // logRadio: READ | NOTIFY | INDICATE — log stream to phone when connected
    BT_GATT_CHARACTERISTIC(&logradio_uuid.uuid,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ,
                           NULL, NULL, NULL),
    BT_GATT_CCC(logradio_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

// ── Advertising ───────────────────────────────────────────────────────────────
//
// Use BLE 5.x TRUE extended advertising (BT_LE_ADV_OPT_EXT_ADV) via the
// bt_le_ext_adv_create API.  This is required because on nRF54L15 the legacy
// advertising paths (both 0x2006 and 0x2036-with-LEGACY-bit) produce non-
// connectable PDUs — a Zephyr SW-LL issue specific to this chip.  True extended
// advertising uses a completely different LLL path (lll_adv_aux.c, EXT_IND +
// AUX_ADV_IND) that correctly sets adv_mode=CONN.
//
// Note: bt_le_adv_start() explicitly rejects BT_LE_ADV_OPT_EXT_ADV (Zephyr
// valid_adv_param returns false for it).  Must use bt_le_ext_adv_create.

static struct bt_le_ext_adv *ext_adv_set = nullptr;

static void start_advertising()
{
    // FLAGS (3B) + UUID128 (18B) = 21B total.
    //
    // IMPORTANT: BT_DATA_BYTES() uses C99 compound literals that GCC C++ treats
    // as temporaries; with -Os the compiler elides the bt_data struct writes,
    // leaving the stack uninitialized.  Use static const arrays instead so the
    // values are placed in .rodata and the pointers are compile-time constants.
    static const uint8_t adv_flags_val[]  = { BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR };
    static const uint8_t adv_uuid128_val[] = { MESH_SVC_UUID_VAL };
    static const struct bt_data ad[2] = {
        { BT_DATA_FLAGS,      sizeof(adv_flags_val),  adv_flags_val  },
        { BT_DATA_UUID128_ALL, sizeof(adv_uuid128_val), adv_uuid128_val },
    };
    int err;

    if (!ext_adv_set) {
        // BT_LE_ADV_OPT_EXT_ADV = true extended advertising (ADV_EXT_IND + AUX_ADV_IND)
        // BT_LE_ADV_OPT_CONN    = connectable (includes _ONE_TIME: stops after connection)
        // BT_LE_ADV_OPT_USE_IDENTITY = use static random identity address
        err = bt_le_ext_adv_create(
            BT_LE_ADV_PARAM(BT_LE_ADV_OPT_EXT_ADV | BT_LE_ADV_OPT_CONN |
                            BT_LE_ADV_OPT_USE_IDENTITY,
                            BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL),
            NULL, &ext_adv_set);
        if (err) {
            LOG_WARN("BLE ext_adv_create failed: %d", err);
            return;
        }
    }

    err = bt_le_ext_adv_set_data(ext_adv_set, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_WARN("BLE ext_adv_set_data failed: %d", err);
        return;
    }

    struct bt_le_ext_adv_start_param start_param = {.timeout = 0, .num_events = 0};
    err = bt_le_ext_adv_start(ext_adv_set, &start_param);
    if (err == -EALREADY) {
        return;
    }
    if (err) {
        LOG_WARN("BLE adv start failed: %d", err);
    } else {
        LOG_INFO("BLE advertising as '%s'", bt_get_name());
    }
}

static void stop_advertising()
{
    if (ext_adv_set) {
        bt_le_ext_adv_stop(ext_adv_set);
    }
}

// ── Connection callbacks ──────────────────────────────────────────────────────

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_WARN("BLE connection failed, err=0x%02x", err);
        return;
    }

    k_mutex_lock(&ble_mutex, K_FOREVER);
    active_conn = bt_conn_ref(conn);
    k_mutex_unlock(&ble_mutex);

    memset(lastToRadio, 0, sizeof(lastToRadio));

    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INFO("BLE connected: %s", addr);

    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&newStatus);

    // For PIN modes, request pairing/encryption on the new connection
    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        if (bt_conn_set_security(conn, BT_SECURITY_L2) != 0) {
            LOG_WARN("BLE: bt_conn_set_security failed");
        }
    }
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    LOG_INFO("BLE disconnected, reason=0x%02x", reason);

    k_mutex_lock(&ble_mutex, K_FOREVER);
    if (active_conn) {
        bt_conn_unref(active_conn);
        active_conn = nullptr;
    }
    k_mutex_unlock(&ble_mutex);

    fromnum_ccc_val  = 0;
    logradio_ccc_val = 0;

    if (phoneAPI) {
        phoneAPI->close();
    }
    memset(lastToRadio, 0, sizeof(lastToRadio));

    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&newStatus);

    // Auto-resume advertising if we're still in the "enabled" state
    if (ble_enabled) {
        start_advertising();
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = connected_cb,
    .disconnected = disconnected_cb,
};

// ── Pairing / auth callbacks ──────────────────────────────────────────────────

static uint32_t configuredPasskey;

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    char passkey_str[7];
    snprintf(passkey_str, sizeof(passkey_str), "%06u", passkey);
    configuredPasskey = passkey;
    LOG_INFO("BLE pairing PIN: %s", passkey_str);
    powerFSM.trigger(EVENT_BLUETOOTH_PAIR);

    std::string textkey(passkey_str);
    meshtastic::BluetoothStatus pairingStatus(textkey);
    bluetoothStatus->updateStatus(&pairingStatus);
}

static void auth_cancel(struct bt_conn *conn)
{
    LOG_WARN("BLE pairing cancelled");
}

static struct bt_conn_auth_cb auth_cb = {
    .passkey_display = auth_passkey_display,
    .passkey_entry   = NULL,
    .cancel          = auth_cancel,
};

static void pairing_complete_cb(struct bt_conn *conn, bool bonded)
{
    LOG_INFO("BLE pairing complete, bonded=%d", (int)bonded);
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::CONNECTED);
    bluetoothStatus->updateStatus(&newStatus);
}

static void pairing_failed_cb(struct bt_conn *conn, enum bt_security_err reason)
{
    LOG_WARN("BLE pairing failed, reason=%d", (int)reason);
    meshtastic::BluetoothStatus newStatus(meshtastic::BluetoothStatus::ConnectionState::DISCONNECTED);
    bluetoothStatus->updateStatus(&newStatus);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
    .pairing_complete = pairing_complete_cb,
    .pairing_failed   = pairing_failed_cb,
};

// ── BluetoothPhoneAPI methods ─────────────────────────────────────────────────

void BluetoothPhoneAPI::onNowHasData(uint32_t fromRadioNum)
{
    PhoneAPI::onNowHasData(fromRadioNum);
    fromNumValue = fromRadioNum;

    if (fromnum_ccc_val & BT_GATT_CCC_NOTIFY) {
        bt_gatt_notify(active_conn, &mesh_svc.attrs[FROMNUM_ATTR_IDX],
                       &fromNumValue, sizeof(fromNumValue));
    }
}

bool BluetoothPhoneAPI::checkIsConnected()
{
    return active_conn != nullptr;
}

// ── BT stack pre-initializer (call from main thread before OSThreads start) ──
//
// bt_enable() requires substantially more stack than a Meshtastic OSThread
// (PowerFSMThread) provides — calling it there causes a stack overflow.
// Call this from nrf54l15Setup() (main Zephyr thread, CONFIG_MAIN_STACK_SIZE)
// so that by the time NRF54L15Bluetooth::setup() runs from PowerFSMThread,
// bt_initialized is already true and bt_enable() is skipped.

void nrf54l15_bt_preinit()
{
    if (!bt_initialized) {
        int err = bt_enable(NULL);
        if (err) {
            LOG_ERROR("BLE pre-init failed: %d", err);
            return;
        }
        bt_initialized = true;
        LOG_INFO("BLE stack pre-initialized on main thread");
    }
}

// ── NRF54L15Bluetooth public methods ─────────────────────────────────────────

void NRF54L15Bluetooth::setup()
{
    LOG_INFO("NRF54L15Bluetooth::setup()");

    if (!phoneAPI) {
        phoneAPI = new BluetoothPhoneAPI();
    }

    // Register auth callbacks before bt_enable so they're in place for first pairing
    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        bt_conn_auth_cb_register(&auth_cb);
        bt_conn_auth_info_cb_register(&auth_info_cb);
    }

    if (!bt_initialized) {
        int err = bt_enable(NULL);
        if (err) {
            LOG_ERROR("BLE enable failed: %d", err);
            return;
        }
        bt_initialized = true;
        LOG_INFO("BLE stack enabled");
    }

    const char *name = getDeviceName();
    bt_set_name(name);

    ble_enabled = true;
    start_advertising();
}

void NRF54L15Bluetooth::shutdown()
{
    LOG_INFO("NRF54L15Bluetooth::shutdown()");
    ble_enabled = false;
    stop_advertising();

    k_mutex_lock(&ble_mutex, K_FOREVER);
    struct bt_conn *conn = active_conn ? bt_conn_ref(active_conn) : nullptr;
    k_mutex_unlock(&ble_mutex);

    if (conn) {
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        bt_conn_unref(conn);
    }
}

void NRF54L15Bluetooth::startDisabled()
{
    // Initialize BT stack but do NOT advertise.
    // Do NOT call setup() — that calls start_advertising() internally.
    // Duplicate just the init portion of setup().
    if (!phoneAPI) {
        phoneAPI = new BluetoothPhoneAPI();
    }

    if (config.bluetooth.mode != meshtastic_Config_BluetoothConfig_PairingMode_NO_PIN) {
        bt_conn_auth_cb_register(&auth_cb);
        bt_conn_auth_info_cb_register(&auth_info_cb);
    }

    if (!bt_initialized) {
        int err = bt_enable(NULL);
        if (err) {
            LOG_ERROR("BLE enable failed: %d", err);
            return;
        }
        bt_initialized = true;
        LOG_INFO("BLE stack enabled");
    }

    const char *name = getDeviceName();
    bt_set_name(name);

    ble_enabled = false;
    LOG_INFO("BLE initialized, advertising stopped (startDisabled)");
}

void NRF54L15Bluetooth::resumeAdvertising()
{
    ble_enabled = true;
    start_advertising();
}

void NRF54L15Bluetooth::clearBonds()
{
    LOG_INFO("BLE clear bonds");
    bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
}

bool NRF54L15Bluetooth::isConnected()
{
    return active_conn != nullptr;
}

int NRF54L15Bluetooth::getRssi()
{
    return 0; // TODO: Zephyr has no direct bt_conn_get_rssi; use HCI RSSI read command
}

void NRF54L15Bluetooth::sendLog(const uint8_t *logMessage, size_t length)
{
    if (!active_conn || length > 512 || logradio_ccc_val == 0) {
        return;
    }
    // Send as notify regardless of whether client subscribed to NOTIFY or INDICATE —
    // bt_gatt_indicate() requires a params struct with a callback; notify is simpler
    // and the app accepts both. Change to indicate if compatibility issues arise.
    bt_gatt_notify(active_conn, &mesh_svc.attrs[LOGRADIO_ATTR_IDX],
                   logMessage, (uint16_t)length);
}
