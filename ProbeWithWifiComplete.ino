#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <Preferences.h>

// =======================
// --- PROBE CONFIG ---
// =======================

#define PREFIX "F17H"    // 4-char prefix for this probe

#define SDA_PIN 4
#define SCL_PIN 7

#define SCD41_ADDR 0x62
#define DBM_ADDR   0x48

// WiFi + HTTP
#define WIFI_SSID  "Bally's Guest"
const char* POST_URL = "http://34.160.35.22/api/probedata";

// =======================
// --- UART CONFIG ---
// =======================
#define UART_TX_PIN 21
#define UART_RX_PIN 20
#define UART_BAUD   115200

HardwareSerial extSerial(1);  // use UART1 peripheral

// =======================
// --- DISPLAY CONFIG ---
// =======================
#define HAS_SCREEN  true
#define BIG_BOY     false

#if HAS_SCREEN
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
  #define SCREEN_WIDTH  128
  #define SCREEN_HEIGHT 64
  #define OLED_ADDR     0x3C
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
#endif

// =======================
// --- DECIBEL METER REGS ---
// =======================
#define DBM_REG_VERSION 0x00
#define DBM_REG_DECIBEL 0x0A

#define SECONDS 1000     

SensirionI2cScd4x scd4x;
Preferences prefs;

// =======================
// --- GLOBAL SENSOR VALUES ---
// =======================
uint16_t co2 = 0;
float temp = 0;
float hum = 0;
uint8_t db = 0;
bool haveCO2 = false;
bool haveDB  = false;
unsigned long updateFrequencySeconds = 1;  // default send interval

bool scdFound = false;
bool decibelFound = false;

// HTTP status for display
String lastPostResult = String("WAITING");  // "POST OK", "FAIL", "NO WIFI"

// For showing I2C devices in setup
String i2cSummary = "";

// =======================
// --- HELPERS ---
// =======================
uint8_t readDBReg(uint8_t reg) {
  Wire.beginTransmission(DBM_ADDR);
  Wire.write(reg);
  Wire.endTransmission();
  Wire.requestFrom(DBM_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0xFF;
}

// =======================
// --- I2C SCAN ---
// =======================
void scanI2CBus() {
  Serial.println("\nScanning I2C bus...");

  byte error, address;
  int nDevices = 0;
  i2cSummary = "";

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("✔ Found device at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);

      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", address);
      i2cSummary += buf;

      if (address == SCD41_ADDR) {
        scdFound = true;
      } else if (address == DBM_ADDR) {
        decibelFound = true;
      }
      nDevices++;
    } else if (error == 4) {
      Serial.print("⚠ Unknown error at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  if (nDevices == 0) {
    Serial.println("❌ No I2C devices found\n");
    i2cSummary = "None";
  } else {
    Serial.println("✅ Scan complete\n");
  }
}

// =======================
// --- DECIBEL ---
// =======================
void initDecibel() {
  delay(100);
  uint8_t version = readDBReg(DBM_REG_VERSION);
  Serial.printf("Decibel meter version: 0x%02X\n", version);
}

void readDecibel() {
  uint8_t newDB = readDBReg(DBM_REG_DECIBEL);
  if (newDB != 255) {
    db = newDB;
    haveDB = true;
    Serial.printf("Sound: %3d dB\n", db);
  } else {
    Serial.println("Sound: waiting for valid reading...");
  }
}

// =======================
// --- SCD41 ---
// =======================
void initSCD41() {
  Serial.println("Resetting and starting SCD41...");
  Wire.beginTransmission(SCD41_ADDR);
  Wire.write(0x36); Wire.write(0xF6);  // wake
  Wire.endTransmission();
  delay(20);

  Wire.beginTransmission(SCD41_ADDR);
  Wire.write(0x00); Wire.write(0x06);  // soft reset
  Wire.endTransmission();
  delay(50);

  scd4x.begin(Wire, SCD41_ADDR);
  scd4x.stopPeriodicMeasurement();
  delay(500);

  int16_t err = scd4x.reinit();
  Serial.printf("reinit() returned %d\n", err);
  delay(1200);

  err = scd4x.startPeriodicMeasurement();
  Serial.printf("startPeriodicMeasurement() returned %d\n", err);

  Serial.println("Waiting 5 s for SCD41 to produce first data...");
  delay(5000);
  Serial.println("SCD41 ready!");
}

void readSCD41() {
  bool dataReady = false;
  uint16_t newCO2;
  float newTemp, newHum;

  int16_t err = scd4x.getDataReadyStatus(dataReady);
  if (err) {
    Serial.printf("getDataReadyStatus error: %d\n", err);
    return;
  }

  if (dataReady) {
    err = scd4x.readMeasurement(newCO2, newTemp, newHum);
    if (!err && newCO2 != 0) {
      co2 = newCO2;
      temp = newTemp;
      hum = newHum;
      haveCO2 = true;
      Serial.printf("CO₂: %u ppm | Temp: %.2f °C | Hum: %.2f %%RH\n", co2, temp, hum);
    } else if (newCO2 == 0) {
      Serial.println("SCD41 warming up...");
    } else {
      Serial.printf("readMeasurement error: %d\n", err);
    }
  }
}

// =======================
// --- WIFI + HTTP ---
// =======================
void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.println("Connecting WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID);   // open network (no password)

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed.");
  }
}

void sendPOST() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    lastPostResult = "P F";
    Serial.println("POST skipped: no WiFi.");
    return;
  }

  // Build body string with prefix and sensor data
  String body = PREFIX;
  body += " co2=";
  body += String(haveCO2 ? co2 : 0);
  body += ",temp=";
  body += String(haveCO2 ? temp : 0.0, 1);
  body += ",hum=";
  body += String(haveCO2 ? hum : 0.0, 1);
  body += ",db=";
  body += String(haveDB ? db : 0);
  body += ",rssi=";
  body += String(WiFi.RSSI());

  Serial.print("[POST] Body: ");
  Serial.println(body);

  HTTPClient http;
  http.begin(POST_URL);
  http.addHeader("Content-Type", "text/plain");

  int code = http.POST(body);

  // ---------------------------
  //  ADDED: Print error details
  // ---------------------------
  Serial.print("[POST] Status: ");
  Serial.print(code);
  Serial.print(" (");
  Serial.print( http.errorToString(code).c_str() );
  Serial.println(")");

  if (code >= 200 && code < 300) {
    lastPostResult = "P OK";
  } else {
    lastPostResult = "P F";
  }

  http.end();
}


