#include "effects_output.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

static void check(bool ok, const char *what, double value = NAN) {
  if (ok) return;
  std::fprintf(stderr, "FAIL: %s (%g)\n", what, value);
  std::abort();
}

static ToneSettings settings(void (*change)(ToneSettings &)) {
  ToneSettings s;
  change(s);
  return s;
}

// Gain in dB for a sine at `freq` after the settings have fully ramped in.
// side = true feeds the sine as pure stereo difference (L = -R) instead of mono.
static double gainDb(const ToneSettings &s, float freq, unsigned rate, bool side = false, float amplitude = 0.05f) {
  ToneShaper shaper;
  shaper.configure(rate);
  shaper.setTarget(s);
  double in = 0, out = 0;
  for (unsigned i = 0; i < rate * 2; ++i) {
    if (i % ToneShaper::blockFrames == 0) shaper.beginBlock();
    float x = amplitude * std::sin(6.2831853f * freq * i / rate);
    float l = x, r = side ? -x : x;
    shaper.process(l, r);
    if (i > rate) {   // measure after the ramp and filter settling
      in += double(x) * x;
      out += double(l) * l;
    }
  }
  return 10 * std::log10(out / in);
}

static void testSettings() {
  ToneSettings s;
  s.eqOn = true; s.bass = -12; s.mid = 12; s.treble = -5;
  s.bassBoostOn = true; s.bassBoostLevel = 3; s.widenerOn = true; s.widthLevel = 1;
  check(ToneSettings::unpack(s.pack()) == s, "pack round trip");
  ToneSettings d = ToneSettings::unpack(ToneSettings().pack());
  check(!d.eqOn && !d.bassBoostOn && !d.widenerOn && d.bassBoostLevel == 2 && d.widthLevel == 2, "default settings");

  ToneSettings t;
  check(parseToneCommand("EQ|1|6|-3|12", t) && t.eqOn && t.bass == 6 && t.mid == -3 && t.treble == 12, "EQ command");
  check(!t.bassBoostOn && !t.widenerOn, "EQ leaves other effects alone");
  check(parseToneCommand("BASS|1|3", t) && t.bassBoostOn && t.bassBoostLevel == 3 && t.bass == 6, "BASS command");
  check(parseToneCommand("WIDE|1|1", t) && t.widenerOn && t.widthLevel == 1, "WIDE command");
  check(parseToneCommand("EQ|0|6|-3|12", t) && !t.eqOn && t.bass == 6, "EQ off keeps slider values");
  ToneSettings before = t;
  for (const char *bad : {"EQ|1|13|0|0", "EQ|1|a|0|0", "EQ|1|1|2", "EQ|2|0|0|0", "BASS|1|0", "BASS|1|4",
                          "WIDE|1|", "WIDE|x|2", "LOUD|1|2", "BASS|1|2|3", "", "EQ|1|0|0|0|0",
                          "EQ|1|000000000000000000000000000000000000000000000000001|0|0"}) {
    check(!parseToneCommand(bad, t), bad);
    check(t == before, "rejected command leaves settings unchanged");
  }
}

static void testBypass() {
  ToneShaper shaper;
  shaper.configure(44100);
  shaper.setTarget(ToneSettings());
  check(shaper.idle(), "default is idle");
  for (int i = -30000; i < 30000; i += 101) {
    float l = i / 32768.0f, r = -i / 32768.0f, l0 = l, r0 = r;
    shaper.beginBlock();
    shaper.process(l, r);
    check(l == l0 && r == r0, "idle passes samples through unchanged");
  }
}

