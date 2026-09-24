// BLE server — runs on the guitar/handheld unit (XIAO ESP32-C3)
// Reads the controls and notifies the connected client on change.
//
// Control sources:
//   Knob1 / Knob2 / LDR  -> MCP3208 external ADC (CH0 / CH1 / CH2) over SPI
//   Joystick X / Y       -> XIAO on-board ADC (D1 / D2)
//   Switch               -> joystick push-button (D3, digital, active-low)
//
// Pin map (XIAO ESP32-C3, server side):
//   D0  / GPIO2   A0           battery sense (via divider — see BATT_* below)
//   D1  / GPIO3   Joystick X   (ADC)
//   D2  / GPIO4   Joystick Y   (ADC)
//   D3  / GPIO5   Switch       (digital, INPUT_PULLUP)
//   D4  / GPIO6   BT_LED       connection status (blinks advertising, solid connected)
//   D5  / GPIO7   P_LED        battery status (brightness = %, blinks when low)
//   D6  / GPIO21  CS/SHDN      MCP3208 chip select
//   D8  / GPIO8   SCK          MCP3208 CLK
//   D9  / GPIO9   MISO         MCP3208 Dout
//   D10 / GPIO10  MOSI         MCP3208 Din

#include <NimBLEDevice.h>
#include <SPI.h>
#include "esp_system.h" 

// ── MCP3208 SPI pins ──────────────────────────────────────
static const uint8_t PIN_SCK  = D8;
static const uint8_t PIN_MISO = D9;
static const uint8_t PIN_MOSI = D10;
static const uint8_t PIN_CS   = D6;

static const uint32_t MCP_SPI_HZ = 1000000;

// ── Status LEDs ───────────────────────────────────────────
static const uint8_t BATT_LED_PIN = D5;   // battery status
static const uint8_t CONN_LED_PIN = D4;   // client connected

// ── Battery sense (R1/R2 = 220k/220k divider into A0) ─────
static const uint8_t BATT_ADC_PIN = D0;
static const float   BATT_DIVIDER = 2.0f;   // (R1 + R2) / R2 = (220k + 220k) / 220k
static const float   BATT_FULL_V  = 4.20f;  // 1S Li-Ion full  -> 100%
static const float   BATT_EMPTY_V = 3.30f;  // 1S Li-Ion empty -> 0%
static const float   BATT_LOW_V   = 3.45f;  // below this: blink instead of dim

// ── Control definitions ───────────────────────────────────
enum Source { SRC_MCP, SRC_ADC, SRC_DIGITAL };

struct Control {
    const char*           name;
    Source                source;
    uint8_t               channel;   // MCP channel for SRC_MCP, else GPIO pin
    const char*           uuid;
    NimBLECharacteristic* characteristic;
    uint16_t              lastValue;
};

static Control controls[] = {
    { "Knob1",  SRC_MCP,     0,  "a1b2c3d4-0001-0000-0000-000000000001", nullptr, 0 },
    { "Knob2",  SRC_MCP,     1,  "a1b2c3d4-0001-0000-0000-000000000002", nullptr, 0 },
    { "LDR",    SRC_MCP,     2,  "a1b2c3d4-0001-0000-0000-000000000003", nullptr, 0 },
    { "JoyX",   SRC_ADC,     D1, "a1b2c3d4-0001-0000-0000-000000000004", nullptr, 0 },
    { "JoyY",   SRC_ADC,     D2, "a1b2c3d4-0001-0000-0000-000000000005", nullptr, 0 },
    { "Switch", SRC_DIGITAL, D3, "a1b2c3d4-0001-0000-0000-000000000006", nullptr, 0 },
};
static const uint8_t NUM_CONTROLS = sizeof(controls) / sizeof(controls[0]);

// ── BLE config ────────────────────────────────────────────
static const char* SERVICE_UUID = "a1b2c3d4-0001-0000-0000-000000000000";
static const char* DEVICE_NAME  = "Pedal";

// ── Tuning ────────────────────────────────────────────────
static const uint16_t ADC_DEADBAND = 12;  // ignore analog noise smaller than this (0–4095)
static const uint32_t SAMPLE_MS    = 20;  // 50 Hz sample rate

// ── State ─────────────────────────────────────────────────
static NimBLEServer* bleServer = nullptr;
static bool          clientConnected = false;

