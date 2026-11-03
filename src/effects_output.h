#pragma once

#include "AudioTools.h"
#include "room_reverb.h"
#include "tone_shaper.h"
#include <atomic>

// Sits between the Bluetooth audio and the DAC and applies the effects the phone app turns on:
// bass boost, EQ and stereo widener (ToneShaper), then reverb (RoomReverb). Everything starts
// off, and with everything off audio passes through untouched.
class EffectsOutput : public audio_tools::AudioOutput {
 public:
  explicit EffectsOutput(audio_tools::I2SStream &destination) : destination(destination) {}
  void prepare();
  bool available() const { return prepared.load(); }
  bool enabled() const { return (mode.load() & 1) != 0; }
  ReverbPreset preset() const { return (mode.load() & 2) ? ReverbPreset::Club : ReverbPreset::Concert; }
  bool setEnabled(bool enabled);
  bool setMode(bool enabled, ReverbPreset preset);
  ToneSettings tone() const { return ToneSettings::unpack(toneBits.load()); }
  void setTone(const ToneSettings &settings) { toneBits.store(settings.pack()); }
  bool begin() override;
  void end() override;
  void setAudioInfo(audio_tools::AudioInfo info) override;
  int availableForWrite() override { return destination.availableForWrite(); }
  size_t write(const uint8_t *data, size_t length) override;
  void printStatus() const;

 private:
  audio_tools::I2SStream &destination;
  RoomReverb room;
  ToneShaper toneShaper;
  std::atomic<uint32_t> toneBits{ToneSettings().pack()};
  unsigned toneFrame = 0;   // frames since the last ToneShaper::beginBlock
  std::atomic<bool> prepared{false};
  std::atomic<unsigned> mode{0}; // bit 0: enabled, bit 1: club (otherwise concert)
  std::atomic<unsigned> rate{44100}, maxProcessMicros{0};
  std::atomic<bool> supportedFormat{true};
  std::atomic<bool> resetRequested{false};
  unsigned activeRate = 0;
  uint8_t scratch[512];
  size_t pendingOffset = 0, pendingLength = 0;
  bool flushPending();
};

EffectsOutput &audioEffects();
