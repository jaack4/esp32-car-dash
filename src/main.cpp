// Bluetooth car audio receiver: the phone streams music over Bluetooth (A2DP),
// the ESP32 sends it to a PCM5102A DAC for the car's AUX input, and the OLED
// shows the song, artist and progress. The Car Dash iPhone app adds a weather page over BLE.
// BOOT button: tap = play/pause, hold = next track, double-tap = next page
// (combo -> music -> weather).
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"
#include "phone_link.h"
#include "status_led.h"
#include "effects_output.h"

const char *BT_NAME = "ESP32 Car Audio";

// Purple regulated PCM5102A module: VIN -> USB 5V/VIN, GND -> GND, SCK -> GND.
const int I2S_BCK = 26, I2S_LRCK = 25, I2S_DIN = 27;
const int OLED_SDA = 21, OLED_SCL = 22;
const int BUTTON_PIN = 0;          // BOOT button, low while pressed
const uint32_t HOLD_MS = 600;      // press longer than this = next track
const uint32_t DOUBLE_TAP_MS = 350; // second tap within this = switch page
const uint32_t POLL_MS = 1000;     // how often to ask the phone for the playback position
const uint8_t TL_PLAY_STATUS = 5;  // AVRCP transaction label (the library uses 0-4)

Adafruit_SSD1306 display(128, 64, &Wire, -1, 100000, 100000);
I2SStream i2s;
EffectsOutput audioEffects(i2s);
BluetoothA2DPSink a2dp(audioEffects);

// Track info arrives on the Bluetooth task; the main loop copies it out under this lock.
portMUX_TYPE infoLock = portMUX_INITIALIZER_UNLOCKED;
bool selectComboPending = false;  // Bluetooth task requests; main loop changes the page
struct TrackInfo {
  char title[96] = "";
  char artist[64] = "";
  uint32_t lengthMs = 0;
  // iPhones don't send the playback position, so we keep our own clock:
  // it restarts when the song title changes and pauses while playback is paused.
  uint32_t elapsedMs = 0;     // time played before runningSince
  uint32_t runningSince = 0;  // millis() when the clock last started, 0 while paused
  bool playing = false;
  bool statusKnown = false;   // the phone has sent a play/pause status since connecting
  bool connected = false;
} info;

// phone volume (AVRCP absolute volume, 0-127); a change shows the volume overlay briefly
const uint32_t VOLUME_OVERLAY_MS = 1500;
volatile int volumeLevel = -1;
volatile uint32_t volumeChangedAt = 0, connectedAt = 0;

void onVolume(int volume) {
  volumeLevel = volume;
  volumeChangedAt = millis();
  Serial.printf("Volume %d/127\n", volume);
}

// audio bytes received from the phone, for the once-a-second log (CD quality = 176,400 B/s)
volatile uint32_t audioBytes = 0;

void onAudio(const uint8_t *, uint32_t length) {
  audioBytes += length;
}

uint32_t positionMs(const TrackInfo &t, uint32_t now) {
  uint32_t pos = t.elapsedMs + (t.runningSince ? now - t.runningSince : 0);
  return t.lengthMs ? min(pos, t.lengthMs) : pos;
}

void onMetadata(uint8_t id, const uint8_t *text) {
  portENTER_CRITICAL(&infoLock);
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      if (strcmp(info.title, (const char *)text) != 0) {   // new song: restart the clock
        info.elapsedMs = 0;
        info.runningSince = info.playing ? millis() : 0;
      }
      strlcpy(info.title, (const char *)text, sizeof(info.title));
      break;
    case ESP_AVRC_MD_ATTR_ARTIST:       strlcpy(info.artist, (const char *)text, sizeof(info.artist)); break;
    case ESP_AVRC_MD_ATTR_PLAYING_TIME: info.lengthMs = strtoul((const char *)text, nullptr, 10); break;
  }
  portEXIT_CRITICAL(&infoLock);
  Serial.printf("Track info %u: %s\n", id, (const char *)text);
}

void onPlayStatus(esp_avrc_playback_stat_t status) {
  uint32_t now = millis();
  portENTER_CRITICAL(&infoLock);
  info.playing = status == ESP_AVRC_PLAYBACK_PLAYING;
  info.statusKnown = true;
  if (info.playing && !info.runningSince) {
    info.runningSince = now;
  } else if (!info.playing && info.runningSince) {
    info.elapsedMs += now - info.runningSince;
    info.runningSince = 0;
  }
  portEXIT_CRITICAL(&infoLock);
  Serial.printf("Playback %s\n", status == ESP_AVRC_PLAYBACK_PLAYING ? "playing" : "paused/stopped");
}

