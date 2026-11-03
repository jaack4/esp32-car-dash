#include "effects_output.h"
#include <cassert>
#include <cmath>
#include <iostream>

static void testRoom(unsigned rate, ReverbPreset preset) {
  RoomReverb room;
  assert(room.prepare());
  room.configure(rate);
  room.setPreset(preset);
  int16_t l = 0, r = 0;
  for (int sample = -30000; sample <= 30000; sample += 137) {
    room.process(sample, -sample, false, l, r);
    assert(l == sample && r == -sample);
  }
  room.process(10000, 10000, true, l, r);
  assert(std::abs(l - 10000) < 32); // starts with a ramp, not a gain step
  for (unsigned i = 0; i < rate / 20; ++i) room.process(0, 0, true, l, r);
  room.process(24000, 0, true, l, r);
  double earlyEnergy = 0, lateEnergy = 0;
  unsigned stereoDifferences = 0;
  for (unsigned i = 0; i < rate * 4; ++i) {
    room.process(0, 0, true, l, r);
    double energy = double(l) * l + double(r) * r;
    if (i > rate / 50 && i < rate / 2) earlyEnergy += energy;
    if (i > rate * 2) lateEnergy += energy;
    if (l != r) ++stereoDifferences;
  }
  assert(earlyEnergy > 1000); // there is an actual reverberant tail
  assert(stereoDifferences > 20);
  assert(lateEnergy < earlyEnergy / 1000); // decays instead of runaway feedback
  assert(l == 0 && r == 0); // no quantization limit cycle
  room.process(12000, -8000, false, l, r);
  for (unsigned i = 0; i < rate / 30; ++i) room.process(12000, -8000, false, l, r);
  assert(room.isDry() && l == 12000 && r == -8000);
  room.process(0, 0, true, l, r);
  assert(l == 0 && r == 0); // enabling does not replay a stale room buffer
  for (unsigned i = 0; i < rate * 2; ++i) room.process(32767, 32767, true, l, r);
  assert(l > 0 && r > 0); // full-scale input does not wrap polarity
  int16_t beforeL = l, beforeR = r;
  room.process(32767, 32767, false, l, r);
  room.process(32767, 32767, true, l, r);
  assert(std::abs(l - beforeL) < 128 && std::abs(r - beforeR) < 128);
  // Reversing a fade must keep the existing tail, rather than clearing it abruptly.
  room.configure(rate == 48000 ? 44100 : 48000);
  room.process(0, 0, true, l, r);
  assert(l == 0 && r == 0); // format change clears the old delay state
}

static double tailRatio(ReverbPreset preset) {
  RoomReverb room;
  assert(room.prepare());
  room.configure(48000);
  room.setPreset(preset);
  int16_t l = 0, r = 0;
  for (unsigned i = 0; i < 2400; ++i) room.process(0, 0, true, l, r);
  room.process(24000, 12000, true, l, r);
  double early = 0, late = 0;
  for (unsigned i = 0; i < 96000; ++i) {
    room.process(0, 0, true, l, r);
    double energy = double(l) * l + double(r) * r;
    if (i < 12000) early += energy;
    if (i >= 24000 && i < 72000) late += energy;
  }
  assert(early > 0);
  return late / early;
}

static void testPresetSwitching() {
  RoomReverb steady, switched;
  assert(steady.prepare() && switched.prepare());
  int16_t a = 0, b = 0, c = 0, d = 0;
  for (unsigned i = 0; i < 44100; ++i) {
    int16_t sample = static_cast<int16_t>(18000 * std::sin(i * 0.15));
    steady.process(sample, -sample / 2, true, a, b);
    switched.process(sample, -sample / 2, true, c, d);
  }
  switched.setPreset(ReverbPreset::Club);
  steady.process(16000, -8000, true, a, b);
  switched.process(16000, -8000, true, c, d);
  assert(std::abs(a - c) < 64 && std::abs(b - d) < 64);
  double concert = tailRatio(ReverbPreset::Concert), club = tailRatio(ReverbPreset::Club);
  assert(concert > 0.0005); // retain a meaningful late field, not just an early echo
  assert(concert > club * 3); // Concert has a substantially longer decay
  std::cout << "Tail late/early energy: Concert " << concert << ", Club " << club << '\n';

  bool enabled = false;
  ReverbPreset preset = ReverbPreset::Club;
  assert(parseReverbCommand("R|1", enabled, preset) && enabled && preset == ReverbPreset::Club);
  assert(parseReverbCommand("R|0|concert", enabled, preset) && !enabled && preset == ReverbPreset::Concert);
  assert(parseReverbCommand("R|1|club", enabled, preset) && enabled && preset == ReverbPreset::Club);
  for (const char *bad : {"R||1|club", "R|2|club", "R|1|unknown", "R|1|club|extra", "R|1|club ", "R"}) {
    assert(!parseReverbCommand(bad, enabled, preset));
  }
}

