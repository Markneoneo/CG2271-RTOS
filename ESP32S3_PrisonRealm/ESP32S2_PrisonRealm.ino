/*
 * PrisonRealm - ESP32-S2 Mini Main
 *
 * Sensors (local):
 *   DHT11  - Pin 7  (temperature)
 *
 * Comms:
 *   UART1 (Serial1) - Receives JSON from MCXC444
 *                     e.g. {"sensor":0, "value":1}
 *   WiFi + Supabase - Uploads all sensor events
 *
 * Supabase table schema:
 *   sensor_logs(id, sensor_type TEXT, value FLOAT, created_at TIMESTAMPTZ)
 */

#include <DHT.h>
#include <ESPSupabase.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "env.h"

#define DHT_PIN       7
#define DHTTYPE       DHT11

#define UART1_RX_PIN  18
#define UART1_TX_PIN  17
#define UART1_BAUD    9600

typedef enum {
    SENSOR_REED  = 0,
    SENSOR_TEMP  = 1,
    SENSOR_LOAD  = 2,
    SENSOR_SHOCK = 3
} SensorType;

typedef struct {
    SensorType sensor;
    float      value;
} TLogEntry;

Supabase      db;
DHT           dht(DHT_PIN, DHTTYPE);
QueueHandle_t uploadQueue;

void initGojo() {
    Serial.println("[Domain Expansion] Preparing Unlimited Void...");
    Serial.println("[Prison Realm] Seal interface detected.");
    Serial.println("[Gojo] 'Throughout Heaven and Earth, I alone am the honored one.'");
}

void initDHT() {
    dht.begin();
    Serial.println("[INIT] DHT11 ready on pin " + String(DHT_PIN));
}

void initUART1() {
    Serial1.begin(UART1_BAUD, SERIAL_8N1, UART1_RX_PIN, UART1_TX_PIN);
    Serial.println("[INIT] Serial1 (MCXC444) ready at " + String(UART1_BAUD)
                   + " baud | RX=" + String(UART1_RX_PIN)
                   + " TX=" + String(UART1_TX_PIN));
}

void initWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.disconnect(true);
    delay(1000);
    WiFi.begin(SSID, PASSWORD);
    Serial.printf("[INIT] Connecting to '%s'\n", SSID);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.printf("  status code: %d\n", WiFi.status());
        if (++attempts > 20) {
            Serial.println("[INIT] Failed to connect. Check SSID/password/band.");
            return;
        }
    }
    delay(500);
    Serial.printf("[INIT] WiFi connected: %s\n",
                  WiFi.localIP().toString().c_str());

    db.begin(SUPABASE_URL, SUPABASE_ANON);
    Serial.println("[INIT] Supabase initialised.");
}

static void uploadToSupabase(SensorType sensor, float value) {
    static const char* sensorNames[] = {
        "reed",
        "temperature",
    };

    JsonDocument doc;
    String       json;

    doc["sensor_type"] = sensorNames[(int)sensor];
    doc["value"]       = value;
    serializeJson(doc, json);

    Serial.printf("[UPLOAD] sensor=%s value=%.2f\n",
                  sensorNames[(int)sensor], value);

    int code = db.insert("sensor_logs", json, false);
    Serial.printf("[UPLOAD] Response: %d\n", code);
    db.urlQuery_reset();
}

// Task: Upload Consumer
static void uploadTask(void *pv) {
    TLogEntry entry;
    for (;;) {
        if (xQueueReceive(uploadQueue, &entry, portMAX_DELAY) == pdTRUE) {
            if (WiFi.status() == WL_CONNECTED) {
                uploadToSupabase(entry.sensor, entry.value);
            } else {
                Serial.println("[UPLOAD] WiFi disconnected - dropping entry.");
            }
        }
    }
}

// Task: UART Receiver (MCXC444 to ESP32)
static void uartRecvTask(void *pv) {
    String line = "";
    for (;;) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '\n') {
                line.trim();
                if (line.length() > 0) {
                    JsonDocument doc;
                    DeserializationError err = deserializeJson(doc, line);
                    if (!err) {
                        TLogEntry entry;
                        entry.sensor = (SensorType)doc["sensor"].as<int>();
                        entry.value  = doc["value"].as<float>();
                        Serial.printf("[UART] sensor=%d value=%.0f\n",
                                      (int)entry.sensor, entry.value);
                        xQueueSend(uploadQueue, &entry, 0);
                    } else {
                        Serial.printf("[UART] JSON parse error: %s | raw: %s\n",
                                      err.c_str(), line.c_str());
                    }
                }
                line = "";
            } else {
                line += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Task: Temperature (DHT11)
static void tempTask(void *pv) {
    for (;;) {
        float t = dht.readTemperature();
        if (!isnan(t)) {
            Serial.printf("[TEMP] %.1f C\n", t);
            TLogEntry entry = { SENSOR_TEMP, t };
            xQueueSend(uploadQueue, &entry, 0);
        } else {
            Serial.println("[TEMP] Read failed.");
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // every 10 s
    }
}

void setup() {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start < 3000));
    Serial.println("\n--- PrisonRealm ESP32 Starting ---");

    initGojo();
    initUART1();
    initDHT();
    initWiFi();

    uploadQueue = xQueueCreate(20, sizeof(TLogEntry));

    xTaskCreate(uploadTask,   "Upload",  8192,  NULL,  1,  NULL);
    xTaskCreate(uartRecvTask, "UART_Rx", 4096,  NULL,  2,  NULL);
    xTaskCreate(tempTask,     "Temp",    4096,  NULL,  2,  NULL);

    Serial.println("[INIT] All tasks started.\n");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
