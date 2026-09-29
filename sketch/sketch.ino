#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <DHT.h>
#include <DHT_U.h>
#include <SPI.h>
#include <LoRa.h>
#include <Arduino_RouterBridge.h> // Router bridge integration

// Node configuration
#define MY_NODE_ID 1  // Identifier for this physical board
#define MAX_NODES 10

// Hardware pins
#define DHTPIN 2
#define DHTTYPE DHT22
#define LORA_SS 10
#define LORA_RST 9
#define LORA_DIO0 8

Adafruit_BMP280 bmp;
DHT_Unified dht(DHTPIN, DHTTYPE);

// Decentralized state structure
#pragma pack(push, 1)
struct NodeData {
  uint32_t version;
  float bmp_temp;
  float bmp_pres;
  float dht_temp;
  float dht_humi;
  long radar_density;
};

struct LoRaPacket {
  uint8_t senderId;
  uint8_t numNodes;
  uint8_t nodeIds[MAX_NODES];
  NodeData nodes[MAX_NODES];
};
#pragma pack(pop)

// Custom mini bridge for serial communication
class MiniBridge {
  public:
    void begin() {
      Monitor.begin();
    }
    void put(String key, String value) {
      Monitor.print("BRIDGE:PUT:");
      Monitor.print(key);
      Monitor.print("=");
      Monitor.println(value);
    }
};

MiniBridge bridge;
NodeData globalState[MAX_NODES + 1]; // +1 because Node IDs are 1-indexed

unsigned long lastSensorRead = 0;
unsigned long lastGossip = 0;
unsigned long lastSerialPrint = 0;
uint32_t localVersionCounter = 1;

// Radar variables
const int MAX_FRAME_LEN = 128;
uint8_t frameBuf[MAX_FRAME_LEN];
int frameIdx = 0;
bool receiving = false;
int expectedLen = 0;
long current_radar_density = 0;

void sendCommand(const uint8_t* data, size_t len) {
  Serial1.write(data, len);
}

void setup() {
  // Initialize bridge
  bridge.begin();
  
  // Initialize Serial1 for mmWave Radar
  Serial1.begin(115200);

  bmp.begin(0x76);
  dht.begin();
  
  // Initialize LoRa
  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) { // SX1278 typically 433MHz
    bridge.put("error", "LoRa init failed. Check wiring.");
  }
  
  // Enable report mode for mmWave radar
  uint8_t cmdEnableReport[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x12, 0x00, 
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01
  };
  sendCommand(cmdEnableReport, sizeof(cmdEnableReport));
  
  // Initialize local state memory
  for(int i=0; i<=MAX_NODES; i++) {
    globalState[i].version = 0;
  }
}

void readEnvironmentalSensors() {
  globalState[MY_NODE_ID].bmp_temp = bmp.readTemperature();
  globalState[MY_NODE_ID].bmp_pres = bmp.readPressure();
  
  sensors_event_t event;
  dht.temperature().getEvent(&event);
  if (!isnan(event.temperature)) globalState[MY_NODE_ID].dht_temp = event.temperature;

  dht.humidity().getEvent(&event);
  if (!isnan(event.relative_humidity)) globalState[MY_NODE_ID].dht_humi = event.relative_humidity;
  
  globalState[MY_NODE_ID].radar_density = current_radar_density;
  globalState[MY_NODE_ID].version = localVersionCounter++;
}

void parseReportFrame(uint8_t* payload, int len) {
  if (len < 35) return; 
  long totalDensityScore = 0;
  for (int i = 0; i < 16; i++) {
    int byteOffset = 3 + (i * 2);
    totalDensityScore += (payload[byteOffset] | (payload[byteOffset + 1] << 8));
  }
  current_radar_density = totalDensityScore;
}

