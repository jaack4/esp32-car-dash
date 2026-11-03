#include "phone_link.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include "effects_output.h"

struct Weather {
  bool valid = false;
  uint32_t receivedAt = 0;   // millis()
  int64_t localTime = 0;     // phone's local time (unix + UTC offset) at receivedAt
  char city[24] = "";
  int temp = 0, code = 0, high = 0, low = 0, wind = 0;
  int rainMinutes = -1;      // at receivedAt; -1 = no rain expected
  bool isDay = true;
};

struct Battery {
  bool valid = false;
  uint32_t receivedAt = 0;
  int percent = 0;
  bool charging = false;
};

static const uint32_t STALE_MS = 2 * 3600 * 1000UL;   // stop showing alerts/battery after this

static portMUX_TYPE dataLock = portMUX_INITIALIZER_UNLOCKED;
static Weather weather;
static Battery battery;
static bool rainAlertPending = false;
static BLECharacteristic *dashData = nullptr;
static std::atomic<bool> phoneConnected{false};

static String effectsStatus() {
  ToneSettings t = audioEffects.tone();
  char value[80];
  snprintf(value, sizeof(value), "FX|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d", audioEffects.available(),
           audioEffects.enabled(), reverbPresetName(audioEffects.preset()), t.eqOn, t.bass, t.mid, t.treble,
           t.bassBoostOn, t.bassBoostLevel, t.widenerOn, t.widthLevel);
  return String(value);
}

void publishEffectsStatus() {
  if (!dashData) return;
  dashData->setValue(effectsStatus());
  if (phoneConnected.load()) dashData->notify();
}

// ---------- BLE ----------

static void parseMessage(const String &message) {
  bool enabled = false;
  ReverbPreset preset = audioEffects.preset();
  if (parseReverbCommand(message.c_str(), enabled, preset)) {
    bool accepted = audioEffects.setMode(enabled, preset);
    Serial.printf("Reverb request %s (%s): %s\n", enabled ? "ON" : "OFF",
                  reverbPresetName(preset), accepted ? "accepted" : "unavailable");
    publishEffectsStatus();
    return;
  }
  ToneSettings tone = audioEffects.tone();
  if (parseToneCommand(message.c_str(), tone)) {
    audioEffects.setTone(tone);
    Serial.printf("Effects request: %s\n", message.c_str());
    publishEffectsStatus();
    return;
  }
  char buf[160];
  strlcpy(buf, message.c_str(), sizeof(buf));
  char *f[12];
  int n = 0;
  char *save;
  for (char *p = strtok_r(buf, "|", &save); p && n < 12; p = strtok_r(nullptr, "|", &save)) f[n++] = p;

  if (n == 11 && strcmp(f[0], "W") == 0) {
    Weather w;
    w.valid = true;
    w.receivedAt = millis();
    w.localTime = atoll(f[1]) + atol(f[2]);
    strlcpy(w.city, f[3], sizeof(w.city));
    w.temp = atoi(f[4]);
    w.code = atoi(f[5]);
    w.isDay = atoi(f[6]) != 0;
    w.high = atoi(f[7]);
    w.low = atoi(f[8]);
    w.wind = atoi(f[9]);
    w.rainMinutes = atoi(f[10]);
    portENTER_CRITICAL(&dataLock);
    // Repeated weather updates for the same rain episode should not keep flashing.
    if (w.rainMinutes < 0) rainAlertPending = false;
    else if (!weather.valid || weather.rainMinutes < 0) rainAlertPending = true;
    weather = w;
    portEXIT_CRITICAL(&dataLock);
    Serial.printf("Weather: %s %d F, code %d, rain %d min\n", w.city, w.temp, w.code, w.rainMinutes);
  } else if (n == 3 && strcmp(f[0], "B") == 0) {
    Battery b;
    b.valid = true;
    b.receivedAt = millis();
    b.percent = constrain(atoi(f[1]), 0, 100);
    b.charging = atoi(f[2]) != 0;
    portENTER_CRITICAL(&dataLock);
    battery = b;
    portEXIT_CRITICAL(&dataLock);
    Serial.printf("Phone battery: %d%%%s\n", b.percent, b.charging ? " (charging)" : "");
  } else {
    Serial.printf("Ignoring phone message: %s\n", message.c_str());
  }
}

void handlePhoneMessage(const String &message) {
  parseMessage(message);
}

class DataCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override { parseMessage(c->getValue()); }
  void onRead(BLECharacteristic *c) override { c->setValue(effectsStatus()); }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    phoneConnected.store(true);
    Serial.println("Car Dash app connected");
  }
  void onDisconnect(BLEServer *) override {
    phoneConnected.store(false);
    Serial.println("Car Dash app disconnected");
    BLEDevice::startAdvertising();   // let the app find us again
  }
};

void startPhoneLink(const char *name) {
  BLEDevice::init(name);
  BLEDevice::setMTU(185);   // room for the effects status in a single notification
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *service = server->createService(DASH_SERVICE_UUID);
  BLECharacteristic *data = service->createCharacteristic(
      DASH_DATA_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR |
                          BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  dashData = data;
  data->addDescriptor(new BLE2902());
  data->setCallbacks(new DataCallbacks());
  service->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(DASH_SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Phone link (BLE) advertising");
}

static Weather currentWeather() {
  portENTER_CRITICAL(&dataLock);
  Weather w = weather;
  portEXIT_CRITICAL(&dataLock);
  return w;
}

bool hasWeather() {
  return currentWeather().valid;
}

bool takeRainAlert() {
  portENTER_CRITICAL(&dataLock);
  bool pending = rainAlertPending;
  rainAlertPending = false;
  portEXIT_CRITICAL(&dataLock);
  return pending;
}

// ---------- text helpers ----------

static void printRight(Adafruit_SSD1306 &d, const char *text, int y, int size = 1) {
  d.setTextSize(size);
  d.setCursor(128 - strlen(text) * 6 * size, y);
  d.print(text);
}

static void formatClock(char *out, size_t n, const Weather &w) {
  int64_t now = w.localTime + (millis() - w.receivedAt) / 1000;
  int hour = (int)(now / 3600 % 24) % 12;
  if (hour == 0) hour = 12;
  snprintf(out, n, "%d:%02d", hour, (int)(now / 60 % 60));
}

// "Rain 20m" / "Rain now", or empty when no rain is expected or the forecast is stale
static void formatRain(char *out, size_t n, const Weather &w, bool longForm) {
  out[0] = 0;
  uint32_t age = millis() - w.receivedAt;
  if (w.rainMinutes < 0 || age > STALE_MS) return;
  int left = w.rainMinutes - (int)(age / 60000);
  if (left <= 0) snprintf(out, n, "Rain now");
  else snprintf(out, n, longForm ? "Rain in %d min" : "Rain %dm", left);
}

// time since power-on, i.e. since the car started
static void formatDrive(char *out, size_t n) {
  uint32_t minutes = millis() / 60000;
  if (minutes < 60) snprintf(out, n, "Drive %lum", (unsigned long)minutes);
  else snprintf(out, n, "Drive %luh%02lu", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
}

// ---------- icons ----------

enum Icon { CLEAR, NIGHT, PARTLY, CLOUD, FOG, RAIN, SNOW, STORM };

static const char *describe(int code, Icon &icon) {
  if (code == 0)                { icon = CLEAR;  return "Clear"; }
  if (code == 1)                { icon = PARTLY; return "Mostly clear"; }
  if (code == 2)                { icon = PARTLY; return "Partly cloudy"; }
  if (code == 3)                { icon = CLOUD;  return "Overcast"; }
  if (code == 45 || code == 48) { icon = FOG;    return "Fog"; }
  if (code >= 51 && code <= 57) { icon = RAIN;   return "Drizzle"; }
  if (code >= 61 && code <= 67) { icon = RAIN;   return "Rain"; }
  if (code >= 71 && code <= 77) { icon = SNOW;   return "Snow"; }
  if (code >= 80 && code <= 82) { icon = RAIN;   return "Showers"; }
  if (code == 85 || code == 86) { icon = SNOW;   return "Snow showers"; }
  if (code >= 95)               { icon = STORM;  return "Thunderstorm"; }
  icon = CLOUD;
  return "Cloudy";
}

static Icon iconFor(const Weather &w) {
  Icon icon;
  describe(w.code, icon);
  return icon == CLEAR && !w.isDay ? NIGHT : icon;
}

static void drawSun(Adafruit_SSD1306 &d, int cx, int cy, int r, int rayIn, int rayOut) {
  d.fillCircle(cx, cy, r, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * PI / 4;
    d.drawLine(cx + rayIn * cos(a), cy + rayIn * sin(a), cx + rayOut * cos(a), cy + rayOut * sin(a), SSD1306_WHITE);
  }
}

static void drawCloud(Adafruit_SSD1306 &d, int x, int y, uint16_t color, int grow = 0) {
  d.fillCircle(x + 8, y + 14, 4 + grow, color);
  d.fillCircle(x + 13, y + 10, 5 + grow, color);
  d.fillCircle(x + 17, y + 14, 4 + grow, color);
  d.fillRect(x + 8, y + 14 - grow, 9, 5 + 2 * grow, color);
}

// 24x24 weather icon with its top-left corner at (x, y)
static void drawIcon(Adafruit_SSD1306 &d, Icon icon, int x, int y) {
  switch (icon) {
    case CLEAR: drawSun(d, x + 12, y + 12, 5, 8, 11); break;
    case NIGHT:
      d.fillCircle(x + 11, y + 12, 9, SSD1306_WHITE);
      d.fillCircle(x + 16, y + 8, 8, SSD1306_BLACK);
      break;
    case PARTLY:
      drawSun(d, x + 8, y + 8, 4, 6, 8);
      drawCloud(d, x + 2, y + 4, SSD1306_BLACK, 2);   // black outline separates cloud from sun
      drawCloud(d, x + 2, y + 4, SSD1306_WHITE);
      break;
    case CLOUD: drawCloud(d, x, y + 1, SSD1306_WHITE); break;
    case FOG:
      for (int i = 0; i < 3; i++) d.fillRect(x + 3 + (i % 2) * 2, y + 6 + i * 5, 18 - (i % 2) * 4, 2, SSD1306_WHITE);
      break;
    case RAIN:
      drawCloud(d, x, y - 3, SSD1306_WHITE);
      for (int i = 0; i < 3; i++) d.drawLine(x + 8 + i * 4, y + 18, x + 6 + i * 4, y + 22, SSD1306_WHITE);
      break;
    case SNOW:
      drawCloud(d, x, y - 3, SSD1306_WHITE);
      d.fillCircle(x + 8, y + 20, 1, SSD1306_WHITE);
      d.fillCircle(x + 12, y + 22, 1, SSD1306_WHITE);
      d.fillCircle(x + 16, y + 20, 1, SSD1306_WHITE);
      break;
    case STORM:
      drawCloud(d, x, y - 3, SSD1306_WHITE);
      d.fillTriangle(x + 13, y + 15, x + 9, y + 21, x + 13, y + 20, SSD1306_WHITE);
      d.fillTriangle(x + 12, y + 18, x + 15, y + 18, x + 11, y + 24, SSD1306_WHITE);
      break;
  }
}

// 14x12 version for the strip, top-left corner at (x, y)
static void drawSmallIcon(Adafruit_SSD1306 &d, Icon icon, int x, int y) {
  auto cloud = [&](int cx, int cy, uint16_t color, int grow) {
    d.fillCircle(cx + 3, cy + 7, 2 + grow, color);
    d.fillCircle(cx + 7, cy + 4, 3 + grow, color);
    d.fillCircle(cx + 11, cy + 7, 2 + grow, color);
    d.fillRect(cx + 3, cy + 7 - grow, 8, 3 + 2 * grow, color);
  };
  switch (icon) {
    case CLEAR: drawSun(d, x + 7, y + 6, 3, 5, 6); break;
    case NIGHT:
      d.fillCircle(x + 6, y + 6, 5, SSD1306_WHITE);
      d.fillCircle(x + 9, y + 4, 4, SSD1306_BLACK);
      break;
    case PARTLY:
      d.fillCircle(x + 4, y + 3, 3, SSD1306_WHITE);
      cloud(x + 1, y + 2, SSD1306_BLACK, 1);
      cloud(x + 1, y + 2, SSD1306_WHITE, 0);
      break;
    case CLOUD: cloud(x, y + 1, SSD1306_WHITE, 0); break;
    case FOG:
      for (int i = 0; i < 3; i++) d.drawFastHLine(x + 1 + (i % 2), y + 2 + i * 4, 12 - (i % 2) * 2, SSD1306_WHITE);
      break;
    case RAIN:
      cloud(x, y - 2, SSD1306_WHITE, 0);
      for (int i = 0; i < 3; i++) d.drawLine(x + 4 + i * 3, y + 9, x + 3 + i * 3, y + 11, SSD1306_WHITE);
      break;
    case SNOW:
      cloud(x, y - 2, SSD1306_WHITE, 0);
      for (int i = 0; i < 3; i++) d.drawPixel(x + 4 + i * 3, y + 10 + (i % 2), SSD1306_WHITE);
      break;
    case STORM:
      cloud(x, y - 2, SSD1306_WHITE, 0);
      d.drawLine(x + 8, y + 8, x + 6, y + 10, SSD1306_WHITE);
      d.drawLine(x + 6, y + 10, x + 8, y + 10, SSD1306_WHITE);
      d.drawLine(x + 8, y + 10, x + 6, y + 12, SSD1306_WHITE);
      break;
  }
}

void drawPhoneBattery(Adafruit_SSD1306 &d, int x, int y) {
  portENTER_CRITICAL(&dataLock);
  Battery b = battery;
  portEXIT_CRITICAL(&dataLock);
  if (!b.valid || millis() - b.receivedAt > STALE_MS) return;

  if (b.charging) {   // small lightning bolt
    d.drawLine(x + 2, y, x, y + 3, SSD1306_WHITE);
    d.drawLine(x, y + 3, x + 3, y + 3, SSD1306_WHITE);
    d.drawLine(x + 3, y + 3, x + 1, y + 6, SSD1306_WHITE);
  }
  int bx = x + 5;
  d.drawRect(bx, y, 11, 7, SSD1306_WHITE);
  d.fillRect(bx + 11, y + 2, 1, 3, SSD1306_WHITE);   // terminal nub
  d.fillRect(bx + 2, y + 2, (7 * b.percent + 50) / 100, 3, SSD1306_WHITE);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", b.percent);
  d.setTextSize(1);
  d.setCursor(bx + 15, y);
  d.print(buf);
}

// ---------- pages ----------

void drawWeatherPage(Adafruit_SSD1306 &d) {
  Weather w = currentWeather();
  char buf[24];

  // top (yellow) strip: city and clock
  formatClock(buf, sizeof(buf), w);
  printRight(d, buf, 4);
  bool bigCity = strlen(w.city) * 12 <= 128 - 36;   // size 2 if it fits next to the clock
  d.setTextSize(bigCity ? 2 : 1);
  d.setCursor(0, bigCity ? 0 : 4);   // size 2 at y 0 keeps descenders inside the yellow strip
  d.print(w.city);

  // icon, big temperature, high / low
  Icon icon;
  const char *text = describe(w.code, icon);
  drawIcon(d, iconFor(w), 0, 18);
  snprintf(buf, sizeof(buf), "%d", w.temp);
  d.setTextSize(3);
  d.setCursor(28, 19);
  d.print(buf);
  d.drawCircle(28 + strlen(buf) * 18 + 3, 21, 2, SSD1306_WHITE);   // degree sign
  snprintf(buf, sizeof(buf), "H %d", w.high);
  printRight(d, buf, 20);
  snprintf(buf, sizeof(buf), "L %d", w.low);
  printRight(d, buf, 32);

  // condition and wind
  d.setTextSize(1);
  d.setCursor(0, 46);
  d.print(text);
  snprintf(buf, sizeof(buf), "%d mph", w.wind);
  printRight(d, buf, 46);

  // bottom line: rain alert, else a stale-data warning, else the drive timer; battery at right
  uint32_t ageMin = (millis() - w.receivedAt) / 60000;
  formatRain(buf, sizeof(buf), w, false);
  if (!buf[0] && ageMin >= 30) snprintf(buf, sizeof(buf), "Old: %lum ago", (unsigned long)ageMin);
  if (!buf[0]) formatDrive(buf, sizeof(buf));
  d.setCursor(0, 56);
  d.print(buf);
  drawPhoneBattery(d, 86, 56);
}

void drawWeatherStrip(Adafruit_SSD1306 &d) {
  Weather w = currentWeather();
  char buf[24];

  formatClock(buf, sizeof(buf), w);
  d.setTextSize(1);
  d.setCursor(0, 4);
  d.print(buf);

  // temperature at the right edge, icon just left of it
  snprintf(buf, sizeof(buf), "%d", w.temp);
  int tempX = 128 - strlen(buf) * 6 - 4;
  d.setCursor(tempX, 4);
  d.print(buf);
  d.drawCircle(124, 5, 1, SSD1306_WHITE);   // degree sign
  int iconX = tempX - 16;
  drawSmallIcon(d, iconFor(w), iconX, 2);

  // between the clock and the icon: the rain alert if there is one, otherwise the city
  formatRain(buf, sizeof(buf), w, false);
  if (!buf[0]) strlcpy(buf, w.city, sizeof(buf));
  int maxChars = (iconX - 3 - 36) / 6;
  if ((int)strlen(buf) > maxChars) buf[maxChars] = 0;
  d.setCursor(36, 4);
  d.print(buf);
}
