#include <M5StickCPlus.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

float airlyLat = 0;
float airlyLng = 0;
String cityName = "";
bool locationResolved = false;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

bool firstRun = true;
TFT_eSprite Disbuff = TFT_eSprite(&M5.Lcd);

unsigned long loopTime, startTime = 0;

HTTPClient http;

float caqi = 0, pm25 = 0, pm10 = 0;
float pressure = 0, humidity = 0, temperature = 0;
int caqiR = 255, caqiG = 255, caqiB = 255;
String caqiLevel = "";

bool screenChanged = true;
int lastMinute = -1;
int rateLimitRemaining = -1;
bool dataAvailable = false;

const int brightnessLevels[] = {10, 30, 50, 75, 100};
int brightnessIdx = 4;  // Start at max

void drawSunIcon() {
  int sunX = 90, sunY = 9;
  int r = 6;
  // Clear sun area
  M5.Lcd.fillRect(sunX - r, sunY - r, r * 2 + 1, r * 2 + 1, BLACK);
  // Center circle
  M5.Lcd.fillCircle(sunX, sunY, 2, YELLOW);
  // Draw rays based on brightnessIdx: 0=0, 1=2, 2=4, 3=6, 4=8
  if (brightnessIdx >= 1) {
    // Top + Bottom
    M5.Lcd.drawLine(sunX, sunY - 5, sunX, sunY - 3, YELLOW);
    M5.Lcd.drawLine(sunX, sunY + 3, sunX, sunY + 5, YELLOW);
  }
  if (brightnessIdx >= 2) {
    // Left + Right
    M5.Lcd.drawLine(sunX - 5, sunY, sunX - 3, sunY, YELLOW);
    M5.Lcd.drawLine(sunX + 3, sunY, sunX + 5, sunY, YELLOW);
  }
  if (brightnessIdx >= 3) {
    // Top-left + Bottom-right
    M5.Lcd.drawLine(sunX - 4, sunY - 4, sunX - 2, sunY - 2, YELLOW);
    M5.Lcd.drawLine(sunX + 2, sunY + 2, sunX + 4, sunY + 4, YELLOW);
  }
  if (brightnessIdx >= 4) {
    // Top-right + Bottom-left
    M5.Lcd.drawLine(sunX + 2, sunY - 2, sunX + 4, sunY - 4, YELLOW);
    M5.Lcd.drawLine(sunX - 4, sunY + 4, sunX - 2, sunY + 2, YELLOW);
  }
}

void drawBatteryIcon() {
  float batV = M5.Axp.GetBatVoltage();
  int batLevel = constrain((int)((batV - 3.2f) / 0.9f * 5), 0, 5);
  int batX = 100, batY = 3;
  M5.Lcd.drawRoundRect(batX, batY, 28, 12, 2, WHITE);
  M5.Lcd.fillRoundRect(batX + 28, batY + 3, 3, 6, 1, WHITE);
  for (int i = 0; i < batLevel; i++) {
    uint16_t barColor;
    if (batLevel == 1) barColor = RED;
    else if (batLevel == 2) barColor = YELLOW;
    else barColor = WHITE;
    M5.Lcd.fillRoundRect(batX + 2 + i * 5, batY + 2, 4, 8, 1, barColor);
  }
}

void fetchLocation() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient locHttp;
  locHttp.begin("http://ip-api.com/json/?fields=lat,lon,city");
  int httpCode = locHttp.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = locHttp.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      airlyLat = doc["lat"].as<float>();
      airlyLng = doc["lon"].as<float>();
      cityName = doc["city"].as<String>();
      locationResolved = true;
      M5.Lcd.println("Location: " + cityName);
    }
  }
  locHttp.end();

  if (!locationResolved) {
    M5.Lcd.println("Location lookup failed");
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(0);
  M5.Lcd.setTextSize(1);

  M5.Lcd.printf("Connecting to %s", ssid);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    M5.Lcd.print(".");
  }
  M5.Lcd.println("\nCONNECTED!");
  M5.Lcd.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  M5.Axp.ScreenBreath(brightnessLevels[brightnessIdx]);

  fetchLocation();
}

