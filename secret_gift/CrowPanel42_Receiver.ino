/*
  CrowPanel 4.2" E-Paper MQTT Message Receiver + microSD Archive

  Board: Elecrow CrowPanel ESP32 4.2" E-Paper HMI, 400x300
  MCU: ESP32-S3
  Display: SSD1683 / GxEPD2_420_GDEY042T81

  Arduino libraries:
    - GxEPD2 by Jean-Marc Zingg
    - PubSubClient by Nick O'Leary
    - SD, SPI, WiFi (included with ESP32 Arduino core)

  SD card:
    Format the microSD card as FAT32.
    Built-in TF slot uses its own SPI bus:
      CS=GPIO10, MOSI=GPIO40, MISO=GPIO13, SCLK=GPIO39

  CrowPanel controls (active LOW):
    Rotary UP    = GPIO6 (older message)
    Rotary DOWN  = GPIO4 (newer message)
    Rotary PRESS = GPIO5 (jump to newest)

  MQTT wire format:
    V1|YYYY-MM-DD|message text|30000 HEX characters

  Archive format:
    /000001.dat, /000002.dat, ...
    Each file contains:
      4 bytes  "EINK"
      11 bytes date string (YYYY-MM-DD plus NUL)
      121 bytes message string (NUL padded)
      15000 bytes packed monochrome bitmap
*/

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>
#include <GxEPD2_BW.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

// ---------------- Wi-Fi and MQTT ----------------
const char* WIFI_SSID     = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC = "my_secret_eink_topic_12345";

// Payload: "V1|" + 10-byte date + "|" + up to 120 text bytes + "|" + 30000 hex bytes.
constexpr uint16_t MQTT_BUFFER_SIZE = 32200;

// ---------------- E-paper display pinout ----------------
#define EPD_PWR  7
#define EPD_CS   45
#define EPD_DC   46
#define EPD_RST  47
#define EPD_BUSY 48
#define EPD_SCK  12
#define EPD_MOSI 11

// ---------------- Built-in microSD slot ----------------
#define SD_CS    10
#define SD_MOSI  40
#define SD_MISO  13
#define SD_SCK   39

// ---------------- Rotary switch ----------------
#define ROTARY_DOWN  4
#define ROTARY_PRESS 5
#define ROTARY_UP    6

// ---------------- Bitmap and archive ----------------
constexpr uint16_t SCREEN_WIDTH = 400;
constexpr uint16_t SCREEN_HEIGHT = 300;
constexpr size_t BITMAP_BYTES = (SCREEN_WIDTH * SCREEN_HEIGHT) / 8;
constexpr size_t HEX_LENGTH = BITMAP_BYTES * 2;
constexpr size_t DATE_FIELD_SIZE = 11;
constexpr size_t TEXT_FIELD_SIZE = 121;
constexpr size_t ARCHIVE_HEADER_SIZE = 4 + DATE_FIELD_SIZE + TEXT_FIELD_SIZE;
constexpr uint8_t HEADER_HEIGHT = 43;

uint8_t bitmap[BITMAP_BYTES];
uint8_t incomingBitmap[BITMAP_BYTES]; // Static storage; do not put 15 KB on the callback stack.
char currentDate[DATE_FIELD_SIZE] = "---- -- --";
char currentMessage[TEXT_FIELD_SIZE] = "";
uint32_t newestMessageId = 0;
uint32_t currentMessageId = 0;
bool sdReady = false;

SPIClass sdSPI(HSPI);

GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool validDate(const char* s) {
  if (strlen(s) != 10) return false;
  for (int i = 0; i < 10; ++i) {
    if (i == 4 || i == 7) {
      if (s[i] != '-') return false;
    } else if (!isdigit((unsigned char)s[i])) {
      return false;
    }
  }
  return true;
}

void showStatus(const char* line1, const char* line2 = "") {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(12, 28);
    display.print(line1);
    display.setTextSize(1);
    display.setCursor(12, 52);
    display.print(line2);
  } while (display.nextPage());
}

