#include <SoftwareSerial.h>
#include <TinyGPS++.h>

// RX Pin = 4 (Connect to GPS TX), TX Pin = 3 (Connect to GPS RX)
SoftwareSerial ss(4, 3);
TinyGPSPlus gps;

void setup() {
  Serial.begin(115200); // Fast speed fixes the text corruption
  ss.begin(9600);       // Default NEO-6M speed
  Serial.println("System Ready. Monitoring satellite search...");
}

void loop() {
  while (ss.available() > 0) {
    gps.encode(ss.read());
  }

  // Print a clean status update once every 2 seconds instead of flooding the screen
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    lastPrint = millis();
    
    Serial.print("Satellites in view: ");
    Serial.print(gps.satellites.value());
    
    if (gps.location.isValid()) {
      Serial.print(" | LAT: "); Serial.print(gps.location.lat(), 6);
      Serial.print(" | LNG: "); Serial.println(gps.location.lng(), 6);
    } else {
      Serial.println(" | Status: Waiting for sky lock...");
    }
  }
}