float getValueByName(JsonArray values, const char* name) {
  for (JsonObject v : values) {
    if (strcmp(v["name"], name) == 0) {
      return v["value"].as<float>();
    }
  }
  return 0;
}

void fetchAirQuality() {
  loopTime = millis();
  if (firstRun || (loopTime - startTime) >= 900000) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!locationResolved) { fetchLocation(); if (!locationResolved) return; }

    String url = String("https://airapi.airly.eu/v2/measurements/point?lat=") + String(airlyLat, 6) + "&lng=" + String(airlyLng, 6);
    http.begin(url);
    http.addHeader("apikey", apikey);
    const char* collectHeaders[] = {"X-RateLimit-Remaining-Day"};
    http.collectHeaders(collectHeaders, 1);
    int httpCode = http.GET();

    if (http.hasHeader("X-RateLimit-Remaining-Day")) {
      rateLimitRemaining = http.header("X-RateLimit-Remaining-Day").toInt();
    }

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        http.end();
        return;
      }

      JsonArray values = doc["current"]["values"].as<JsonArray>();
      pm25 = getValueByName(values, "PM25");
      pm10 = getValueByName(values, "PM10");
      pressure = getValueByName(values, "PRESSURE");
      humidity = getValueByName(values, "HUMIDITY");
      temperature = getValueByName(values, "TEMPERATURE");

      JsonObject index = doc["current"]["indexes"][0];
      caqi = index["value"].as<float>();
      caqiLevel = index["level"].as<String>();
      const char* color = index["color"];

      if (color && strlen(color) >= 2) {
        long number = (long)strtol(&color[1], NULL, 16);
        caqiR = number >> 16;
        caqiG = (number >> 8) & 0xFF;
        caqiB = number & 0xFF;
      }

      startTime = loopTime;
      firstRun = false;
      dataAvailable = true;
      screenChanged = true;
    } else {
      if (httpCode == 429 && !dataAvailable) {
        screenChanged = true;
      }
      firstRun = false;
      startTime = loopTime;
    }
    http.end();
  }
}

void drawClock() {
  M5.Lcd.fillRect(8, 28, 120, 32, BLACK);
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    M5.Lcd.setTextSize(4);
    M5.Lcd.setTextColor(WHITE, BLACK);
    M5.Lcd.setCursor(8, 28);
    M5.Lcd.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
}

