#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <DHT_U.h>

#define DHTPIN 2
#define DHTTYPE DHT22

Adafruit_BMP280 bmp;
DHT_Unified dht(DHTPIN, DHTTYPE);

unsigned long lastSensorReadTime = 0;
const unsigned long SENSOR_INTERVAL = 2000; // Read env sensors every 2 seconds

void sendCommand(const uint8_t* data, size_t len) {
  Serial1.write(data, len);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(115200);

  // Wait for serial monitor to connect
  while (!Serial) { delay(10); }

  Serial.println("======================================");
  Serial.println("Initializing Environmental Sensors...");
  
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 initialization failed! Check wiring/I2C address.");
  }
  
  dht.begin();
  
  Serial.println("Configuring mmWave Sensor into Report Mode...");
  
  // The exact command from the documentation image to enable Report Mode
  uint8_t cmdEnableReport[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 
    0x08, 0x00, 
    0x12, 0x00, 
    0x00, 0x00, 
    0x04, 0x00, 0x00, 0x00, 
    0x04, 0x03, 0x02, 0x01
  };
  sendCommand(cmdEnableReport, sizeof(cmdEnableReport));
  delay(200); // Give it a moment to switch modes

  Serial.println("System configured successfully! Reading Data...");
  Serial.println("======================================");
}

const int MAX_FRAME_LEN = 128;
uint8_t frameBuf[MAX_FRAME_LEN];
int frameIdx = 0;
bool receiving = false;
int expectedLen = 0;

void parseReportFrame(uint8_t* payload, int len);
void readEnvironmentalSensors();

void loop() {
  // Read incoming bytes from radar
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    
    // Look for Frame Header: F4 F3 F2 F1
    if (!receiving) {
      frameBuf[frameIdx++] = b;
      if (frameIdx >= 4) {
        if (frameBuf[frameIdx-4] == 0xF4 && frameBuf[frameIdx-3] == 0xF3 && 
            frameBuf[frameIdx-2] == 0xF2 && frameBuf[frameIdx-1] == 0xF1) {
          receiving = true;
          // Shift the header to the start of our buffer
          frameBuf[0] = 0xF4; frameBuf[1] = 0xF3; frameBuf[2] = 0xF2; frameBuf[3] = 0xF1;
          frameIdx = 4;
        } else {
          // Slide the window if no match
          frameBuf[0] = frameBuf[1];
          frameBuf[1] = frameBuf[2];
          frameBuf[2] = frameBuf[3];
          frameIdx = 3;
        }
      }
    } else {
      frameBuf[frameIdx++] = b;
      
      // The 2 bytes after header define the length of the payload
      if (frameIdx == 6) {
        expectedLen = frameBuf[4] | (frameBuf[5] << 8);
        if (expectedLen > MAX_FRAME_LEN - 10) { 
          // Prevent buffer overflow if garbage data arrives
          receiving = false;
          frameIdx = 0;
          continue;
        }
      }
      
      // Check if we received the full frame (Header 4 + Len 2 + expectedLen + Tail 4)
      if (frameIdx >= 6 && frameIdx == (6 + expectedLen + 4)) {
        // Verify Frame Tail: F8 F7 F6 F5
        if (frameBuf[frameIdx-4] == 0xF8 && frameBuf[frameIdx-3] == 0xF7 && 
            frameBuf[frameIdx-2] == 0xF6 && frameBuf[frameIdx-1] == 0xF5) {
          
          // Send the payload off to be analyzed!
          parseReportFrame(&frameBuf[6], expectedLen);
        }
        
        // Reset state for the next frame
        receiving = false;
        frameIdx = 0;
      }
    }
  }

  // Non-blocking timer for environmental sensors (runs every 2 seconds)
  if (millis() - lastSensorReadTime >= SENSOR_INTERVAL) {
    lastSensorReadTime = millis();
    readEnvironmentalSensors();
  }
}

void readEnvironmentalSensors() {
  float tempBMP = bmp.readTemperature();
  float pressure = bmp.readPressure();

  Serial.println("\n--- Environment Data ---");
  Serial.print("BMP280 Temp: "); Serial.print(tempBMP); Serial.println(" *C");
  Serial.print("BMP280 Pres: "); Serial.print(pressure); Serial.println(" Pa");
  
  sensors_event_t event;
  
  // Get temperature event
  dht.temperature().getEvent(&event);
  if (isnan(event.temperature)) {
    Serial.println("DHT22 Temp read failed!");
  } else {
    Serial.print("DHT22  Temp: "); Serial.print(event.temperature); Serial.println(" *C");
  }

  // Get humidity event
  dht.humidity().getEvent(&event);
  if (isnan(event.relative_humidity)) {
    Serial.println("DHT22 Humi read failed!");
  } else {
    Serial.print("DHT22  Humi: "); Serial.print(event.relative_humidity); Serial.println(" %");
  }
  
  Serial.println("------------------------\n");
}

void parseReportFrame(uint8_t* payload, int len) {
  // According to the image, the payload has:
  // 1 byte Detection Result
  // 2 bytes Target Distance
  // 32 bytes Energy Values (16 gates * 2 bytes)
  // Total expected length = 35 bytes
  
  if (len < 35) {
    return; // Ignore frames that don't match our expected format
  }

  uint8_t detectionResult = payload[0]; // 00 absent, 01 present
  uint16_t targetDistance = payload[1] | (payload[2] << 8); // Little endian
  
  long totalDensityScore = 0;
  
  // Parse the 16 distance gates (2 bytes each) starting at byte 3
  for (int i = 0; i < 16; i++) {
    int byteOffset = 3 + (i * 2);
    uint16_t gateEnergy = payload[byteOffset] | (payload[byteOffset + 1] << 8);
    
    totalDensityScore += gateEnergy;
  }

  // Print our custom analysis to the Serial Monitor
  Serial.print("Activity: "); 
  if (detectionResult == 0) Serial.print("[ ABSENT ]");
  else Serial.print("[ PRESENT ]");
  
  Serial.print("\t| Distance: ");
  Serial.print(targetDistance);
  Serial.print(" cm");

  Serial.print("\t| Density Score (Total Energy): ");
  Serial.println(totalDensityScore);
}