static void testFrequencyResponse(unsigned rate) {
  // Bass boost (medium, +7 dB at 65 Hz) with half of it taken back as headroom.
  auto boost = settings([](ToneSettings &s) { s.bassBoostOn = true; s.bassBoostLevel = 2; });
  double low = gainDb(boost, 65, rate), high = gainDb(boost, 3000, rate);
  check(std::fabs(low - high - 7) < 0.7, "bass boost lifts 65 Hz by 7 dB", low - high);
  check(std::fabs(high + 3.5) < 0.5, "bass boost headroom is half the boost", high);

  auto treble = settings([](ToneSettings &s) { s.eqOn = true; s.treble = -6; });
  double t12k = gainDb(treble, 12000, rate), t200 = gainDb(treble, 200, rate);
  check(t12k < -4.5 && t12k > -6.5, "treble cut at 12 kHz", t12k);
  check(std::fabs(t200) < 0.3, "treble cut leaves 200 Hz alone", t200);

  auto bass = settings([](ToneSettings &s) { s.eqOn = true; s.bass = 6; });
  double b40 = gainDb(bass, 40, rate), b2k = gainDb(bass, 2000, rate);
  check(std::fabs(b40 - b2k - 6) < 1.0, "EQ bass +6 dB", b40 - b2k);

  auto mid = settings([](ToneSettings &s) { s.eqOn = true; s.mid = 6; });
  double m1k = gainDb(mid, 1000, rate), m60 = gainDb(mid, 60, rate);
  check(m1k - m60 > 5 && m1k - m60 < 6.5, "EQ mid +6 dB at 1 kHz", m1k - m60);

  // Widener: mono stays as it is, stereo difference above ~150 Hz gets wider, bass doesn't.
  auto wide = settings([](ToneSettings &s) { s.widenerOn = true; s.widthLevel = 2; });
  check(std::fabs(gainDb(wide, 1000, rate)) < 0.05, "widener leaves mono unchanged");
  double side2k = gainDb(wide, 2000, rate, true), side40 = gainDb(wide, 40, rate, true);
  check(std::fabs(side2k - 20 * std::log10(1.7)) < 0.5, "widener widens 2 kHz stereo", side2k);
  check(side40 < 1.5, "widener keeps bass centred", side40);
}

static void testLimiterAndRamps() {
  const unsigned rate = 44100;
  ToneShaper shaper;
  shaper.configure(rate);
  // Everything boosting the bass, with a full-scale 50 Hz input: output must stay under the ceiling.
  shaper.setTarget(settings([](ToneSettings &s) {
    s.eqOn = true; s.bass = 12; s.bassBoostOn = true; s.bassBoostLevel = 3; s.widenerOn = true; s.widthLevel = 3;
  }));
  float peak = 0;
  for (unsigned i = 0; i < rate * 2; ++i) {
    if (i % ToneShaper::blockFrames == 0) shaper.beginBlock();
    float x = std::sin(6.2831853f * 50 * i / rate), l = x, r = x * 0.3f;
    shaper.process(l, r);
    peak = std::max({peak, std::fabs(l), std::fabs(r)});
  }
  check(peak <= 0.9441f, "limiter keeps peaks under -0.5 dBFS", peak);
  check(shaper.limiterReduction() < 1, "limiter engaged on boosted full-scale bass");

  // Switching off ramps back to neutral, the limiter lets go, and audio is untouched again.
  shaper.setTarget(ToneSettings());
  for (unsigned i = 0; i < rate; ++i) {
    if (i % ToneShaper::blockFrames == 0) shaper.beginBlock();
    float l = 0.1f, r = -0.1f;
    shaper.process(l, r);
  }
  check(shaper.idle(), "returns to idle after switching off");

  // Switching on mid-song must not click: no jump bigger than the music's own movement.
  shaper.setTarget(settings([](ToneSettings &s) { s.bassBoostOn = true; s.bassBoostLevel = 3; }));
  float previous = 0, largestStep = 0;
  for (unsigned i = 0; i < rate / 2; ++i) {
    if (i % ToneShaper::blockFrames == 0) shaper.beginBlock();
    float x = 0.3f * std::sin(6.2831853f * 1000 * i / rate), l = x, r = x;
    shaper.process(l, r);
    if (i) largestStep = std::max(largestStep, std::fabs(l - previous));
    previous = l;
  }
  check(largestStep < 0.06f, "switching on ramps smoothly", largestStep);
}

static std::vector<uint8_t> music(unsigned frames) {
  std::vector<uint8_t> data(frames * 4);
  for (unsigned i = 0; i < frames; ++i) {
    int16_t l = int16_t(12000 * std::sin(i * 0.01) + 6000 * std::sin(i * 0.3));
    int16_t r = int16_t(10000 * std::sin(i * 0.013));
    std::memcpy(&data[i * 4], &l, 2);
    std::memcpy(&data[i * 4 + 2], &r, 2);
  }
  return data;
}

