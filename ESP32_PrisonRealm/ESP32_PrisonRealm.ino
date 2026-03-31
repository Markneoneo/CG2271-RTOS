#include <ESPSupabase.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "env.h"

Supabase db;
const char *supabase_url = SUPABASE_URL;
const char *anon_key = SUPABASE_ANON;

const char *ssid = SSID;
const char *password = PASSWORD;

String JSON;

int lastTime;
bool triggered = false;

void uploadSensorData(float value) {
  db.begin(supabase_url, anon_key);
  JsonDocument doc;
  doc["value"] = value;
  serializeJson(doc, JSON);
  Serial.println("Sending JSON:");
  serializeJsonPretty(doc, Serial);
  Serial.println();
  int response = db.insert("sensor", JSON, false);
  Serial.print("Response Code: ");
  Serial.println(response);
  db.urlQuery_reset();
}

void action() {
  triggered = true;
}

void setup() {

  Serial1.begin(115200, SERIAL_8N1, NEW_RX_PIN, NEW_TX_PIN);
  Serial.begin(115200);
  /*
  Serial.print("Connecting to SSID: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.print("\nConnected to WiFi network with IP Address: ");
  Serial.println(WiFi.localIP());
*/
}

void loop() {
  // put your main code here, to run repeatedly:
  while (Serial1.available() <= 0)
    ;
  String json = Serial1.readString();
  Serial.print("Message received: ");
  Serial.print(json);
  JsonDocument doc;
  deserializeJson(doc, json);
  int sensor = doc["sensor"];
  int value = doc["value"];
  Serial.println(sensor);
  Serial.println(value);
}