// Phones that do report the position (many Android phones) correct our clock.
void onPosition(uint32_t pos) {
  portENTER_CRITICAL(&infoLock);
  info.elapsedMs = pos;
  info.runningSince = info.playing ? millis() : 0;
  portEXIT_CRITICAL(&infoLock);
  Serial.printf("Position report: %lu ms\n", (unsigned long)pos);
}

// Reply to our "get play status" request: the phone's exact position, song length and state.
// This is what keeps the time right when you seek within a song.
void onPlayStatusReply(const esp_avrc_ct_cb_param_t::avrc_ct_get_play_status_rsp_param &r) {
  if (r.song_position == 0xFFFFFFFF) return;   // phone doesn't know
  uint32_t now = millis();
  portENTER_CRITICAL(&infoLock);
  uint32_t before = positionMs(info, now);
  info.playing = r.play_status == ESP_AVRC_PLAYBACK_PLAYING;
  info.statusKnown = true;
  info.elapsedMs = r.song_position;
  info.runningSince = info.playing ? now : 0;
  if (r.song_length && r.song_length != 0xFFFFFFFF) info.lengthMs = r.song_length;
  portEXIT_CRITICAL(&infoLock);
  int32_t jump = (int32_t)r.song_position - (int32_t)before;
  if (abs(jump) > 2000) Serial.printf("Position corrected by %+ld ms (seek?)\n", (long)jump);
}

// Sits in front of the library's AVRCP handler: picks out play status replies, passes everything on.
void avrcControllerEvent(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
  if (event == ESP_AVRC_CT_PLAY_STATUS_RSP_EVT) onPlayStatusReply(param->play_status_rsp);
  ccall_app_rc_ct_callback(event, param);
}

void pollPlayStatus() {
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < POLL_MS || !a2dp.is_avrc_connected()) return;
  lastPoll = millis();
  esp_avrc_ct_send_get_play_status_cmd(TL_PLAY_STATUS);
}

void onTrackChange(uint8_t *id) {
  Serial.printf("Track change notice (id byte %u)\n", id[7]);
}

void onConnection(esp_a2d_connection_state_t state, void *) {
  portENTER_CRITICAL(&infoLock);
  info.connected = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
  if (!info.connected) info = TrackInfo();
  else selectComboPending = true;
  portEXIT_CRITICAL(&infoLock);
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) connectedAt = millis();
  Serial.printf("Bluetooth %s\n", a2dp.to_str(state));
}