void drawCurrentMessage() {
  display.setFullWindow();
  display.firstPage();

  do {
    display.fillScreen(GxEPD_WHITE);

    // Draw the received 400x300 bitmap.
    display.drawBitmap(0, 0, bitmap, SCREEN_WIDTH, SCREEN_HEIGHT, GxEPD_BLACK);

    // Reserve and redraw a clean header over the top of the bitmap.
    display.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, GxEPD_WHITE);
    display.drawFastHLine(0, HEADER_HEIGHT - 1, SCREEN_WIDTH, GxEPD_BLACK);

    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);

    // Date in the upper-left corner; date only, no time.
    display.setCursor(6, 13);
    display.print(currentDate);

    // Archive message number at upper-right.
    char idLabel[24];
    snprintf(idLabel, sizeof(idLabel), "MESSAGE #%06lu",
             (unsigned long)currentMessageId);
    int16_t x1, y1;
    uint16_t tw, th;
    display.getTextBounds(idLabel, 0, 0, &x1, &y1, &tw, &th);
    display.setCursor(SCREEN_WIDTH - tw - 6, 13);
    display.print(idLabel);

    // Sender-entered text on the second header line.
    display.setCursor(6, 32);
    if (currentMessage[0] != '\0') {
      display.print(currentMessage);
    }

  } while (display.nextPage());

  Serial.printf("Displayed message #%lu (%s)\n",
                (unsigned long)currentMessageId, currentDate);
}

bool parseMessage(byte* payload, unsigned int length,
                  char* dateOut, char* textOut, uint8_t* bitmapOut) {
  // Minimum: V1| + date + | + empty text + | + bitmap hex.
  if (length < 3 + 10 + 1 + 1 + HEX_LENGTH) return false;

  // Header is ASCII: V1|YYYY-MM-DD|text|HEX...
  if (payload[0] != 'V' || payload[1] != '1' || payload[2] != '|') return false;

  int dateEnd = -1;
  for (unsigned int i = 3; i < length; ++i) {
    if (payload[i] == '|') {
      dateEnd = (int)i;
      break;
    }
  }
  if (dateEnd != 13) return false; // Exactly 10 date characters after "V1|".

  char dateTemp[11];
  memcpy(dateTemp, payload + 3, 10);
  dateTemp[10] = '\0';
  if (!validDate(dateTemp)) return false;

  int textEnd = -1;
  for (unsigned int i = dateEnd + 1; i < length; ++i) {
    if (payload[i] == '|') {
      textEnd = (int)i;
      break;
    }
  }
  if (textEnd < 0) return false;

  const size_t textLength = (size_t)(textEnd - dateEnd - 1);
  if (textLength >= TEXT_FIELD_SIZE) return false;
  if (length - (unsigned int)textEnd - 1 != HEX_LENGTH) return false;

  // Validate every hex digit before accepting the message.
  const unsigned int hexStart = (unsigned int)textEnd + 1;
  for (size_t i = 0; i < HEX_LENGTH; ++i) {
    if (hexValue((char)payload[hexStart + i]) < 0) return false;
  }

  memcpy(dateOut, dateTemp, sizeof(dateTemp));
  memset(textOut, 0, TEXT_FIELD_SIZE);
  if (textLength) {
    memcpy(textOut, payload + dateEnd + 1, textLength);
  }

  for (size_t i = 0; i < BITMAP_BYTES; ++i) {
    const int hi = hexValue((char)payload[hexStart + i * 2]);
    const int lo = hexValue((char)payload[hexStart + i * 2 + 1]);
    bitmapOut[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool getMessagePath(uint32_t id, char* path, size_t pathSize) {
  const int n = snprintf(path, pathSize, "/%06lu.dat", (unsigned long)id);
  return n > 0 && (size_t)n < pathSize;
}

uint32_t findNewestMessageId() {
  if (!sdReady) return 0;

  File root = SD.open("/");
  if (!root) return 0;

  uint32_t maxId = 0;
  File entry = root.openNextFile();

  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      const char* base = strrchr(name, '/');
      base = base ? base + 1 : name;

      unsigned long parsed = 0;
      char extra = '\0';
      if (sscanf(base, "%6lu.dat%c", &parsed, &extra) == 1 && parsed > maxId) {
        maxId = (uint32_t)parsed;
      }
    }
    entry.close();
    entry = root.openNextFile();
  }

  root.close();
  return maxId;
}

bool saveMessageToSD(uint32_t id, const char* date, const char* message,
                     const uint8_t* imageData) {
  if (!sdReady) return false;

  char path[20];
  if (!getMessagePath(id, path, sizeof(path))) return false;

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("Could not create archive file %s\n", path);
    return false;
  }

  const char magic[4] = {'E', 'I', 'N', 'K'};
  char dateField[DATE_FIELD_SIZE] = {0};
  char textField[TEXT_FIELD_SIZE] = {0};
  strncpy(dateField, date, DATE_FIELD_SIZE - 1);
  strncpy(textField, message, TEXT_FIELD_SIZE - 1);

  bool ok = true;
  if (file.write((const uint8_t*)magic, sizeof(magic)) != sizeof(magic)) ok = false;
  if (file.write((const uint8_t*)dateField, sizeof(dateField)) != sizeof(dateField)) ok = false;
  if (file.write((const uint8_t*)textField, sizeof(textField)) != sizeof(textField)) ok = false;
  if (file.write(imageData, BITMAP_BYTES) != BITMAP_BYTES) ok = false;

  file.flush();
  file.close();

  if (!ok) {
    SD.remove(path);
    Serial.println("SD write failed; incomplete archive file removed.");
    return false;
  }

  Serial.printf("Saved message #%lu to %s\n", (unsigned long)id, path);
  return true;
}

