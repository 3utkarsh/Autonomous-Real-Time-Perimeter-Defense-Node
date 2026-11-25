#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>

// =============================================================
// 1. NETWORK & MQTT CONFIGURATION (Update these values)
// =============================================================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MQTT_BROKER   = "broker.hivemq.com"; // Public test broker or local IP
const int   MQTT_PORT     = 1883;
const char* MQTT_CLIENT   = "ESP32S3_Perimeter_Node_01";

// MQTT Topics
const char* TOPIC_ALERT     = "perimeter/node01/alert";     // Publishes instant breach JSON
const char* TOPIC_TELEMETRY = "perimeter/node01/telemetry"; // Publishes periodic ADC & baseline
const char* TOPIC_COMMAND   = "perimeter/node01/command";   // Subscribes to "RESET" / "CALIBRATE"

// =============================================================
// 2. PIN MAPPING (Laser + LDR Only)
// =============================================================
#define PIN_LDR_ADC    4   // HW-486 Signal (S)
#define PIN_LASER_PWR  6   // HW-493 Signal (S / +)

// =============================================================
// 3. SHARED VOLATILE STATE ACROSS CORES
// =============================================================
volatile bool     g_beam_tripped     = false;
volatile uint32_t g_trip_timestamp   = 0;
volatile int32_t  g_current_adc      = 0;
volatile int32_t  g_ambient_baseline = 2048;
volatile uint32_t g_trip_count       = 0;
volatile bool     g_recalibrate_req  = false;

const int32_t BEAM_BREAK_DELTA = 600;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
TaskHandle_t TaskOpticalSamplerHandle;

// =============================================================
// CORE 0: Deterministic 1 kHz Optical Sensing & Baseline Task
// =============================================================
void OpticalSamplerTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1); // Exact 1 ms interval

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);

        int32_t raw_val = analogRead(PIN_LDR_ADC);
        g_current_adc = raw_val;

        // Handle remote recalibration request safely on Core 0
        if (g_recalibrate_req) {
            g_ambient_baseline = raw_val;
            g_recalibrate_req = false;
            continue;
        }

        // Adaptive Baseline Filter (1-cycle bit-shift leaky integrator)
        g_ambient_baseline = g_ambient_baseline + ((raw_val - g_ambient_baseline) >> 7);

        // Sudden optical drop check
        int32_t delta = abs(raw_val - g_ambient_baseline);
        if (delta > BEAM_BREAK_DELTA && !g_beam_tripped) {
            g_beam_tripped = true;
            g_trip_timestamp = millis();
            g_trip_count++;
        }
    }
}

// =============================================================
// MQTT Incoming Command Handler (Runs on Core 1)
// =============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }
    msg.trim();
    msg.toUpperCase();

    if (String(topic) == TOPIC_COMMAND) {
        if (msg == "RESET") {
            portDISABLE_INTERRUPTS();
            g_beam_tripped = false;
            portENABLE_INTERRUPTS();
            mqttClient.publish(TOPIC_ALERT, "{\"status\":\"ARMED\",\"event\":\"REMOTE_RESET\"}", true);
            Serial.println("[MQTT] Perimeter reset and re-armed.");
        } 
        else if (msg == "CALIBRATE") {
            g_recalibrate_req = true;
            portDISABLE_INTERRUPTS();
            g_beam_tripped = false;
            portENABLE_INTERRUPTS();
            Serial.println("[MQTT] Baseline recalibrated.");
        }
    }
}

// =============================================================
// Wi-Fi & MQTT Reconnection Helpers
// =============================================================
void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    Serial.printf("\n[WIFI] Connected | IP: %s\n", WiFi.localIP().toString().c_str());
}

void reconnectMQTT() {
    while (!mqttClient.connected()) {
        Serial.print("[MQTT] Connecting to broker...");
        // Last Will & Testament (LWT) alerts broker if node goes offline unexpectedly
        if (mqttClient.connect(MQTT_CLIENT, TOPIC_ALERT, 1, true, "{\"status\":\"OFFLINE\"}")) {
            Serial.println(" Connected!");
            mqttClient.subscribe(TOPIC_COMMAND);
            mqttClient.publish(TOPIC_ALERT, "{\"status\":\"ARMED\",\"event\":\"BOOT\"}", true);
        } else {
            Serial.printf(" Failed (rc=%d). Retrying in 2s...\n", mqttClient.state());
            delay(2000);
        }
    }
}

// =============================================================
// SETUP (Core 1)
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // 1. Power Laser ON
    pinMode(PIN_LASER_PWR, OUTPUT);
    digitalWrite(PIN_LASER_PWR, HIGH);

    // 2. Configure 12-bit ADC
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_LDR_ADC, ADC_11db);

    // 3. Calibrate Initial Ambient Baseline (500 ms)
    int32_t acc = 0;
    for (int i = 0; i < 50; i++) {
        acc += analogRead(PIN_LDR_ADC);
        delay(10);
    }
    g_ambient_baseline = acc / 50;
    Serial.printf("[OPTICAL] Calibrated Baseline: %d\n", g_ambient_baseline);

    // 4. Launch Core 0 Real-Time Optical Task BEFORE Network Init
    xTaskCreatePinnedToCore(
        OpticalSamplerTask,
        "OpticalSampler",
        4096,
        NULL,
        2,
        &TaskOpticalSamplerHandle,
        0 // Core 0
    );

    // 5. Connect Wi-Fi & Configure MQTT
    setupWiFi();
    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(mqttCallback);
}

// =============================================================
// MAIN LOOP (Core 1: MQTT Telemetry & Breach Dispatch)
// =============================================================
void loop() {
    static bool alert_published = false;
    static uint32_t last_telemetry_ms = 0;

    if (WiFi.status() != WL_CONNECTED) {
        setupWiFi();
    }
    if (!mqttClient.connected()) {
        reconnectMQTT();
    }
    mqttClient.loop();

    // Read trip state atomically
    bool tripped = false;
    portDISABLE_INTERRUPTS();
    tripped = g_beam_tripped;
    portENABLE_INTERRUPTS();

    // 1. Immediate Breach Alert Publish
    if (tripped && !alert_published) {
        alert_published = true;

        char alertPayload[160];
        snprintf(alertPayload, sizeof(alertPayload),
                 "{\"status\":\"BREACHED\",\"adc\":%d,\"baseline\":%d,\"breach_count\":%u,\"uptime_ms\":%u}",
                 g_current_adc, g_ambient_baseline, g_trip_count, g_trip_timestamp);

        mqttClient.publish(TOPIC_ALERT, alertPayload, true); // Retained alert
        Serial.printf("[ALERT SENT] %s\n", alertPayload);
    }

    // Reset local publish latch once cleared via MQTT "RESET" command
    if (!tripped && alert_published) {
        alert_published = false;
    }

    // 2. Periodic Heartbeat Telemetry (Every 2 Seconds)
    if (millis() - last_telemetry_ms >= 2000) {
        last_telemetry_ms = millis();

        char telemetryPayload[140];
        snprintf(telemetryPayload, sizeof(telemetryPayload),
                 "{\"armed\":%s,\"adc\":%d,\"baseline\":%d,\"breaches\":%u}",
                 tripped ? "false" : "true", g_current_adc, g_ambient_baseline, g_trip_count);

        mqttClient.publish(TOPIC_TELEMETRY, telemetryPayload);
        Serial.printf("[TELEMETRY] %s\n", telemetryPayload);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
}