void formatTime(char *out, size_t n, uint32_t ms) {
  snprintf(out, n, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
}

// Draw text at size 2 (12 px per char), scrolling it sideways if it's wider than the screen.
void drawScrolling(const char *text, int y, uint32_t now) {
  int width = strlen(text) * 12;
  int x = 0;
  if (width > 128) {
    int travel = width - 128 + 24;              // scroll past the end with a little gap
    int step = (now / 40) % (travel + 50);      // pause ~1 s at the start of each pass
    x = -max(0, min(step - 25, travel));
  }
  display.setTextSize(2);
  display.setCursor(x, y);
  display.print(text);
}

enum Page { COMBO, MUSIC, WEATHER };
Page page = COMBO;

void drawPairingHint() {
  display.setTextSize(1);
  display.setCursor(0, 4);
  display.print("Bluetooth ready");
  display.setCursor(0, 24);
  display.print("Pair your phone with");
  display.setCursor(0, 40);
  display.print(BT_NAME);
}

void drawMusicPage(const TrackInfo &t, uint32_t now) {
  // top (yellow) strip: play/pause icon and elapsed / total time
  uint32_t pos = positionMs(t, now);
  if (t.playing) {
    display.fillTriangle(1, 2, 1, 12, 9, 7, SSD1306_WHITE);
  } else {
    display.fillRect(1, 2, 3, 10, SSD1306_WHITE);
    display.fillRect(6, 2, 3, 10, SSD1306_WHITE);
  }
  char elapsed[12], total[12], times[26];
  formatTime(elapsed, sizeof(elapsed), pos);
  formatTime(total, sizeof(total), t.lengthMs);
  snprintf(times, sizeof(times), "%s / %s", elapsed, total);
  display.setTextSize(1);
  display.setCursor(128 - strlen(times) * 6, 4);
  display.print(times);

  // song title (scrolls if long), artist, progress bar
  drawScrolling(t.title[0] ? t.title : "Connected", 19, now);
  display.setTextSize(1);
  display.setCursor(0, 40);
  display.print(t.artist);
  display.drawRect(0, 54, 128, 8, SSD1306_WHITE);
  if (t.lengthMs) display.fillRect(2, 56, (uint32_t)124 * pos / t.lengthMs, 4, SSD1306_WHITE);
}

// music and weather together: clock, city and temperature in the yellow strip,
// then the song, artist, progress bar and times
void drawComboPage(const TrackInfo &t, uint32_t now) {
  drawWeatherStrip(display);
  drawScrolling(t.title[0] ? t.title : (t.connected ? "Connected" : "No audio"), 18, now);
  display.setTextSize(1);
  display.setCursor(0, 36);
  display.print(t.connected ? t.artist : "Connect in Bluetooth");

  uint32_t pos = positionMs(t, now);
  display.drawRect(0, 46, 128, 6, SSD1306_WHITE);
  if (t.lengthMs) display.fillRect(1, 47, (uint32_t)126 * pos / t.lengthMs, 4, SSD1306_WHITE);

  if (t.playing) {
    display.fillTriangle(0, 56, 0, 62, 5, 59, SSD1306_WHITE);
  } else {
    display.fillRect(0, 56, 2, 7, SSD1306_WHITE);
    display.fillRect(4, 56, 2, 7, SSD1306_WHITE);
  }
  char elapsed[12], total[12];
  formatTime(elapsed, sizeof(elapsed), pos);
  formatTime(total, sizeof(total), t.lengthMs);
  display.setCursor(10, 56);
  display.print(elapsed);
  drawPhoneBattery(display, 44, 56);
  display.setCursor(128 - strlen(total) * 6, 56);
  display.print(total);
}

// box with a volume bar over whatever page is showing, for a moment after the phone's volume
// changes (not for the volume the phone sets right after connecting)
void drawVolumeOverlay(uint32_t now) {
  int volume = volumeLevel;
  if (volume < 0 || now - volumeChangedAt > VOLUME_OVERLAY_MS || volumeChangedAt - connectedAt < 3000) return;
  display.fillRect(8, 18, 112, 30, SSD1306_BLACK);
  display.drawRect(8, 18, 112, 30, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(14, 22);
  display.print("Volume");
  char pct[8];
  snprintf(pct, sizeof(pct), "%d%%", (volume * 100 + 63) / 127);
  display.setCursor(114 - strlen(pct) * 6, 22);
  display.print(pct);
  display.drawRect(14, 34, 100, 8, SSD1306_WHITE);
  display.fillRect(16, 36, 96 * volume / 127, 4, SSD1306_WHITE);
}

void drawScreen() {
  TrackInfo t;
  portENTER_CRITICAL(&infoLock);
  t = info;
  bool selectCombo = selectComboPending;
  selectComboPending = false;
  portEXIT_CRITICAL(&infoLock);
  if (selectCombo) page = COMBO;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  uint32_t now = millis();
  bool weather = hasWeather();
  if (weather && page == COMBO) {
    drawComboPage(t, now);
  } else if (!t.connected) {
    if (weather) drawWeatherPage(display);
    else drawPairingHint();
  } else if (weather && page == WEATHER) {
    drawWeatherPage(display);
  } else {
    drawMusicPage(t, now);   // the music page, or combo/weather before any weather has arrived
  }
  drawVolumeOverlay(now);
  display.display();
}

void togglePlayback() {
  bool playing;
  portENTER_CRITICAL(&infoLock);
  playing = info.playing;
  portEXIT_CRITICAL(&infoLock);
  if (playing) a2dp.pause(); else a2dp.play();
}

// tap = play/pause (after a short wait to rule out a double-tap),
// double-tap = switch page, hold = next track
void handleButton() {
  static bool wasDown = false, holdFired = false;
  static uint32_t downAt = 0, tapAt = 0;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();

  if (down && !wasDown && now - downAt > 50) {        // pressed (debounced)
    downAt = now;
    holdFired = false;
  } else if (down && !holdFired && now - downAt > HOLD_MS) {
    a2dp.next();
    holdFired = true;
    tapAt = 0;
  } else if (!down && wasDown && !holdFired && now - downAt > 50) {   // released quickly
    if (tapAt && now - tapAt < DOUBLE_TAP_MS) {
      page = (Page)((page + 1) % 3);
      tapAt = 0;
    } else {
      tapAt = now;
    }
  }
  if (tapAt && !down && now - tapAt >= DOUBLE_TAP_MS) {
    togglePlayback();
    tapAt = 0;
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
  startStatusLed();
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = I2S_BCK;
  cfg.pin_ws = I2S_LRCK;
  cfg.pin_data = I2S_DIN;
  i2s.begin(cfg);

  a2dp.set_avrc_metadata_attribute_mask(ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST |
                                        ESP_AVRC_MD_ATTR_PLAYING_TIME);
  a2dp.set_avrc_metadata_callback(onMetadata);
  a2dp.set_avrc_rn_playstatus_callback(onPlayStatus);
  a2dp.set_avrc_rn_play_pos_callback(onPosition, 1);
  a2dp.set_avrc_rn_track_change_callback(onTrackChange);
  a2dp.set_on_connection_state_changed(onConnection);
  a2dp.set_avrc_rn_volumechange(onVolume);
  a2dp.set_stream_reader(onAudio, true);   // count audio bytes, still play them through I2S
  a2dp.set_auto_reconnect(true);   // reconnect to the last phone when the car starts
  a2dp.set_default_bt_mode(ESP_BT_MODE_BTDM);   // keep BLE available for the phone app
  a2dp.start(BT_NAME);
  esp_avrc_ct_register_callback(avrcControllerEvent);   // replaces the library's, which we forward to
  Serial.printf("Bluetooth started as \"%s\"\n", BT_NAME);
  startPhoneLink(BT_NAME);
  audioEffects.prepare();
  publishEffectsStatus();
}

// Once a second: log the audio rate, and if music is flowing but the phone hasn't told us
// it's playing (iPhones don't after an automatic reconnect), assume it is and start the clock.
void checkAudio() {
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if (now - lastCheck < 1000) return;
  lastCheck = now;
  uint32_t bytes = audioBytes;
  audioBytes = 0;
  if (bytes) Serial.printf("Audio %lu KB/s\n", (unsigned long)(bytes / 1024));

  if (bytes > 100 * 1024) {
    portENTER_CRITICAL(&infoLock);
    bool inferred = !info.statusKnown && !info.playing;
    if (inferred) {
      info.playing = true;
      info.runningSince = now - 1000;   // that second of audio already played
    }
    portEXIT_CRITICAL(&infoLock);
    if (inferred) Serial.println("Audio is flowing, assuming playback");
  }
}

// Debug aids over serial, so layouts can be checked without the phone or looking at the board:
//   s            -> screen contents as hex (SSD1306 page layout)
//   l            -> GPIO2's configured PWM duty and frequency
//   r            -> test the two-flash LED pattern without changing weather
//   a            -> audio effects diagnostic status
//   W|... B|... R|... EQ|... BASS|... WIDE|... -> phone messages (see phone_link.h)
void handleSerial() {
  if (!Serial.available()) return;
  int c = Serial.peek();
  if (c == 'W' || c == 'B' || c == 'R' || c == 'E') {
    handlePhoneMessage(Serial.readStringUntil('\n'));
    return;
  }
  Serial.read();
  if (c == 'l') { printStatusLed(); return; }
  if (c == 'r') { updateStatusLed(a2dp.is_connected(), true); return; }
  if (c == 'a') { audioEffects.printStatus(); return; }
  if (c != 's') return;
  const uint8_t *buf = display.getBuffer();
  Serial.print("SCREEN ");
  for (int i = 0; i < 128 * 64 / 8; i++) Serial.printf("%02x", buf[i]);
  Serial.println();
}

void loop() {
  handleButton();
  handleSerial();
  bool audioConnected;
  portENTER_CRITICAL(&infoLock);
  audioConnected = info.connected;
  portEXIT_CRITICAL(&infoLock);
  updateStatusLed(audioConnected, takeRainAlert());
  checkAudio();
  pollPlayStatus();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50) {   // ~20 fps, smooth enough for scrolling titles
    lastDraw = millis();
    drawScreen();
  }
  delay(5);
}
