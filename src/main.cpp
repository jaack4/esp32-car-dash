// Bluetooth car audio receiver: the phone streams music over Bluetooth (A2DP),
// the ESP32 sends it to a PCM5102A DAC for the car's AUX input, and the OLED
// shows the song, artist and progress. BOOT button: tap = play/pause, hold = next track.
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "AudioTools.h"
#include "BluetoothA2DPSink.h"

const char *BT_NAME = "ESP32 Car Audio";

// PCM5102A wiring (VIN -> 3V3, GND -> GND, SCK -> GND)
const int I2S_BCK = 26, I2S_LRCK = 25, I2S_DIN = 27;
const int OLED_SDA = 21, OLED_SCL = 22;
const int BUTTON_PIN = 0;          // BOOT button, low while pressed
const uint32_t HOLD_MS = 600;      // press longer than this = next track
const uint32_t POLL_MS = 1000;     // how often to ask the phone for the playback position
const uint8_t TL_PLAY_STATUS = 5;  // AVRCP transaction label (the library uses 0-4)

Adafruit_SSD1306 display(128, 64, &Wire, -1, 100000, 100000);
I2SStream i2s;
BluetoothA2DPSink a2dp(i2s);

// Track info arrives on the Bluetooth task; the main loop copies it out under this lock.
portMUX_TYPE infoLock = portMUX_INITIALIZER_UNLOCKED;
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
  portEXIT_CRITICAL(&infoLock);
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

void drawScreen() {
  TrackInfo t;
  portENTER_CRITICAL(&infoLock);
  t = info;
  portEXIT_CRITICAL(&infoLock);
  uint32_t now = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);

  if (!t.connected) {
    display.setTextSize(1);
    display.setCursor(0, 4);
    display.print("Bluetooth ready");
    display.setCursor(0, 24);
    display.print("Pair your phone with");
    display.setCursor(0, 40);
    display.print(BT_NAME);
    display.display();
    return;
  }

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

  display.display();
}

void handleButton() {
  static bool wasDown = false, holdFired = false;
  static uint32_t downAt = 0;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();

  if (down && !wasDown && now - downAt > 50) {        // pressed (debounced)
    downAt = now;
    holdFired = false;
  } else if (down && !holdFired && now - downAt > HOLD_MS) {
    a2dp.next();                                      // held: next track
    holdFired = true;
  } else if (!down && wasDown && !holdFired && now - downAt > 50) {
    bool playing;                                     // released quickly: play/pause
    portENTER_CRITICAL(&infoLock);
    playing = info.playing;
    portEXIT_CRITICAL(&infoLock);
    if (playing) a2dp.pause(); else a2dp.play();
  }
  wasDown = down;
}

void setup() {
  Serial.begin(115200);
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
  a2dp.set_stream_reader(onAudio, true);   // count audio bytes, still play them through I2S
  a2dp.set_auto_reconnect(true);   // reconnect to the last phone when the car starts
  a2dp.start(BT_NAME);
  esp_avrc_ct_register_callback(avrcControllerEvent);   // replaces the library's, which we forward to
  Serial.printf("Bluetooth started as \"%s\"\n", BT_NAME);
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

void loop() {
  handleButton();
  checkAudio();
  pollPlayStatus();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50) {   // ~20 fps, smooth enough for scrolling titles
    lastDraw = millis();
    drawScreen();
  }
  delay(5);
}