void processIncomingRadar() {
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (!receiving) {
      frameBuf[frameIdx++] = b;
      if (frameIdx >= 4) {
        if (frameBuf[frameIdx-4] == 0xF4 && frameBuf[frameIdx-3] == 0xF3 && 
            frameBuf[frameIdx-2] == 0xF2 && frameBuf[frameIdx-1] == 0xF1) {
          receiving = true;
          frameBuf[0] = 0xF4; frameBuf[1] = 0xF3; frameBuf[2] = 0xF2; frameBuf[3] = 0xF1;
          frameIdx = 4;
        } else {
          frameBuf[0] = frameBuf[1]; frameBuf[1] = frameBuf[2];
          frameBuf[2] = frameBuf[3]; frameIdx = 3;
        }
      }
    } else {
      frameBuf[frameIdx++] = b;
      if (frameIdx == 6) {
        expectedLen = frameBuf[4] | (frameBuf[5] << 8);
        if (expectedLen > MAX_FRAME_LEN - 10) { receiving = false; frameIdx = 0; continue; }
      }
      if (frameIdx >= 6 && frameIdx == (6 + expectedLen + 4)) {
        if (frameBuf[frameIdx-4] == 0xF8 && frameBuf[frameIdx-3] == 0xF7 && 
            frameBuf[frameIdx-2] == 0xF6 && frameBuf[frameIdx-1] == 0xF5) {
          parseReportFrame(&frameBuf[6], expectedLen);
        }
        receiving = false;
        frameIdx = 0;
      }
    }
  }
}

void broadcastGossip() {
  LoRaPacket pkt;
  pkt.senderId = MY_NODE_ID;
  pkt.numNodes = 0;
  
  for(int i=1; i<=MAX_NODES; i++) {
    if (globalState[i].version > 0) {
      pkt.nodeIds[pkt.numNodes] = i;
      pkt.nodes[pkt.numNodes] = globalState[i];
      pkt.numNodes++;
    }
  }
  
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&pkt, sizeof(pkt));
  LoRa.endPacket();
}

void receiveGossip() {
  int packetSize = LoRa.parsePacket();
  if (packetSize == sizeof(LoRaPacket)) {
    LoRaPacket pkt;
    LoRa.readBytes((uint8_t*)&pkt, sizeof(pkt));
    
    // CRDT Last-Writer-Wins merge logic
    for (int i=0; i<pkt.numNodes; i++) {
      uint8_t id = pkt.nodeIds[i];
      if (id > 0 && id <= MAX_NODES && id != MY_NODE_ID) { // Ignore self-updates
        if (pkt.nodes[i].version > globalState[id].version) {
          globalState[id] = pkt.nodes[i];
        }
      }
    }
  }
}

void reportStateToPython() {
  // Output full network state as a JSON array
  String json = "[";
  bool first = true;
  for(int i=1; i<=MAX_NODES; i++) {
    if (globalState[i].version > 0) {
      if (!first) json += ",";
      json += "{";
      json += "\"node_id\":" + String(i) + ",";
      json += "\"version\":" + String(globalState[i].version) + ",";
      json += "\"bmp_temp\":" + String(globalState[i].bmp_temp, 2) + ",";
      json += "\"bmp_pres\":" + String(globalState[i].bmp_pres, 2) + ",";
      json += "\"dht_temp\":" + String(globalState[i].dht_temp, 2) + ",";
      json += "\"dht_humi\":" + String(globalState[i].dht_humi, 2) + ",";
      json += "\"radar_density\":" + String(globalState[i].radar_density);
      json += "}";
      first = false;
    }
  }
  json += "]";
  
  // Output data over bridge
  bridge.put("network_state", json);
}

void loop() {
  processIncomingRadar();
  receiveGossip();

  if (millis() - lastSensorRead >= 2000) {
    readEnvironmentalSensors();
    lastSensorRead = millis();
  }

  if (millis() - lastGossip >= (5000 + random(0, 3000))) {
    broadcastGossip();
    lastGossip = millis();
  }

  if (millis() - lastSerialPrint >= 2000) {
    reportStateToPython();
    lastSerialPrint = millis();
  }
}
