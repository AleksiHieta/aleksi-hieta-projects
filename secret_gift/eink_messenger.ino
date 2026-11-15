#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <GxEPD2_BW.h>

// Wi-Fi & MQTT Settings
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* mqtt_server = "broker.emqx.io";
const char* mqtt_topic = "my_secret_eink_topic_12345"; // Must match HTML!

// CrowPanel 4.2" Internal Pinout (SSD1683 Driver, 400x300)
#define EPD_PWR  7
#define EPD_CS   45
#define EPD_DC   46
#define EPD_RST  47
#define EPD_BUSY 48
#define EPD_SCK  12
#define EPD_MOSI 11

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

WiFiClient espClient;
PubSubClient client(espClient);

// 400 x 300 pixels = 120,000 bits = 15,000 bytes (30,000 hex chars)
uint8_t bitmap[15000]; 

uint8_t hexToByte(char high, char low) {
  auto val = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  return (val(high) << 4) | val(low);
}

void callback(char* topic, byte* payload, unsigned int length) {
  if (length < 30000) return; // Wait for full 400x300 hex payload

  for (int i = 0; i < 15000; i++) {
    bitmap[i] = hexToByte((char)payload[i * 2], (char)payload[i * 2 + 1]);
  }

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawBitmap(0, 0, bitmap, 400, 300, GxEPD_BLACK);
  } while (display.nextPage());
}

void setup() {
  // Crucial: Turn on power to the CrowPanel e-ink screen
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(100);

  // Initialize custom SPI pins for CrowPanel
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(0); // Native 400x300 landscape

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  client.setServer(mqtt_server, 1883);
  client.setBufferSize(32000); // Increased buffer for 30,000-byte payload
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    while (!client.connected()) {
      if (client.connect("CrowPanel_42_Receiver")) {
        client.subscribe(mqtt_topic);
      } else {
        delay(2000);
      }
    }
  }
  client.loop();
}
