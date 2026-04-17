/*
 * PrisonRealm - ESP32-S3
 *
 * Sensors (local):
 *   DHT11  - Pin 7  (temperature)
 *
 * Sensors (via MCXC444 over UART):
 *   Reed switch  - SENSOR_REED  = 0
 *   Temp sensor  - SENSOR_TEMP  = 1
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
#include <Keypad.h>

#include "../common/protocol.h"

// DHT11 (Temp and humidity sensor)
#define DHT_PIN 7
#define DHTTYPE DHT11
#define TEMP_TASK_DELAY 10000  //10s

// UART1 (Link to MCXC444)
#define UART1_RX_PIN 18
#define UART1_TX_PIN 17
#define UART1_BAUD 115200

// Supabase command polling
#define COMMAND_POLL_MS 2000
#define CMD_LOCK_STR "lock"
#define CMD_UNLOCK_STR "unlock"

// Buzzer for keypress feedback
#define BUZZER_PIN 9
#define BUZZER_DURATION 50          // How long to buzz for in ms
#define BUZZER_VOLUME 80            // Volume as percentage
#define BUZZER_BASE_FREQUENCY 1000  // Base buzzer frequency
#define BUZZER_DELTA 300            // Final freq = random * delta + base

// Command Struct
// Pairs a command type with the Supabase row ID so it can be marked executed later
typedef struct {
  CommandType command;
  int commandId;
} TCommand;

Supabase db;
DHT dht(DHT_PIN, DHTTYPE);

// FreeRTOS queues
QueueHandle_t uploadQueue; // Waiting to be uploaded to sensor_logs
QueueHandle_t commandQueue; // TCommand items waiting to be sent to MCXC444 over UART
QueueHandle_t stateQueue;   // State data to upload to Supabase
SemaphoreHandle_t dbMutex;  // Protects all Supabase calls
TimerHandle_t buzzerTimer;

// Keypad setup
const byte ROWS = 4;
const byte COLS = 3;

char keys[ROWS][COLS] = {
  { '1', '2', '3' },
  { '4', '5', '6' },
  { '7', '8', '9' },
  { '*', '0', '#' }
};

byte rowPins[ROWS] = { 35, 36, 37, 38 };  // R1-R4
byte colPins[COLS] = { 39, 40, 41 };      // C1-C3

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Even while sealed in the Prison Realm, Satoru Gojo provides
// system diagnostics, morale support, and domain-level debugging.
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

// Connect to WiFi
// initialise SUpabase client
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


// Buzzer Helpers
// 
// Drive buzzer at a randomised frequency
// Called on every keypress
static void setBuzzer(int volume) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  int dutyCycle = (float)volume / 100 * 255;
  int frequency = ((rand() % 3) * BUZZER_DELTA) + BUZZER_BASE_FREQUENCY;
  analogWrite(BUZZER_PIN, dutyCycle);
  analogWriteFrequency(BUZZER_PIN, frequency);
}

// Silence buzzer
static void clearBuzzer() {
  analogWrite(BUZZER_PIN, 0);
}

void buzzerTimerCallback(TimerHandle_t xTimer) {
  clearBuzzer();
}

void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerTimer = xTimerCreate("Buzzer Timer", pdMS_TO_TICKS(BUZZER_DURATION), pdFALSE, (void *)0, buzzerTimerCallback);
}

// Supabase helpers
//
// Serialise a TSensorData reading to JSON and INSERT it into sensor_logs
// Takes the dbMutex before calling client to prevent concurrent access
static void uploadSensorToSupabase(TSensorData entry) {
  static const char *sensorNames[] = {
    "reed",         // SENSOR_REED  = 0
    "temperature",  // SENSOR_TEMP  = 1
    "load",         // SENSOR_LOAD  = 2
    "shock",        // SENSOR_SHOCK = 3
  };

  // stack allocated JSON buffer
  StaticJsonDocument<256> doc;
  String json;

  doc["sensor_type"] = sensorNames[(int)entry.sensor];
  doc["value"] = entry.value;
  serializeJson(doc, json);

  Serial.printf("[UPLOAD] sensor=%s value=%.2f\n",
                sensorNames[(int)entry.sensor], entry.value);

  if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    int code = db.insert("sensor_logs", json, false);
    Serial.printf("[UPLOAD] Response: %d\n", code);
    db.urlQuery_reset();
    xSemaphoreGive(dbMutex);
  } else {
    Serial.println("[UPLOAD] Could not acquire db mutex - skipping.");
  }
}

// Serialise PrisonRealm state snapshot to JSON and INSERT it into the state table
// Record whether the door is locked/opened and whether the alarm is active
static void uploadStateToSupabase(SystemState state) {
  StaticJsonDocument<256> doc;
  String json;

  doc["is_door_locked"] = (state.lock == LOCKED);
  doc["is_door_open"] = (state.door == OPEN);
  doc["is_alarm_on"] = (state.alarm == ALARM_ACTIVE);
  serializeJson(doc, json);

  Serial.printf("[UPLOAD] state: is_door_locked=%d is_door_open=%d is_alarm_on=%d\n",
                state.lock == LOCKED, state.door == OPEN, state.alarm == ALARM_ACTIVE);

  if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
    int code = db.insert("state", json, false);  // table name is "state" not "system_state"
    Serial.printf("[UPLOAD] State response: %d\n", code);
    db.urlQuery_reset();
    xSemaphoreGive(dbMutex);
  } else {
    Serial.println("[UPLOAD] Could not acquire db mutex - skipping state.");
  }
}

// PATCH command SET executed=true where id=commandId
// Called after a command has been forwarded to the MCXC444 so that
// it wont be reprocessed on the next poll cycle
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

// FreeRTOS Tasks
//
// Task: Upload Consumer (Priority = 1)
// Continuously drains both uploadQueue (sensor readings) and stateQueue (system state snapshots)
// POSTing each item to Supa
// 
// Runs at low prio to ensure it doesnt starve time critical tasks
// (UART, keypad)
static void uploadTask(void *pv) {
  for (;;) {
    TSensorData sensorEntry;
    SystemState stateEntry;

    if (xQueueReceive(uploadQueue, &sensorEntry, 0) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED)
        uploadSensorToSupabase(sensorEntry);
      else
        Serial.println("[UPLOAD] WiFi disconnected - dropping sensor entry.");
    }

    if (xQueueReceive(stateQueue, &stateEntry, 0) == pdTRUE) {
      if (WiFi.status() == WL_CONNECTED)
        uploadStateToSupabase(stateEntry);
      else
        Serial.println("[UPLOAD] WiFi disconnected - dropping state entry.");
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Helper to map command strings to an integer
// Convert a command string from Supabase into the CommandType
static CommandType commandStrToInt(const char *cmd) {
  if (strncmp(cmd, CMD_LOCK_STR, 4) == 0) return CMD_LOCK;
  if (strncmp(cmd, CMD_UNLOCK_STR, 6) == 0) return CMD_UNLOCK;
  return CMD_INVALID;  // unknown
}

// Task: Command Poll (Supabase -> MCXC444)
// Priority = 1
// Every COMMAND_POLL_MS, queries Supabase for the oldest unexecuted
// command row (aka executed=false)
//
// If a valid command is found, it will be pushed onto the commandQueue for uartSendTask
// to send to the MCXC444
static void commandPollTask(void *pv) {
  for (;;) {
    //Serial.printf("[CMD POLL] WiFi status: %d\n", WiFi.status());
    if (WiFi.status() == WL_CONNECTED) {
      String response = "";

      if (xSemaphoreTake(dbMutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        // Take the latest value from commands table in DB
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

          // Extract command string and convert to integer
          const char *cmdStr = row["command"] | "unknown";
          cmd.command = commandStrToInt(cmdStr);
          cmd.commandId = row["id"] | 0;

          // If valid command, send it to queue
          if (cmd.command == CMD_INVALID) {
            Serial.printf("[CMD] Unknown command: '%s', skipping.\n", cmdStr);
          } else {
            Serial.printf("[CMD] New command: %d (id=%d)\n", cmd.command, cmd.commandId);
            xQueueSend(commandQueue, &cmd, 0);
          };
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(COMMAND_POLL_MS));
  }
}

// Task: UART Send (ESP32 -> MCXC444)
// Priority = 2
// Blocks on commandQueue indefitintely.
// When TCommand arrives, it serialise it to JSON and writes it to Serial1
// 
// After successful send, if WiFi doesnt fail me, it marks the Supabase row executed
// so the command isnt reissued on the next poll.
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
// Priority = 2
// Polls Serial1 for newline terminated JSON messages from MCXC444
static void uartRecvTask(void *pv) {
  for (;;) {
    if (Serial1.available()) {
      String line = Serial1.readStringUntil('\n');
      Serial.print("[UART RX] Received: ");
      Serial.println(line);
      line.trim();
      if (line.length() > 0) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, line);
        if (!err) {
          // MCU sends two message types:
          //   {"type":"sensor", "sensor":N, "value":V}  — upload to Supabase
          //   {"type":"state",  "door":N, "lock":N, "alarm":N} — ignore
          const char *type = doc["type"] | "sensor";  // default for legacy messages without "type"
          if (strcmp(type, "ack") == 0) {
            Serial.println("[UART RX] ACK received.");
          } else if (strcmp(type, "nack") == 0) {
            Serial.printf("[UART RX] NACK received.\n");
          } else if (strcmp(type, "sensor") == 0) {
            TSensorData entry;
            entry.sensor = (SensorType)doc["sensor"].as<int>();
            entry.value = doc["value"].as<float>();
            Serial.printf("[UART RX] sensor=%d value=%.2f\n",
                          (int)entry.sensor, entry.value);
            xQueueSend(uploadQueue, &entry, 0);
          } else {
            Serial.printf("[UART RX] Ignoring type='%s'\n", type);
          }
        } else {
          Serial.printf("[UART RX] JSON parse error: %s | raw: %s\n",
                        err.c_str(), line.c_str());
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Task: Temperature (DHT11)
// Priority = 2
// Reads the temp sensor every TEMP_TASK_DELAY
// pushes a TSensorData entry onto uploadQueue for cloud logging
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
    vTaskDelay(pdMS_TO_TICKS(TEMP_TASK_DELAY));
  }
}



// Task: Keypad -> Command Queue
// Priority = 2
static void keypadTask(void *pv) {
  const char correctPassword[] = "2271";
  char buffer[8];     // store entered digits
  uint8_t index = 0;  // current buffer index
  bool entering = false;

  for (;;) {
    char key = keypad.getKey();

    if (key) {
      Serial.printf("[KEYPAD] Pressed: %c\n", key);

      // Buzz, set timer to clear buzzer
      setBuzzer(BUZZER_VOLUME);
      xTimerStart(buzzerTimer, 0);

      if (key == '#') {
        // Start entering password
        entering = true;
        index = 0;
        memset(buffer, 0, sizeof(buffer));
        Serial.println("[KEYPAD] Enter password...");
      } else if (key == '*') {
        // Immediate lock
        TCommand cmd;
        cmd.commandId = 0;
        cmd.command = CMD_LOCK;
        //strncpy(cmd.command, "lock", sizeof(cmd.command));
        xQueueSend(commandQueue, &cmd, 0);
        Serial.println("[KEYPAD] Lock command sent.");
        entering = false;  // reset any partial password
      } else if (entering && index < sizeof(buffer) - 1) {
        // Append digit to buffer
        buffer[index++] = key;

        // Check password length
        if (index == strlen(correctPassword)) {
          if (strncmp(buffer, correctPassword, strlen(correctPassword)) == 0) {
            TCommand cmd;
            cmd.commandId = 0;
            cmd.command = CMD_UNLOCK;
            //strncpy(cmd.command, "unlock", sizeof(cmd.command));
            xQueueSend(commandQueue, &cmd, 0);
            Serial.println("[KEYPAD] Password correct. Unlock sent.");
          } else {
            Serial.println("[KEYPAD] Password incorrect.");
          }
          // Reset buffer
          entering = false;
          index = 0;
          memset(buffer, 0, sizeof(buffer));
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  unsigned long start = millis();
  while ((millis() - start) < 3000)
    ;

  Serial.println("\n--- PrisonRealm ESP32 Starting ---");

  initGojo();
  initUART1();
  initDHT();
  initBuzzer();
  initWiFi();

  // Sensor log stuff
  uploadQueue = xQueueCreate(20, sizeof(TSensorData));
  // Commands to MCXC444
  commandQueue = xQueueCreate(10, sizeof(TCommand));
  // State snapshots to upload
  stateQueue = xQueueCreate(10, sizeof(SystemState));

  // Single mutex protects the Supabase db object across all tasks
  dbMutex = xSemaphoreCreateMutex();

  xTaskCreate(uploadTask, "Upload", 12288, NULL, 1, NULL);
  xTaskCreate(uartRecvTask, "UART_Rx", 8192, NULL, 2, NULL);
  xTaskCreate(uartSendTask, "UART_Tx", 8192, NULL, 2, NULL);

  xTaskCreate(tempTask, "Temp", 8192, NULL, 2, NULL);
  xTaskCreate(keypadTask, "Keypad", 8192, NULL, 2, NULL);

  xTaskCreate(commandPollTask, "CmdPoll", 12288, NULL, 1, NULL);

  Serial.println("[INIT] All tasks started.\n");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
