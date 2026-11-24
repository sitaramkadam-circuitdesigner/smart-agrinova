#include <Wire.h>
#include <DHT.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Wi-Fi name/password and ThingSpeak key live in secrets.h (not on GitHub).
// Copy secrets.h.example to secrets.h and edit it.
#include "secrets.h"

//Pins
#define DHT_PIN     18
#define DHT_TYPE    DHT22
#define RAIN_PIN    16
#define RAIN_AO_PIN 35
#define WATER_PIN   32
#define SOIL_PIN    34
#define RELAY_PIN   23
#define SDA_PIN     21
#define SCL_PIN     22

//LCD (16x2)
#define LCD_COLS    16
#define LCD_ROWS    2

//Rain sensor
#define RAIN_WET_LEVEL  LOW
#define RAIN_DRY_RAW    4095
#define RAIN_WET_RAW    1500
#define RAIN_ZERO_WHEN_DRY 0

//Calibration
#define SOIL_DRY_RAW     3200
#define SOIL_WET_RAW     1300
#define WATER_EMPTY_RAW  0
#define WATER_FULL_RAW   2000

//Irrigation rules
#define SOIL_START_PCT   35
#define SOIL_STOP_PCT    60
#define TANK_MIN_PCT     20
#define PUMP_MAX_MS      300000UL
#define PUMP_REST_MS     600000UL

WebServer server(80);

//ThingSpeak (cloud)
unsigned long lastCloud = 0;

DHT dht(DHT_PIN, DHT_TYPE);
hd44780_I2Cexp lcd;

float g_tempC = 0;
float g_hum = 0;
int   g_soilPct = 0;
int   g_tankPct = 0;
int   g_rainPct = 0;
bool  g_rain = false;

bool pumpOn = false;
unsigned long pumpStart = 0;
unsigned long restUntil = 0;
unsigned long lastUpdate = 0;
const char* statusText = "Idle";

//Wi-Fi web pages
void handleRoot() {
  String h = "<html><head><meta http-equiv='refresh' content='5'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "</head><body style='font-family:sans-serif;font-size:20px'>";
  h += "<h2>Smart Irrigation</h2>";
  h += "<p>Temperature: " + String(g_tempC, 1) + " C</p>";
  h += "<p>Humidity: " + String(g_hum, 0) + " %</p>";
  h += "<p>Soil moisture: " + String(g_soilPct) + " %</p>";
  h += "<p>Water tank: " + String(g_tankPct) + " %</p>";
  h += "<p>Rain: " + String(g_rain ? "YES" : "NO") + " " + String(g_rainPct) + " %</p>";
  h += "<p>Pump/Valve: " + String(pumpOn ? "ON" : "OFF") + "</p>";
  h += "<p>Status: " + String(statusText) + "</p>";
  h += "</body></html>";
  server.send(200, "text/html", h);
}

void handleData() {
  String j = "{\"temp\":" + String(g_tempC, 1);
  j += ",\"hum\":" + String(g_hum, 0);
  j += ",\"soil\":" + String(g_soilPct);
  j += ",\"tank\":" + String(g_tankPct);
  j += ",\"rainpct\":" + String(g_rainPct);
  j += ",\"rain\":" + String(g_rain ? "true" : "false");
  j += ",\"pump\":" + String(pumpOn ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

//Send data to ThingSpeak
void sendToCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  String url = "https://api.thingspeak.com/update?api_key=" + String(TS_API_KEY);
  url += "&field1=" + String(g_tempC, 1);
  url += "&field2=" + String(g_hum, 0);
  url += "&field3=" + String(g_soilPct);
  url += "&field4=" + String(g_tankPct);
  url += "&field5=" + String(g_rainPct);
  url += "&field6=" + String(g_rain ? 1 : 0);
  url += "&field7=" + String(pumpOn ? 1 : 0);
  if (http.begin(client, url)) {
    int code = http.GET();
    String body = (code > 0) ? http.getString() : "";
    Serial.printf("ThingSpeak: HTTP %d, entry %s\n", code, body.c_str());  // 200 and entry > 0 = OK
    http.end();
  } else {
    Serial.println("ThingSpeak: begin failed");
  }
}

int toPercent(int raw, int rawLow, int rawHigh) {
  int p = map(raw, rawLow, rawHigh, 0, 100);
  return constrain(p, 0, 100);
}

// Relay: ON = drive LOW, OFF = release pin (INPUT)
void relayWrite(bool on) {
  if (on) { pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, LOW); }
  else    { pinMode(RELAY_PIN, INPUT); }
}

void setPump(bool on) {
  pumpOn = on;
  relayWrite(on);
  if (on) pumpStart = millis();
}

