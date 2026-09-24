// BLE client — runs on the pedalboard unit (XIAO ESP32-C3)
// Scans for the guitar/server, connects, subscribes to every control,
// and forwards a combined snapshot to the Daisy Seed over UART.
//
// Wiring (client side):
//   D6  / GPIO21  ESP_TX        -> Daisy USART1_RX (pin 15)
//   D7  / GPIO20  ESP_RX        <- Daisy USART1_TX (pin 14)
//   D10 / GPIO10  BLUETOOTH_LED -> on while connected to the server
//   shared GND between the ESP32 and the Daisy Seed

#include <NimBLEDevice.h>

// ── UART link to the Daisy Seed ───────────────────────────
static const uint8_t  UART_TX_PIN = D6;   // GPIO21 -> Daisy RX
static const uint8_t  UART_RX_PIN = D7;   // GPIO20 <- Daisy TX
static const uint32_t UART_BAUD   = 115200;
#define DaisySerial Serial1

// ── Connection-status LED ─────────────────────────────────
static const uint8_t  LED_PIN = D10;      // on = connected to server

// ── Match SERVICE_UUID, DEVICE_NAME and characteristic UUIDs to the server ──
static const char* SERVICE_UUID = "a1b2c3d4-0001-0000-0000-000000000000";
static const char* DEVICE_NAME  = "Pedal";

// Order here defines the index used in the UART packet below.
enum ControlIndex { KNOB1, KNOB2, LDR, JOY_X, JOY_Y, SWITCH, NUM_CONTROLS };

struct Control {
    const char*                 name;
    const char*                 uuid;
    NimBLERemoteCharacteristic* characteristic;
};

static Control controls[NUM_CONTROLS] = {
    { "Knob1",  "a1b2c3d4-0001-0000-0000-000000000001", nullptr },
    { "Knob2",  "a1b2c3d4-0001-0000-0000-000000000002", nullptr },
    { "LDR",    "a1b2c3d4-0001-0000-0000-000000000003", nullptr },
    { "JoyX",   "a1b2c3d4-0001-0000-0000-000000000004", nullptr },
    { "JoyY",   "a1b2c3d4-0001-0000-0000-000000000005", nullptr },
    { "Switch", "a1b2c3d4-0001-0000-0000-000000000006", nullptr },
};

// Latest value per control: 0–4095 for the analog inputs, 0/1 for the switch.
static volatile uint16_t values[NUM_CONTROLS] = { 0 };
static volatile bool     dirty = false;   // a value changed and needs forwarding

// ── State ─────────────────────────────────────────────────
static NimBLEClient*  bleClient = nullptr;
static NimBLEAddress  serverAddress;
static bool           connected = false;
static bool           doConnect = false;

// ── UART packet to the Daisy ──────────────────────────────
// Fixed 14-byte frame, sent whenever any control changes:
//   [0]      0xAA   sync
//   [1]      0x55   sync
//   [2..3]   Knob1  uint16 little-endian
//   [4..5]   Knob2
//   [6..7]   LDR
//   [8..9]   JoyX
//   [10..11] JoyY
//   [12]     Switch (0/1)
//   [13]     checksum = XOR of bytes [2..12]
void sendToDaisy() {
    uint8_t pkt[14];
    pkt[0] = 0xAA;
    pkt[1] = 0x55;
    for (int i = 0; i < 5; i++) {
        uint16_t v = values[i];
        pkt[2 + i * 2] = v & 0xFF;
        pkt[3 + i * 2] = v >> 8;
    }
    pkt[12] = values[SWITCH] ? 1 : 0;
    uint8_t cksum = 0;
    for (int i = 2; i <= 12; i++) cksum ^= pkt[i];
    pkt[13] = cksum;
    DaisySerial.write(pkt, sizeof(pkt));
}

// ── Notification callback ─────────────────────────────────
void notifyCallback(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
    if (length < 2) return;
    uint16_t raw = pData[0] | (pData[1] << 8);

    for (int i = 0; i < NUM_CONTROLS; i++) {
        if (controls[i].characteristic == pChar) {
            values[i] = raw;
            dirty = true;
            break;
        }
    }
}

// ── BLE callbacks ─────────────────────────────────────────
class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override {
        Serial.println("Connected to server");
    }
    void onDisconnect(NimBLEClient*, int reason) override {
        connected = false;
        Serial.printf("Disconnected (reason %d) — scanning again\n", reason);
        NimBLEDevice::getScan()->start(0);
    }
};

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* device) override {
        std::string name = device->getName();
        bool        bySvc  = device->isAdvertisingService(NimBLEUUID(SERVICE_UUID));
        bool        byName = (name == DEVICE_NAME);

        if (!name.empty() || bySvc) {
            Serial.printf("Saw %s  name='%s'  svc=%d\n",
                          device->getAddress().toString().c_str(), name.c_str(), bySvc);
        }

        if (bySvc || byName) {
            Serial.printf("Found server: %s\n", device->getAddress().toString().c_str());
            NimBLEDevice::getScan()->stop();
            serverAddress = device->getAddress();
            doConnect = true;
        }
    }
};

// ── Connection logic ──────────────────────────────────────
bool connectToServer() {
    bleClient = NimBLEDevice::createClient();
    bleClient->setClientCallbacks(new ClientCallbacks());

    if (!bleClient->connect(serverAddress)) {
        Serial.println("Connection failed");
        NimBLEDevice::deleteClient(bleClient);
        bleClient = nullptr;
        return false;
    }

    NimBLERemoteService* service = bleClient->getService(SERVICE_UUID);
    if (!service) {
        Serial.println("Service not found on server");
        bleClient->disconnect();
        return false;
    }

    for (int i = 0; i < NUM_CONTROLS; i++) {
        Control& c = controls[i];
        c.characteristic = service->getCharacteristic(c.uuid);
        if (!c.characteristic) {
            Serial.printf("Characteristic not found: %s\n", c.name);
            continue;
        }

        if (c.characteristic->canRead()) {
            values[i] = c.characteristic->readValue<uint16_t>();
        }

        if (c.characteristic->canNotify()) {
            c.characteristic->subscribe(true, notifyCallback);
        }
    }

    connected = true;       
    dirty = true;          
    return true;
}

// Connection LED: solid when connected, blinks ~2 Hz while scanning/connecting.
void updateConnLed() {
    digitalWrite(LED_PIN, connected ? HIGH : ((millis() / 250) % 2));
}

// ── Arduino entry points ──────────────────────────────────
void setup() {
    Serial.begin(115200);
    DaisySerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    NimBLEDevice::init("");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(new ScanCallbacks());
    scan->setActiveScan(true);
    scan->start(0);  // scan indefinitely until the server is found

    Serial.println("Scanning for server...");
}

void loop() {
    updateConnLed();

    if (doConnect) {
        doConnect = false;
        if (connectToServer()) {
            Serial.println("Subscribed — ready");
        } else {
            NimBLEDevice::getScan()->start(0);
        }
    }

    if (dirty) {
        dirty = false;
        sendToDaisy();
    }

    static uint32_t lastPrint = 0;
    if (connected && millis() - lastPrint > 500) {
        lastPrint = millis();
        Serial.printf("K1=%u K2=%u LDR=%u JX=%u JY=%u SW=%u\n",
                      values[KNOB1], values[KNOB2], values[LDR],
                      values[JOY_X], values[JOY_Y], values[SWITCH]);
    }
}