bool fetchProbeConfig() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[CFG] No WiFi, skipping config fetch");
    return false;
  }

  String url = String("http://34.160.35.22/api/probeconfig?prefix=") + PREFIX;
  Serial.println("[CFG] Fetching: " + url);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();

  if (code <= 0) {
    Serial.printf("[CFG] HTTP error: %d (%s)\n", code, http.errorToString(code).c_str());
    http.end();
    return false;
  }

  if (code != 200) {
    Serial.printf("[CFG] Server returned code %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();
  Serial.println("[CFG] Received: " + payload);

  // ----------------------------
  // Parse JSON manually (fast + minimal)
  // ----------------------------
  int idx = payload.indexOf("\"refresh\"");
  if (idx < 0) {
    Serial.println("[CFG] No 'refresh' field in JSON");
    return false;
  }

  idx = payload.indexOf(":", idx);
  if (idx < 0) return false;

  int end = payload.indexOf(",", idx);
  if (end < 0) end = payload.indexOf("}", idx);
  if (end < 0) return false;

  String refreshStr = payload.substring(idx + 1, end);
  refreshStr.trim();
  int newFreq = refreshStr.toInt();

  if (newFreq <= 0 || newFreq > 600) {
    Serial.printf("[CFG] Invalid refresh value: %d\n", newFreq);
    return false;
  }

  // ----------------------------
  // Apply + Save to Flash
  // ----------------------------
  updateFrequencySeconds = newFreq;

  prefs.begin("config", false);
  prefs.putUInt("updateFreq", updateFrequencySeconds);
  prefs.end();

  Serial.printf("[CFG] Refresh interval updated to %d seconds and saved.\n", newFreq);

#if HAS_SCREEN
  display.clearDisplay();
  display.setCursor(0, 0);
  display.setTextSize(2);
  display.print("CFG OK");
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.printf("refresh=%d", updateFrequencySeconds);
  display.display();
  delay(800);
#endif

  return true;
}


// =======================
// --- DISPLAY ---
// =======================
#if HAS_SCREEN
void updateDisplay() {
  display.clearDisplay();

  // --- Big Decibel Display (top) ---
  if (haveDB) {
    display.setTextSize(6);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.printf("%3d", db);
    display.setTextSize(2);
    display.setCursor(104, 0);
    display.print("dB");
  } else {
    display.setTextSize(3);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 20);
    display.print("-- dB");
  }

  // --- Bottom Lines ---

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Line 1: CO2 / Temp / Humidity
 display.setCursor(0, 46);
  if (haveCO2) {
      display.printf("C:%u  T:%.1f  H:%d", co2, temp, (int)hum);
  } else {
      display.print("C:--  T:--.-  H:--");
  }


    // Line 2: short IP, PREFIX, POST status
  display.setCursor(0, 56);

  String ipShort = "--";

  if (WiFi.status() == WL_CONNECTED) {

      IPAddress ip = WiFi.localIP();
      ipShort = String(ip[2]) + "." + String(ip[3]);   // show only last 2 octets

      // Print full IP to Serial (for debugging)
      Serial.print("WiFi connected, IP: ");
      Serial.println(ip);

      // Fetch remote config once per boot
      static bool configFetched = false;
      if (!configFetched) {
        if (fetchProbeConfig()) {
          configFetched = true;
        }
      }
  }


  display.printf("%s  %s  %s",
                 ipShort.c_str(),
                 PREFIX,
                 lastPostResult.c_str());

  display.display();
}
#endif