static void testOutputStage() {
  auto data = music(44100), original = data;

  // Tone effects on, reverb off: audio changes, the source buffer doesn't.
  audio_tools::I2SStream sink;
  EffectsOutput fx(sink);
  fx.prepare();
  fx.begin();
  fx.setTone(settings([](ToneSettings &s) { s.bassBoostOn = true; }));
  check(fx.write(data.data(), data.size()) == data.size(), "write consumes everything");
  check(sink.written != data && data == original, "bass boost changes output, not the source");

  // Switched back off, it returns to bit-exact passthrough once the ramp finishes.
  fx.setTone(ToneSettings());
  fx.write(data.data(), data.size());
  sink.written.clear();
  fx.write(data.data(), data.size());
  check(sink.written == data, "off again is bit-exact passthrough");

  // Tone effects need no buffer, so they still work when reverb couldn't get its memory.
  audio_tools::I2SStream lowSink;
  EffectsOutput low(lowSink);
  ESP.freeHeap = 40000;
  low.prepare();
  ESP.freeHeap = 100000;
  low.begin();
  check(!low.available(), "reverb unavailable on low memory");
  low.setTone(settings([](ToneSettings &s) { s.eqOn = true; s.treble = -8; }));
  low.write(data.data(), data.size());
  check(lowSink.written != data, "EQ works without reverb memory");

  // Reverb and tone together still consume every byte across short I2S writes.
  audio_tools::I2SStream shortSink;
  EffectsOutput both(shortSink);
  both.prepare();
  both.begin();
  both.setMode(true, ReverbPreset::Concert);
  both.setTone(settings([](ToneSettings &s) { s.widenerOn = true; s.eqOn = true; s.bass = 4; }));
  shortSink.maxWrite = 9;
  size_t total = 0;
  while (total < data.size()) total += both.write(data.data() + total, data.size() - total);
  check(total == data.size() && shortSink.written.size() == data.size(), "no drops with short writes");
}

// The promise in the app: with everything switched off, audio is exactly what it would be
// without any effects. Checked from power-on and after every effect has been on and off again,
// at each sample rate a phone might use, including extreme sample values.
static void testAllOffIsPure() {
  for (unsigned rate : {44100, 48000}) {
    auto data = music(rate);
    for (int16_t extreme : {int16_t(32767), int16_t(-32768), int16_t(1), int16_t(-1)}) {
      std::memcpy(&data[400], &extreme, 2);
      std::memcpy(&data[402], &extreme, 2);
      data[404 + (extreme & 3) * 4] ^= 0x55;   // and some arbitrary bit patterns
    }
    audio_tools::AudioInfo info;
    info.sample_rate = rate;

    audio_tools::I2SStream sink;
    EffectsOutput fx(sink);
    fx.prepare();
    fx.begin();
    fx.setAudioInfo(info);
    fx.write(data.data(), data.size());
    check(sink.written == data, "power-on default is untouched audio");

    ToneSettings everything;
    everything.eqOn = true; everything.bass = 12; everything.mid = -12; everything.treble = 7;
    everything.bassBoostOn = true; everything.bassBoostLevel = 3;
    everything.widenerOn = true; everything.widthLevel = 3;
    fx.setTone(everything);
    check(fx.setMode(true, ReverbPreset::Concert), "reverb on");
    sink.written.clear();
    fx.write(data.data(), data.size());
    check(sink.written != data, "all four effects change the audio");

    fx.setTone(ToneSettings());
    fx.setMode(false, ReverbPreset::Concert);
    fx.write(data.data(), data.size());   // one second for the fades, tail and limiter to finish
    sink.written.clear();
    fx.write(data.data(), data.size());
    check(sink.written == data, "all off again is untouched audio");
  }
}

int main() {
  testSettings();
  testBypass();
  for (unsigned rate : {44100, 48000}) testFrequencyResponse(rate);
  testLimiterAndRamps();
  testOutputStage();
  testAllOffIsPure();
  std::cout << "PASS: tone settings and commands, idle passthrough, bass boost, EQ bands, widener, "
               "limiter ceiling, smooth switching, return to bypass, output stage with and without reverb, "
               "all off is byte-identical audio (from power-on and after everything was on)\n";
}