// ── MCP3208 single-ended read (12-bit, 0–4095) ────────────
uint16_t readMCP3208(uint8_t channel) {
    SPI.beginTransaction(SPISettings(MCP_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    uint8_t cmd = 0b00000110 | ((channel & 0x04) >> 2); 
    SPI.transfer(cmd);
    uint8_t hi = SPI.transfer((channel & 0x03) << 6);   
    uint8_t lo = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
    return ((hi & 0x0F) << 8) | lo;
}

uint16_t readControl(const Control& c) {
    switch (c.source) {
        case SRC_MCP:     return readMCP3208(c.channel);
        case SRC_ADC:     return analogRead(c.channel);        
        case SRC_DIGITAL: return digitalRead(c.channel) ? 0 : 1;
    }
    return 0;
}

static float readBatteryVolts() {
    return (analogReadMilliVolts(BATT_ADC_PIN) / 1000.0f) * BATT_DIVIDER;
}

static int batteryPercent(float volts) {
    int pct = (int)((volts - BATT_EMPTY_V) / (BATT_FULL_V - BATT_EMPTY_V) * 100.0f);
    return constrain(pct, 0, 100);
}

// ── Battery status LED ────────────────────────────────────
// Brightness tracks battery
void updateBatteryLed() {
    static uint32_t lastCheck = 0;
    static float    volts     = BATT_FULL_V;

    if (millis() - lastCheck > 1000) {
        lastCheck = millis();
        volts = readBatteryVolts();
        Serial.printf("Battery: %.2f V (%d%%)%s\n",
                      volts, batteryPercent(volts), volts < BATT_LOW_V ? " LOW" : "");
    }

    if (volts < BATT_LOW_V)
        analogWrite(BATT_LED_PIN, ((millis() / 250) % 2) ? 255 : 0);  // blink ~2 Hz
    else
        analogWrite(BATT_LED_PIN, map(batteryPercent(volts), 0, 100, 0, 255));
}

// Connection LED: solid when a client is connected, blinks ~2 Hz while advertising.
void updateConnLed() {
    digitalWrite(CONN_LED_PIN, clientConnected ? HIGH : ((millis() / 250) % 2));
}

// ── BLE callbacks ─────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        clientConnected = true;   // LED handling is in updateConnLed()
        Serial.println("Client connected");
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        clientConnected = false;
        Serial.println("Client disconnected — restarting advertising");
        NimBLEDevice::startAdvertising();
    }
};

void bootBlink() {
    for (int i = 0; i < 3; i++) {
        digitalWrite(BATT_LED_PIN, HIGH);
        digitalWrite(CONN_LED_PIN, HIGH);
        delay(120);
        digitalWrite(BATT_LED_PIN, LOW);
        digitalWrite(CONN_LED_PIN, LOW);
        delay(120);
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n=== Pedal server boot === reset_reason=%d %s\n",
                  esp_reset_reason(),
                  esp_reset_reason() == ESP_RST_BROWNOUT ? "(BROWNOUT! supply sagging)" : "");

    pinMode(BATT_LED_PIN, OUTPUT);
    pinMode(CONN_LED_PIN, OUTPUT);
    digitalWrite(BATT_LED_PIN, LOW);
    digitalWrite(CONN_LED_PIN, LOW);
    bootBlink(); 

    // Control inputs
    for (auto& c : controls) {
        if (c.source == SRC_ADC)          pinMode(c.channel, INPUT);
        else if (c.source == SRC_DIGITAL) pinMode(c.channel, INPUT_PULLUP);
    }

    // MCP3208 SPI
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    Serial.println("[1] GPIO + SPI ready");

    Serial.printf("[2] Battery (pre-BLE): %.2f V\n", readBatteryVolts());

    Serial.println("[3] starting BLE radio...");
    NimBLEDevice::init(DEVICE_NAME);
    bleServer = NimBLEDevice::createServer();
    bleServer->setCallbacks(new ServerCallbacks());

    NimBLEService* service = bleServer->createService(SERVICE_UUID);

    for (auto& c : controls) {
        c.characteristic = service->createCharacteristic(
            c.uuid,
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
        );
        uint16_t initial = readControl(c);
        c.characteristic->setValue(initial);
        c.lastValue = initial;
    }

    service->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SERVICE_UUID);
    adv->setName(DEVICE_NAME);       
    adv->enableScanResponse(true); 
    bool advOk = adv->start();

    Serial.printf("[4] Advertising as \"%s\" with %u control(s) — start=%s\n",
                  DEVICE_NAME, NUM_CONTROLS, advOk ? "OK" : "FAILED");
}

void loop() {
    updateBatteryLed();
    updateConnLed();

    static uint32_t lastSample = 0;
    if (millis() - lastSample < SAMPLE_MS) return;
    lastSample = millis();

    for (auto& c : controls) {
        uint16_t val = readControl(c);

        bool changed = (c.source == SRC_DIGITAL)
                           ? (val != c.lastValue)
                           : (abs((int)val - (int)c.lastValue) > ADC_DEADBAND);
        if (!changed) continue;

        c.lastValue = val;
        c.characteristic->setValue(val);
        if (clientConnected) {
            c.characteristic->notify();
        }
        // Serial.printf("%s: %u\n", c.name, val);
    }
}
