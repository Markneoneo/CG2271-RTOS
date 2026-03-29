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
  Serial.begin(115200);
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

  pinMode(REED_SENSOR, INPUT);
  attachInterrupt(digitalPinToInterrupt(REED_SENSOR), action, FALLING);
}

void loop() {
  // put your main code here, to run repeatedly:
  if (Serial.available() > 0) {
    String s = Serial.readString();
    if (s == "send") {
      uploadSensorData(5);
    }
  }

  if (triggered) {
    Serial.println("Shaken");
    triggered = false;
  }
}
