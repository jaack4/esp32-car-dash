#pragma once
#include <Adafruit_SSD1306.h>

// BLE link to the Car Dash iPhone app. The app writes '|' separated UTF-8 messages to the
// data characteristic:
//   W|<unix time>|<UTC offset s>|<city>|<temp>|<WMO code>|<is day 0/1>|<high>|<low>|<wind>|<rain min>
//       rain min: minutes until rain starts, 0 = raining now, -1 = none in the next 2 hours
//   B|<battery %>|<charging 0/1>
//   R|<reverb 0/1>|<concert/club>        (legacy R|0 and R|1 keep the current preset)
//   EQ|<on 0/1>|<bass dB>|<mid dB>|<treble dB>   (-12..12)
//   BASS|<on 0/1>|<level 1-3>             bass boost: light, medium, heavy
//   WIDE|<on 0/1>|<level 1-3>             stereo widener: subtle, wide, extra
// Reading (or a notification from) the characteristic gives the state of every effect:
//   FX|<reverb available>|<reverb on>|<concert/club>|<eq on>|<bass>|<mid>|<treble>|
//      <bass boost on>|<level>|<widener on>|<level>
#define DASH_SERVICE_UUID "6d0f0001-8f4c-4c38-9d51-6a4b1b0c2a10"
#define DASH_DATA_UUID    "6d0f0002-8f4c-4c38-9d51-6a4b1b0c2a10"

void startPhoneLink(const char *name);
void publishEffectsStatus();
// handle a message as if the app had sent it (used for testing over serial)
void handlePhoneMessage(const String &message);
bool hasWeather();
// Consume one event when a forecast changes from no rain to rain expected.
bool takeRainAlert();
void drawWeatherPage(Adafruit_SSD1306 &display);
// one-line version for the top (yellow) strip: clock, city (or rain alert), icon, temperature
void drawWeatherStrip(Adafruit_SSD1306 &display);
// phone battery icon and percentage at (x, y), about 42 px wide; draws nothing if unknown
void drawPhoneBattery(Adafruit_SSD1306 &display, int x, int y);
