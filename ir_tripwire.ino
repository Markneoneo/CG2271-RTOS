#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// Pin Definitions
const uint16_t kRecvPin = 14;
const uint16_t kIrLedPin = 4;

// Initialize objects
IRrecv irrecv(kRecvPin);
IRsend irsend(kIrLedPin);
decode_results results;

void setup() {
  Serial.begin(115200);
  
  irrecv.enableIRIn(); // Start the receiver
  irsend.begin();      // Start the sender
  
  Serial.println("System Ready: Listening and Ready to Transmit");
}

void loop() {
  // --- TRANSMITTER SECTION ---
  // Send a signal every 5 seconds without needing a trigger
  static unsigned long lastSend = 0;
  if (millis() - lastSend > 5000) {
    Serial.println("Auto-sending IR signal...");
    
    irrecv.pause();           // Stop receiving so we don't hear ourselves
    irsend.sendSony(0xa90, 12); 
    irrecv.resume();          // Start listening again
    
    lastSend = millis();
  }

  // --- RECEIVER SECTION ---
  if (irrecv.decode(&results)) {
    Serial.print("I heard a signal: ");
    serialPrintUint64(results.value, HEX);
    Serial.println("");
    irrecv.resume(); 
  }
}