static std::vector<uint8_t> inputAudio() {
  std::vector<uint8_t> data(48000 * 4 / 3);
  uint32_t seed = 7;
  for (size_t i = 0; i < data.size(); ++i) {
    seed = seed * 1664525U + 1013904223U;
    data[i] = seed >> 24;
  }
  return data;
}

int main() {
  assert(RoomReverb::capacity * sizeof(int16_t) + sizeof(RoomReverb) < 16000);
  for (auto preset : {ReverbPreset::Concert, ReverbPreset::Club}) {
    for (unsigned rate : {8000, 32000, 44100, 48000}) testRoom(rate, preset);
  }
  testPresetSwitching();
  auto data = inputAudio(), original = data;
  audio_tools::I2SStream drySink;
  EffectsOutput dry(drySink);
  assert(dry.begin());
  assert(!dry.setEnabled(true)); // missing storage must leave original audio available
  assert(dry.write(data.data(), data.size()) == data.size());
  assert(drySink.written == data);
  dry.prepare();
  drySink.written.clear();
  assert(dry.write(data.data(), data.size()) == data.size());
  assert(drySink.written == data); // default-off is bit-exact bypass

  audio_tools::I2SStream fullSink, shortSink;
  EffectsOutput full(fullSink), shortWrites(shortSink);
  full.prepare(); shortWrites.prepare();
  full.begin(); shortWrites.begin();
  full.setMode(true, ReverbPreset::Club); shortWrites.setMode(true, ReverbPreset::Club);
  assert(full.enabled() && full.preset() == ReverbPreset::Club);
  assert(full.write(data.data(), data.size()) == data.size());
  shortSink.maxWrite = 7;
  shortSink.allowance = 53;
  size_t consumed = shortWrites.write(data.data(), data.size());
  assert(consumed == 512 && shortSink.written.size() == 53);
  shortSink.allowance = std::numeric_limits<size_t>::max();
  assert(shortWrites.write(data.data() + consumed, data.size() - consumed) == data.size() - consumed);
  assert(shortSink.written == fullSink.written); // no drops or double-processing on short writes
  assert(data == original); // never modify the Bluetooth-owned PCM buffer
  assert(fullSink.written != data);

  audio_tools::I2SStream lowMemorySink;
  EffectsOutput lowMemory(lowMemorySink);
  ESP.freeHeap = 40000;
  lowMemory.prepare();
  assert(!lowMemory.available() && !lowMemory.setEnabled(true));
  lowMemory.begin();
  assert(lowMemory.write(data.data(), data.size()) == data.size());
  assert(lowMemorySink.written == data); // preserve Bluetooth heap reserve and audio
  ESP.freeHeap = 100000;

  audio_tools::AudioInfo unsupported;
  unsupported.bits_per_sample = 24;
  shortWrites.setAudioInfo(unsupported);
  shortSink.written.clear();
  shortSink.maxWrite = std::numeric_limits<size_t>::max();
  assert(shortWrites.write(data.data(), data.size()) == data.size());
  assert(shortSink.written == data); // unsupported formats pass through safely
  unsupported.bits_per_sample = 16;
  shortWrites.setAudioInfo(unsupported);
  shortSink.written.clear();
  std::vector<uint8_t> silence(48000 * 4 / 4);
  assert(shortWrites.write(silence.data(), silence.size()) == silence.size());
  assert(shortSink.written == silence); // no stale tail after returning from unsupported format
  std::cout << "PASS: both venue presets, decay contrast, preset smoothing, strict control commands, bypass, stereo decay, ramp switching, buffer reset, sample rates, full-scale input, short I2S writes, immutable source, low-memory and unsupported-format fallback\n";
}