void printLine(uint8_t row, const char* text) {
  char buf[LCD_COLS + 1];
  snprintf(buf, sizeof(buf), "%-*s", LCD_COLS, text);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

void setup() {
  relayWrite(false);

  Serial.begin(115200);
  pinMode(RAIN_PIN, INPUT);
  analogReadResolution(12);

  Wire.begin(SDA_PIN, SCL_PIN);

  int status = lcd.begin(LCD_COLS, LCD_ROWS);
  if (status) {
    Serial.printf("LCD init failed: %d\n", status);
    hd44780::fatalError(status);
  }

  lcd.backlight();
  printLine(0, "Smart Irrigation");
  printLine(1, "Starting...");

  dht.begin();

  //Wi-Fi start
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  int n = WiFi.scanNetworks();
  Serial.printf("Networks found: %d\n", n);
  for (int i = 0; i < n; i++) Serial.println(WiFi.SSID(i));

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  printLine(1, "Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nOpen this on your phone: http://");
    Serial.println(WiFi.localIP());
    printLine(0, "WiFi connected");
    printLine(1, WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWi-Fi not connected, running without it.");
    printLine(1, "Wi-Fi failed");
  }
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();

  delay(4000);
  lcd.clear();
}

void loop() {
  server.handleClient();

  if (millis() - lastUpdate < 2000) return;
  lastUpdate = millis();

  // Wi-Fi retry: once every 30 seconds only
  static unsigned long lastTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastTry > 30000) {
    lastTry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  //Read sensors
  float tempC = dht.readTemperature();
  float hum   = dht.readHumidity();
  bool dhtOk  = !(isnan(tempC) || isnan(hum));

  bool raining = (digitalRead(RAIN_PIN) == RAIN_WET_LEVEL);
  int rainRaw  = analogRead(RAIN_AO_PIN);
  int rainPct  = toPercent(rainRaw, RAIN_DRY_RAW, RAIN_WET_RAW);
#if RAIN_ZERO_WHEN_DRY
  if (!raining) rainPct = 0;
#endif

  int soilRaw  = analogRead(SOIL_PIN);
  int soilPct  = toPercent(soilRaw, SOIL_DRY_RAW, SOIL_WET_RAW);

  int waterRaw = analogRead(WATER_PIN);
  int tankPct  = toPercent(waterRaw, WATER_EMPTY_RAW, WATER_FULL_RAW);

  g_tempC = dhtOk ? tempC : 0;
  g_hum = dhtOk ? hum : 0;
  g_soilPct = soilPct;
  g_tankPct = tankPct;
  g_rainPct = rainPct;
  g_rain = raining;

  //Control logic
  bool inRest = (millis() < restUntil);

  if (pumpOn) {
    if (raining) {
      setPump(false); statusText = "Stopped: rain";
    } else if (tankPct < TANK_MIN_PCT) {
      setPump(false); statusText = "Stopped:tank low";
    } else if (soilPct >= SOIL_STOP_PCT) {
      setPump(false); statusText = "Soil wet enough";
    } else if (millis() - pumpStart > PUMP_MAX_MS) {
      setPump(false); restUntil = millis() + PUMP_REST_MS;
      statusText = "Max time, rest";
    } else {
      statusText = "Watering...";
    }
  } else {
    if (soilPct < SOIL_START_PCT) {
      if (raining)                     statusText = "Dry but raining";
      else if (tankPct < TANK_MIN_PCT) statusText = "Dry, tank low";
      else if (inRest)                 statusText = "Resting";
      else { setPump(true);            statusText = "Watering..."; }
    } else {
      statusText = "Soil OK";
    }
  }

  //LCD: 2 pages, switches every 2 seconds
  static bool page = false;
  page = !page;
  char line[32];

  if (!page) {
    if (dhtOk) snprintf(line, sizeof(line), "T:%.1fC H:%.0f%%", tempC, hum);
    else       snprintf(line, sizeof(line), "DHT22 ERROR");
    printLine(0, line);
    snprintf(line, sizeof(line), "S:%d%% W:%d%%", soilPct, tankPct);
    printLine(1, line);
  } else {
    snprintf(line, sizeof(line), "Rain:%s %d%%", raining ? "YES" : "NO", rainPct);
    printLine(0, line);
    snprintf(line, sizeof(line), "Pump: %s", pumpOn ? "ON" : "OFF");
    printLine(1, line);
  }

  //Serial monitor
  Serial.printf("T=%.1fC H=%.0f%% soil=%d%% (raw %d) tank=%d%% (raw %d) rain=%d rain%%=%d (raw %d) pump=%d | %s\n",
                tempC, hum, soilPct, soilRaw, tankPct, waterRaw, raining, rainPct, rainRaw, pumpOn, statusText);

  //Send to ThingSpeak every 20 seconds
  if (millis() - lastCloud > 20000) {
    lastCloud = millis();
    sendToCloud();
  }
}