bool loadMessageFromSD(uint32_t id) {
  if (!sdReady || id == 0) return false;

  char path[20];
  if (!getMessagePath(id, path, sizeof(path))) return false;

  File file = SD.open(path, FILE_READ);
  if (!file) return false;

  char magic[4];
  char dateField[DATE_FIELD_SIZE];
  char textField[TEXT_FIELD_SIZE];

  bool ok = true;
  if (file.read((uint8_t*)magic, sizeof(magic)) != sizeof(magic)) ok = false;
  if (ok && memcmp(magic, "EINK", 4) != 0) ok = false;
  if (ok && file.read((uint8_t*)dateField, sizeof(dateField)) != sizeof(dateField)) ok = false;
  if (ok && file.read((uint8_t*)textField, sizeof(textField)) != sizeof(textField)) ok = false;
  if (ok && file.read(bitmap, BITMAP_BYTES) != BITMAP_BYTES) ok = false;
  file.close();

  if (!ok) {
    Serial.printf("Archive file %s is invalid or incomplete.\n", path);
    return false;
  }

  dateField[DATE_FIELD_SIZE - 1] = '\0';
  textField[TEXT_FIELD_SIZE - 1] = '\0';
  if (!validDate(dateField)) return false;

  strncpy(currentDate, dateField, sizeof(currentDate));
  strncpy(currentMessage, textField, sizeof(currentMessage));
  currentMessageId = id;
  return true;
}

void displayNewestMessageOrBlank() {
  newestMessageId = findNewestMessageId();

  if (newestMessageId > 0 && loadMessageFromSD(newestMessageId)) {
    drawCurrentMessage();
  } else {
    currentMessageId = 0;
    strcpy(currentDate, "NO MESSAGES");
    currentMessage[0] = '\0';
    memset(bitmap, 0, sizeof(bitmap));
    showStatus("E-Ink Messenger", sdReady ? "Waiting for first message..." : "SD card not available");
  }
}

