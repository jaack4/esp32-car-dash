#include "effects_output.h"

void EffectsOutput::prepare() {
  // Leave space for Bluetooth connection/stream allocations after startup.
  const unsigned bufferBytes = RoomReverb::capacity * sizeof(int16_t);
  prepared.store(ESP.getFreeHeap() >= bufferBytes + 32U * 1024U && room.prepare());
  Serial.printf("Reverb %s; buffer %u bytes; free heap %lu bytes\n",
                available() ? "ready (off)" : "unavailable (audio bypassed)",
                available() ? RoomReverb::capacity * sizeof(int16_t) : 0U,
                (unsigned long)ESP.getFreeHeap());
}

bool EffectsOutput::setEnabled(bool value) {
  if (value && !available()) return false;
  unsigned current = mode.load();
  while (!mode.compare_exchange_weak(current, value ? current | 1U : current & ~1U)) {}
  return true;
}

bool EffectsOutput::setMode(bool value, ReverbPreset preset) {
  if (value && !available()) return false;
  mode.store((value ? 1U : 0U) | (preset == ReverbPreset::Club ? 2U : 0U));
  return true;
}

// Rounded and clamped float -> int16 for the tone stage's output.
static int16_t toSample(float value) {
  return static_cast<int16_t>(std::lrint(std::max(-32768.0f, std::min(32767.0f, value * 32768.0f))));
}

bool EffectsOutput::begin() {
  pendingOffset = pendingLength = 0;
  activeRate = 0;
  auto info = destination.audioInfo();
  audio_tools::AudioOutput::setAudioInfo(info);
  rate.store(info.sample_rate);
  supportedFormat.store(info.bits_per_sample == 16 && info.channels == 2 &&
                        info.sample_rate >= 8000 && info.sample_rate <= 48000);
  return destination.begin();
}

void EffectsOutput::end() {
  destination.end();
  pendingOffset = pendingLength = 0;
  activeRate = 0;
}

void EffectsOutput::setAudioInfo(audio_tools::AudioInfo info) {
  audio_tools::AudioOutput::setAudioInfo(info);
  rate.store(info.sample_rate);
  supportedFormat.store(info.bits_per_sample == 16 && info.channels == 2 &&
                        info.sample_rate >= 8000 && info.sample_rate <= 48000);
  resetRequested.store(true);
  destination.setAudioInfo(info);
}

bool EffectsOutput::flushPending() {
  while (pendingOffset < pendingLength) {
    size_t count = destination.write(scratch + pendingOffset, pendingLength - pendingOffset);
    if (!count) return false;
    pendingOffset += count;
  }
  pendingOffset = pendingLength = 0;
  return true;
}

size_t EffectsOutput::write(const uint8_t *data, size_t length) {
  if (!flushPending()) return 0;
  if (!supportedFormat.load() || length % 4 != 0) return destination.write(data, length);
  unsigned sampleRate = rate.load();
  if (resetRequested.exchange(false) || sampleRate != activeRate) {
    if (available()) room.configure(sampleRate);
    toneShaper.configure(sampleRate);
    toneFrame = 0;
    activeRate = sampleRate;
  }
  // The tone effects need no extra memory, so they work even if reverb couldn't get its buffer.
  toneShaper.setTarget(tone());
  if (available()) room.setPreset(preset());
  const bool reverbIdle = !available() || (!enabled() && room.isDry());
  // Everything off (and done fading out): hand Bluetooth's samples to the DAC untouched, exactly
  // as if there were no effects at all.
  if (reverbIdle && toneShaper.idle()) return destination.write(data, length);

  size_t consumed = 0;
  while (consumed < length) {
    size_t count = std::min(sizeof(scratch), length - consumed);
    unsigned started = micros();
    unsigned currentMode = mode.load();
    bool desired = (currentMode & 1) != 0;
    if (available()) room.setPreset((currentMode & 2) ? ReverbPreset::Club : ReverbPreset::Concert);
    for (size_t i = 0; i < count; i += 4) {
      int16_t inL, inR, outL, outR;
      std::memcpy(&inL, data + consumed + i, 2);
      std::memcpy(&inR, data + consumed + i + 2, 2);
      if (toneFrame++ % ToneShaper::blockFrames == 0) toneShaper.beginBlock();
      float l = inL / 32768.0f, r = inR / 32768.0f;   // exact, so no tone effects = unchanged samples
      toneShaper.process(l, r);
      const int16_t toneL = toSample(l), toneR = toSample(r);
      if (available()) {
        room.process(toneL, toneR, desired, outL, outR);
      } else {
        outL = toneL;
        outR = toneR;
      }
      std::memcpy(scratch + i, &outL, 2);
      std::memcpy(scratch + i + 2, &outR, 2);
    }
    unsigned elapsed = micros() - started;
    if (elapsed > maxProcessMicros.load()) maxProcessMicros.store(elapsed);
    pendingLength = count;
    consumed += count; // retained in scratch even if I2S temporarily writes less
    if (!flushPending()) break;
  }
  return consumed;
}

void EffectsOutput::printStatus() const {
  Serial.printf("Reverb %s (%s); ready %d; rate %u Hz; max DSP block %u us; free heap %lu; minimum heap %lu\n",
                enabled() ? "ON" : "OFF", reverbPresetName(preset()), available(), rate.load(), maxProcessMicros.load(),
                (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  ToneSettings t = tone();
  Serial.printf("EQ %s (bass %d, mid %d, treble %d dB); bass boost %s (level %d); widener %s (level %d)\n",
                t.eqOn ? "ON" : "OFF", t.bass, t.mid, t.treble, t.bassBoostOn ? "ON" : "OFF", t.bassBoostLevel,
                t.widenerOn ? "ON" : "OFF", t.widthLevel);
}