void displayAirScreen() {
  // Screen is 135x240 in portrait (rotation 0)
  M5.Lcd.fillScreen(BLACK);

  // --- Title ---
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setCursor(4, 4);
  if (cityName.length() > 0) {
    M5.Lcd.print(cityName);
  } else {
    M5.Lcd.print("AIR QUALITY");
  }

  drawSunIcon();
  drawBatteryIcon();

  // --- Large clock ---
  drawClock();

  if (!dataAvailable) {
    // Show placeholders
    const int m = 4;
    const int gap = 4;
    const int tileW = (135 - 2 * m - gap) / 2;
    const int tileH = 54;
    const int r = 6;
    const int leftX = m;
    const int rightX = m + tileW + gap;
    const int row1Y = 68;
    const int row2Y = row1Y + tileH + gap;
    uint16_t gray = M5.Lcd.color565(60, 60, 60);

    M5.Lcd.fillRoundRect(leftX, row1Y, tileW, tileH, r, gray);
    M5.Lcd.fillRoundRect(rightX, row1Y, tileW, tileH, r, gray);
    M5.Lcd.fillRoundRect(leftX, row2Y, tileW, tileH, r, gray);
    M5.Lcd.fillRoundRect(rightX, row2Y, tileW, tileH, r, gray);

    M5.Lcd.setTextColor(WHITE, gray);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setCursor(leftX + 6, row1Y + 4);
    M5.Lcd.print("TEMP");
    M5.Lcd.setCursor(leftX + 18, row1Y + 26);
    M5.Lcd.print("--");

    M5.Lcd.setCursor(rightX + 6, row1Y + 4);
    M5.Lcd.print("HUMI");
    M5.Lcd.setCursor(rightX + 18, row1Y + 26);
    M5.Lcd.print("--");

    M5.Lcd.setCursor(leftX + 6, row2Y + 4);
    M5.Lcd.print("CAQI");
    M5.Lcd.setCursor(leftX + 18, row2Y + 26);
    M5.Lcd.print("--");

    M5.Lcd.setCursor(rightX + 6, row2Y + 4);
    M5.Lcd.print("PRES");
    M5.Lcd.setCursor(rightX + 18, row2Y + 26);
    M5.Lcd.print("--");

    const int botY = row2Y + tileH + gap;
    const int botW = 135 - 2 * m;
    const int botH = 240 - botY - m;
    M5.Lcd.fillRoundRect(leftX, botY, botW, botH, r, gray);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setTextSize(1);
    if (rateLimitRemaining == 0) {
      M5.Lcd.setCursor(leftX + 12, botY + 8);
      M5.Lcd.print("RATE LIMIT REACHED");
      M5.Lcd.setCursor(leftX + 12, botY + 24);
      M5.Lcd.print("Try again tomorrow");
    } else {
      M5.Lcd.setCursor(leftX + 20, botY + 14);
      M5.Lcd.print("Loading...");
    }
    return;
  }

  // --- Tile layout ---
  const int m = 4;
  const int gap = 4;
  const int tileW = (135 - 2 * m - gap) / 2;
  const int tileH = 54;
  const int r = 6;
  const int leftX = m;
  const int rightX = m + tileW + gap;
  const int row1Y = 68;
  const int row2Y = row1Y + tileH + gap;

  // Draw all blue tiles first
  M5.Lcd.fillRoundRect(leftX, row1Y, tileW, tileH, r, BLUE);
  M5.Lcd.fillRoundRect(rightX, row1Y, tileW, tileH, r, BLUE);
  M5.Lcd.fillRoundRect(leftX, row2Y, tileW, tileH, r, BLUE);
  M5.Lcd.fillRoundRect(rightX, row2Y, tileW, tileH, r, BLUE);

  // Row 1: TEMP | HUMID
  M5.Lcd.setTextColor(WHITE, BLUE);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(leftX + 6, row1Y + 8);
  M5.Lcd.print("TEMP");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(leftX + 8, row1Y + 30);
  M5.Lcd.printf("%.0f", temperature);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(rightX + 2, row1Y + 8);
  M5.Lcd.print("HUMID");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(rightX + 8, row1Y + 30);
  M5.Lcd.printf("%.0f", humidity);

  // Row 2: CAQI | PRES
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(leftX + 6, row2Y + 8);
  M5.Lcd.print("CAQI");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(leftX + 8, row2Y + 30);
  M5.Lcd.printf("%.0f", caqi);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(rightX + 6, row2Y + 8);
  M5.Lcd.print("PRES");
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(rightX + 4, row2Y + 30);
  M5.Lcd.printf("%.0f", pressure);

  // --- Bottom bar: AIR POLLUTION + level ---
  const int botY = row2Y + tileH + gap;
  const int botW = 135 - 2 * m;
  const int botH = 240 - botY - m;
  M5.Lcd.fillRoundRect(leftX, botY, botW, botH, r, BLUE);

  M5.Lcd.setTextColor(WHITE);
  M5.Lcd.setTextSize(1);
  int labelW = 13 * 6;
  M5.Lcd.setCursor(leftX + (botW - labelW) / 2, botY + 8);
  M5.Lcd.print("AIR POLLUTION");

  M5.Lcd.setTextColor(Disbuff.color565(caqiR, caqiG, caqiB));
  M5.Lcd.setTextSize(2);
  int levelW = caqiLevel.length() * 12;
  M5.Lcd.setCursor(leftX + (botW - levelW) / 2, botY + 24);
  M5.Lcd.print(caqiLevel);
}



void loop() {
  M5.update();

  // Button A adjusts brightness
  if (M5.BtnA.wasPressed()) {
    brightnessIdx = (brightnessIdx + 1) % 5;
    M5.Axp.ScreenBreath(brightnessLevels[brightnessIdx]);
    drawSunIcon();
  }

  fetchAirQuality();
  struct tm t;
  if (getLocalTime(&t)) {
    if (t.tm_min != lastMinute) {
      lastMinute = t.tm_min;
      drawClock();
    }
  }
  if (screenChanged) {
    displayAirScreen();
    screenChanged = false;
  }

  delay(100);
}