void handleIncomingMessage(byte* payload, unsigned int length) {
  char receivedDate[DATE_FIELD_SIZE];
  char receivedText[TEXT_FIELD_SIZE];
  if (!parseMessage(payload, length, receivedDate, receivedText, incomingBitmap)) {
    Serial.println("Rejected malformed MQTT message.");
    return;
  }

  if (!sdReady) {
    Serial.println("Message received, but SD is unavailable; not archived.");
    return;
  }

  const uint32_t newId = newestMessageId + 1;
  if (!saveMessageToSD(newId, receivedDate, receivedText, incomingBitmap)) {
    Serial.println("Message was not displayed because archive save failed.");
    return;
  }

  newestMessageId = newId;
  if (loadMessageFromSD(newId)) {
    drawCurrentMessage();
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC) != 0) return;
  handleIncomingMessage(payload, length);
}

void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("Wi-Fi IP: ");
  Serial.println(WiFi.localIP());
}

void connectToMQTT() {
  while (!mqttClient.connected()) {
    String clientId = "CrowPanel42-";
    clientId += String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);

    Serial.printf("Connecting to MQTT %s:%u...\n", MQTT_HOST, MQTT_PORT);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("MQTT connected.");
      if (mqttClient.subscribe(MQTT_TOPIC, 0)) {
        Serial.printf("Subscribed to %s\n", MQTT_TOPIC);
      } else {
        Serial.println("MQTT subscribe failed.");
      }
    } else {
      Serial.printf("MQTT failed, state=%d; retrying in 3 seconds.\n", mqttClient.state());
      delay(3000);
    }
  }
}

void initSDCard() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  // Dedicated HSPI bus; the e-paper continues using the default SPI bus.
  if (!SD.begin(SD_CS, sdSPI, 20000000)) {
    sdReady = false;
    Serial.println("microSD initialization failed. Check FAT32 card and slot.");
    return;
  }

  sdReady = true;
  Serial.printf("microSD ready. Card size: %llu MB\n",
                SD.cardSize() / (1024ULL * 1024ULL));
}

void pollRotarySwitch() {
  static uint32_t lastChange = 0;
  static int lastUp = HIGH;
  static int lastDown = HIGH;
  static int lastPress = HIGH;

  const uint32_t now = millis();
  if (now - lastChange < 180) return;

  const int up = digitalRead(ROTARY_UP);
  const int down = digitalRead(ROTARY_DOWN);
  const int press = digitalRead(ROTARY_PRESS);

  if (up == LOW && lastUp == HIGH) {
    lastChange = now;
    if (currentMessageId > 1 && loadMessageFromSD(currentMessageId - 1)) {
      drawCurrentMessage();
    }
  } else if (down == LOW && lastDown == HIGH) {
    lastChange = now;
    if (currentMessageId < newestMessageId &&
        loadMessageFromSD(currentMessageId + 1)) {
      drawCurrentMessage();
    }
  } else if (press == LOW && lastPress == HIGH) {
    lastChange = now;
    if (newestMessageId > 0 && loadMessageFromSD(newestMessageId)) {
      drawCurrentMessage();
    }
  }

  lastUp = up;
  lastDown = down;
  lastPress = press;
}

void setup() {
  Serial.begin(115200);
  delay(150);

  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(100);

  pinMode(ROTARY_UP, INPUT_PULLUP);
  pinMode(ROTARY_DOWN, INPUT_PULLUP);
  pinMode(ROTARY_PRESS, INPUT_PULLUP);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(0);
  showStatus("E-Ink Messenger", "Starting SD card...");

  initSDCard();
  if (sdReady) {
    displayNewestMessageOrBlank();
  } else {
    showStatus("SD card error", "Insert a FAT32 microSD card and restart.");
  }

  connectToWiFi();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(MQTT_BUFFER_SIZE);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(15);
  mqttClient.setCallback(mqttCallback);

  Serial.println("Receiver ready.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectToWiFi();
  if (!mqttClient.connected()) connectToMQTT();

  mqttClient.loop();
  pollRotarySwitch();
}
