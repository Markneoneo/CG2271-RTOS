#include "DHT.h"

#define DHTPIN 8
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

struct SmartSafeEnv {
    float temperature;
    float humidity;
    String status;      
    long lastUpdated;   
};

SmartSafeEnv currentEnv;

String getThermalState(float temp) {
    if (temp > 40.0) return "FIRE_ALERT";
    if (temp > 35.0) return "OVERHEAT";
    if (temp < 10.0) return "FREEZE_ALERT";
    return "STABLE";
}

void broadcastEnvironment() {
    Serial.print("$TEMP:");
    Serial.print(currentEnv.temperature);
    Serial.print(";HUM:");
    Serial.print(currentEnv.humidity);
    Serial.print(";STAT:");
    Serial.print(currentEnv.status);
    Serial.println(";");
}

void setup() {
    Serial.begin(115200);
    dht.begin();
    Serial.println(F("System Initialized..."));
}

void loop() {
    if (Serial.available() > 0) {
        String pcRequest = Serial.readStringUntil('\n');
        pcRequest.trim();

        if (pcRequest == "GET_STATUS") {
            Serial.println(F("\n--- Manual Status Check ---"));
            Serial.print(F("Current Temp: ")); Serial.println(currentEnv.temperature);
            Serial.print(F("Current Humidity: ")); Serial.println(currentEnv.humidity);
            Serial.print(F("Current Stat: ")); Serial.println(currentEnv.status);
            Serial.println(F("---------------------------\n"));
        }
    }

    // DHT11 needs time to stabilize
    delay(2000); 

    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (isnan(h) || isnan(t)) {
        Serial.println(F("E:SENSOR_FAIL"));
        return;
    }

    currentEnv.temperature = t;
    currentEnv.humidity = h;
    currentEnv.status = getThermalState(t);
    currentEnv.lastUpdated = millis();

    broadcastEnvironment();
}