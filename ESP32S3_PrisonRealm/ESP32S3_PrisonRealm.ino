/*
 * PrisonRealm - ESP32-S3
 *
 * Sensors (local):
 *   DHT11  - Pin 7  (temperature)
 *
 * Sensors (via MCXC444 over UART):
 *   Reed switch  - SENSOR_REED  = 0
 *   Load cell    - SENSOR_LOAD  = 2
 *   Shock sensor - SENSOR_SHOCK = 3
 *
 * Comms:
 *   UART1 (Serial1) - Bidirectional with MCXC444
 *                     RX: {"sensor":0, "value":1}
 *                     TX: {"command":"lock"} or {"command":"unlock"}
 *   WiFi + Supabase - Uploads sensor events, polls commands table
 *
 * Supabase tables:
 *   sensor_logs(id, sensor_type TEXT, value FLOAT, created_at TIMESTAMPTZ)
 *   commands(id, command TEXT, executed BOOL, created_at TIMESTAMPTZ)
 */

#include <DHT.h>
#include <ESPSupabase.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "env.h"

#include "../common/protocol.h"

#define DHT_PIN          7
#define DHTTYPE          DHT11

#define UART1_RX_PIN     18
#define UART1_TX_PIN     17
#define UART1_BAUD       9600

#define COMMAND_POLL_MS  2000

typedef struct {
    char command[32];
    int  commandId;
} TCommand;

Supabase          db;
DHT               dht(DHT_PIN, DHTTYPE);
QueueHandle_t     uploadQueue;
QueueHandle_t     commandQueue;
SemaphoreHandle_t dbMutex;   // Protects all Supabase calls

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

    // db.begin() called ONCE and only ONCE
    db.begin(SUPABASE_URL, SUPABASE_ANON);
    Serial.println("[INIT] Supabase initialised.");
}

static void uploadToSupabase(SensorType sensor, float value) {
    static const char* sensorNames[] = {
        "reed",         // SENSOR_REED  = 0
        "temperature",  // SENSOR_TEMP  = 1
        "load",         // SENSOR_LOAD  = 2
        "shock",        // SENSOR_SHOCK = 3
    };

    StaticJsonDocument<256> doc;
    String json;

    doc["sensor_type"] = sensorNames[(int)sensor];
    doc["value"]       = value;
    serializeJson(doc, json);

    Serial.printf("[UPLOAD] sensor=%s value=%.2f\n",
                  sensorNames[(int)sensor], value);

    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        int code = db.insert("sensor_logs", json, false);
        Serial.printf("[UPLOAD] Response: %d\n", code);
        db.urlQuery_reset();
        xSemaphoreGive(dbMutex);
    } else {
        Serial.println("[UPLOAD] Could not acquire db mutex - skipping.");
    }
}

static void markCommandExecuted(int commandId) {
    StaticJsonDocument<64> doc;  // Stack allocated
    String json;

    doc["executed"] = true;
    serializeJson(doc, json);

    String idStr = String(commandId);

    if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        int code = db.update("commands")
                     .eq("id", idStr)
                     .doUpdate(json);
        Serial.printf("[CMD] Marked id=%d executed. Code: %d\n", commandId, code);
        db.urlQuery_reset();
        xSemaphoreGive(dbMutex);
    } else {
        Serial.println("[CMD] Could not acquire db mutex - skipping mark.");
    }
}

// Task: Upload Consumer 
static void uploadTask(void *pv) {
    TSensorData entry;
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

// Task: Command Poll (Supabase -> MCXC444)
static void commandPollTask(void *pv) {
    for (;;) {
        //Serial.printf("[CMD POLL] WiFi status: %d\n", WiFi.status());
        if (WiFi.status() == WL_CONNECTED) {
            String response = "";

            if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
                response = db.from("commands")
                             .select("*")
                             .eq("executed", "false")
                             .order("created_at", "asc", true)
                             .limit(1)
                             .doSelect();
                db.urlQuery_reset();
                xSemaphoreGive(dbMutex);
            } else {
                Serial.println("[CMD POLL] Could not acquire db mutex - skipping.");
            }

            if (response.length() > 2) {
                StaticJsonDocument<512> doc;  // Stack allocated, larger for query response
                DeserializationError err = deserializeJson(doc, response);
                if (!err && doc.is<JsonArray>() && doc.as<JsonArray>().size() > 0) {
                    JsonObject row = doc[0];
                    TCommand cmd;
                    strncpy(cmd.command, row["command"] | "unknown", sizeof(cmd.command));
                    cmd.commandId = row["id"] | 0;

                    Serial.printf("[CMD] New command: '%s' (id=%d)\n",
                                  cmd.command, cmd.commandId);

                    xQueueSend(commandQueue, &cmd, 0);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(COMMAND_POLL_MS));
    }
}

// Task: UART Send (ESP32 -> MCXC444)
static void uartSendTask(void *pv) {
    TCommand cmd;
    for (;;) {
        if (xQueueReceive(commandQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            StaticJsonDocument<128> doc;  // Stack allocated
            String json;
            doc["command"] = cmd.command;
            serializeJson(doc, json);
            json += "\n";

            Serial1.print(json);
            Serial.printf("[UART TX] Sent: %s", json.c_str());

            if (WiFi.status() == WL_CONNECTED) {
                markCommandExecuted(cmd.commandId);
            }
        }
    }
}

// Task: UART Receiver (MCXC444 -> ESP32)
static void uartRecvTask(void *pv) {
    String line = "";
    for (;;) {
        while (Serial1.available()) {
            char c = (char)Serial1.read();
            if (c == '\n') {
                line.trim();
                if (line.length() > 0) {
                    StaticJsonDocument<256> doc;  // Stack allocated
                    DeserializationError err = deserializeJson(doc, line);
                    if (!err) {
                        TSensorData entry;
                        entry.sensor = (SensorType)doc["sensor"].as<int>();
                        entry.value  = doc["value"].as<float>();
                        Serial.printf("[UART RX] sensor=%d value=%.2f\n",
                                      (int)entry.sensor, entry.value);
                        xQueueSend(uploadQueue, &entry, 0);
                    } else {
                        Serial.printf("[UART RX] JSON parse error: %s | raw: %s\n",
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
            TSensorData entry = { SENSOR_TEMP, t };
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

    uploadQueue  = xQueueCreate(20, sizeof(TSensorData));
    commandQueue = xQueueCreate(10, sizeof(TCommand));
    dbMutex      = xSemaphoreCreateMutex();

    xTaskCreate(uploadTask,      "Upload",   12288, NULL,  1,  NULL);
    xTaskCreate(uartRecvTask,    "UART_Rx",  8192,  NULL,  2,  NULL);
    xTaskCreate(uartSendTask,    "UART_Tx",  8192,  NULL,  2,  NULL);
    xTaskCreate(tempTask,        "Temp",     8192,  NULL,  2,  NULL);
    xTaskCreate(commandPollTask, "CmdPoll",  12288, NULL,  1,  NULL);

    Serial.println("[INIT] All tasks started.\n");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