// =======================
// --- UART OUTPUT ---
// =======================
void sendUARTData() {
  if (haveCO2 || haveDB) {
    char buffer[96];
    snprintf(buffer, sizeof(buffer),
             "CO2:%u,Temp:%.2f,Hum:%.2f,Sound:%u\n",
             co2, temp, hum, db);
    extSerial.print(buffer);
  }
}

// =======================
// --- UART COMMAND HANDLER ---
// =======================
void checkUARTCommands() {
  if (Serial.available() || extSerial.available()) {
    String cmd;
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') break;
      cmd += c;
    }
    while (extSerial.available()) {
      char c = extSerial.read();
      if (c == '\n' || c == '\r') break;
      cmd += c;
    }

    cmd.trim();
    if (cmd.startsWith("SET UPDATE FREQUENCY")) {
      int newFreq = cmd.substring(21).toInt();
      if (newFreq > 0 && newFreq <= 600) {
        updateFrequencySeconds = newFreq;

        // Save to flash
        prefs.begin("config", false);
        prefs.putUInt("updateFreq", updateFrequencySeconds);
        prefs.end();

        char ack[64];
        snprintf(ack, sizeof(ack),
                 "UPDATE FREQUENCY SET TO: %lus (saved)\n",
                 updateFrequencySeconds);
        Serial.print(ack);
        extSerial.print(ack);
      } else {
        Serial.println("Invalid frequency (1–600 seconds)");
      }
    }
  }
}

// =======================
// --- SETUP ---
// =======================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Load saved frequency
  prefs.begin("config", true);
  updateFrequencySeconds = prefs.getUInt("updateFreq", updateFrequencySeconds);
  prefs.end();
  Serial.printf("Loaded update frequency: %lu s\n", updateFrequencySeconds);

  // Start I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);  // 100 kHz is safe for all

  // Scan I2C (before display init, as your original code did)
  scanI2CBus();

  // UART
  extSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  Serial.println("External UART ready.");

#if HAS_SCREEN
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED init failed");
    while (true) { delay(1000); }
  }
#if BIG_BOY
  display.setRotation(2);
#endif
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("Freq: %lus", updateFrequencySeconds);
  display.setCursor(0, 10);
  display.print("I2C: ");
  display.print(i2cSummary);
  display.display();
  delay(1000);
#endif

  // Sensors
  initDecibel();
  initSCD41();
  Serial.println("Sensors initialized.\n");

#if HAS_SCREEN
  display.clearDisplay();
  display.setCursor(0, 0);
  if (scdFound) {
    display.println("SCD41 found");
  } else {
    display.println("SCD41 NOT found");
  }
  if (decibelFound) {
    display.println("DBM found");
  } else {
    display.println("DBM NOT found");
  }
  display.println("Sensors initialized");
  display.display();
  delay(1000);
#endif
}

// =======================
// --- LOOP ---
// =======================
void loop() {
  static uint32_t lastScreen = 0;
  static uint32_t lastUart = 0;
  static uint32_t lastPost = 0;

  checkUARTCommands();

  if (millis() - lastScreen >= 1 * SECONDS) {
    lastScreen = millis();
    readSCD41();
    readDecibel();
#if HAS_SCREEN
    updateDisplay();
#endif
  }

  if (millis() - lastUart >= updateFrequencySeconds * SECONDS) {
    lastUart = millis();
    sendUARTData();
  }

  if (millis() - lastPost >= 5000) {  // POST every 5 seconds
    lastPost = millis();
    sendPOST();
  }

  delay(50);
}
