#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include "reverb_preset.h"

// Damped venue presets: four feedback combs and two allpasses per
// output. Fixed-size int16 delay storage keeps the working set around 15 KB.
// Only the audio writer touches this object. Control requests live in its wrapper.
class RoomReverb {
 public:
  static constexpr unsigned capacity = 7524;

  bool prepare() {
    if (!storage) storage.reset(new (std::nothrow) int16_t[capacity]);
    if (!storage) return false;
    configure(44100);
    return true;
  }

  void configure(unsigned rate) {
    sampleRate = std::max(8000U, std::min(48000U, rate));
    static const unsigned lengths[] = {1429, 1613, 1789, 1999, 223, 67, 307, 97};
    unsigned offset = 0;
    for (unsigned i = 0; i < 8; ++i) {
      lines[i].data = storage.get() + offset;
      lines[i].length = std::max(1U, lengths[i] * sampleRate / 48000U);
      offset += lengths[i];
    }
    step = 0.43f / (sampleRate * 0.025f);
    parameterSmoothing = 1.0f / (sampleRate * 0.05f);
    updateTargets();
    feedback = targetFeedback;
    damping = targetDamping;
    highpassPole = targetHighpassPole;
    clear();
    mix = 0;
  }

  void setPreset(ReverbPreset value) {
    if (value == preset) return;
    preset = value;
    updateTargets();
    if (mix == 0) {
      feedback = targetFeedback;
      damping = targetDamping;
      highpassPole = targetHighpassPole;
    }
  }

  void process(int16_t left, int16_t right, bool enabled, int16_t &outL, int16_t &outR) {
    if (enabled && !wasEnabled && mix == 0) clear();
    wasEnabled = enabled;
    float targetMix = enabled ? targetWet : 0.0f;
    mix += std::max(-step, std::min(step, targetMix - mix));
    if (mix == 0) {
      outL = left;
      outR = right;
      return;
    }
    feedback += (targetFeedback - feedback) * parameterSmoothing;
    damping += (targetDamping - damping) * parameterSmoothing;
    highpassPole += (targetHighpassPole - highpassPole) * parameterSmoothing;
    const float l = left / 32768.0f, r = right / 32768.0f;
    // Keep bass in the original signal, without feeding low-frequency buildup
    // into the room. This filter affects only the reverberant send.
    bassL += (1.0f - highpassPole) * (l - bassL);
    bassR += (1.0f - highpassPole) * (r - bassR);
    float sendL = std::max(-1.0f, std::min(1.0f, l - bassL));
    float sendR = std::max(-1.0f, std::min(1.0f, r - bassR));
    // Scale the send with feedback, so longer tails don't overload delay storage.
    float sendGain = 0.78f * (1.0f - feedback);
    const float a = comb(0, sendL * sendGain);
    const float b = comb(1, sendR * sendGain);
    const float c = comb(2, (sendL + sendR) * (sendGain * 0.5f));
    const float d = comb(3, (sendL - sendR) * (sendGain * 0.5f));
    const float wetL = allpass(5, allpass(4, (a + c + d) / 3.0f));
    const float wetR = allpass(7, allpass(6, (b + c - d) / 3.0f));
    outL = quantize((1.0f - mix) * l + mix * wetL);
    outR = quantize((1.0f - mix) * r + mix * wetR);
  }

  bool isDry() const { return mix == 0; }

 private:
  struct Line {
    int16_t *data = nullptr;
    unsigned length = 1, index = 0;
    float damped = 0;
  } lines[8];
  std::unique_ptr<int16_t[]> storage;
  unsigned sampleRate = 44100;
  float mix = 0, step = 0;
  bool wasEnabled = false;
  ReverbPreset preset = ReverbPreset::Concert;
  float feedback = 0, damping = 0, highpassPole = 0, parameterSmoothing = 0;
  float targetFeedback = 0, targetDamping = 0, targetHighpassPole = 0, targetWet = 0;
  float bassL = 0, bassR = 0;

  void updateTargets() {
    const bool concert = preset == ReverbPreset::Concert;
    targetFeedback = concert ? 0.90f : 0.78f;
    targetDamping = concert ? 0.35f : 0.62f;
    targetWet = concert ? 0.43f : 0.36f;
    const float cutoff = concert ? 120.0f : 180.0f;
    targetHighpassPole = std::exp(-6.2831853f * cutoff / sampleRate);
  }

  void clear() {
    std::memset(storage.get(), 0, capacity * sizeof(int16_t));
    for (auto &line : lines) { line.index = 0; line.damped = 0; }
    bassL = bassR = 0;
  }

  static int16_t quantize(float value) {
    value = std::max(-1.0f, std::min(32767.0f / 32768.0f, value));
    // Truncating delay writes lets a sub-LSB feedback tail reach true silence.
    return static_cast<int16_t>(value * 32768.0f);
  }

  float comb(unsigned id, float input) {
    Line &line = lines[id];
    float delayed = line.data[line.index] / 32768.0f;
    line.damped = delayed * (1.0f - damping) + line.damped * damping;
    line.data[line.index] = quantize(input + line.damped * feedback);
    if (++line.index == line.length) line.index = 0;
    return delayed;
  }

  float allpass(unsigned id, float input) {
    Line &line = lines[id];
    float delayed = line.data[line.index] / 32768.0f;
    float output = delayed - input * 0.5f;
    line.data[line.index] = quantize(input + output * 0.5f);
    if (++line.index == line.length) line.index = 0;
    return output;
  }
};
