#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Bass boost, 3-band EQ and stereo widener, followed by a peak limiter.
//
// Each effect fades in and out by ramping its setting from/to neutral (0 dB, width 1), so
// switching never clicks. Once every setting is neutral and the limiter has let go, the stage
// reports itself bypassed and the caller passes audio through untouched.
//
// Boosts raise peaks, which would clip the DAC's line output, so the stage lowers its input by
// half of the largest boost and a fast limiter catches whatever is left.

struct ToneSettings {
  static constexpr int maxGainDb = 12;
  bool eqOn = false;
  int bass = 0, mid = 0, treble = 0;   // dB, -12..12
  bool bassBoostOn = false;
  int bassBoostLevel = 2;              // 1 light, 2 medium, 3 heavy
  bool widenerOn = false;
  int widthLevel = 2;                  // 1 subtle, 2 wide, 3 extra

  // Packs into 22 bits so the BLE task can hand settings to the audio task atomically.
  uint32_t pack() const {
    auto gain = [](int db) { return uint32_t(std::max(-maxGainDb, std::min(maxGainDb, db)) + maxGainDb); };
    auto level = [](int value) { return uint32_t(std::max(1, std::min(3, value))); };
    return uint32_t(eqOn) | uint32_t(bassBoostOn) << 1 | uint32_t(widenerOn) << 2 |
           level(bassBoostLevel) << 3 | level(widthLevel) << 5 |
           gain(bass) << 7 | gain(mid) << 12 | gain(treble) << 17;
  }

  static ToneSettings unpack(uint32_t bits) {
    ToneSettings s;
    s.eqOn = bits & 1;
    s.bassBoostOn = bits >> 1 & 1;
    s.widenerOn = bits >> 2 & 1;
    s.bassBoostLevel = bits >> 3 & 3;
    s.widthLevel = bits >> 5 & 3;
    s.bass = int(bits >> 7 & 31) - maxGainDb;
    s.mid = int(bits >> 12 & 31) - maxGainDb;
    s.treble = int(bits >> 17 & 31) - maxGainDb;
    return s;
  }

  bool operator==(const ToneSettings &o) const { return pack() == o.pack(); }
};

// Phone commands (see phone_link.h). Out-of-range or malformed commands are rejected, and on
// success only the fields of that effect change.
//   EQ|<on 0/1>|<bass dB>|<mid dB>|<treble dB>
//   BASS|<on 0/1>|<level 1-3>
//   WIDE|<on 0/1>|<level 1-3>
inline bool parseToneCommand(const char *message, ToneSettings &settings) {
  char buf[48];
  if (std::strlen(message) >= sizeof(buf)) return false;
  std::strcpy(buf, message);
  char *fields[6];
  int count = 0;
  char *save = nullptr;
  for (char *p = strtok_r(buf, "|", &save); p && count < 6; p = strtok_r(nullptr, "|", &save)) fields[count++] = p;
  auto toInt = [](const char *text, int low, int high, int &out) {
    char *end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != 0 || value < low || value > high) return false;
    out = int(value);
    return true;
  };
  int on = 0;
  if (count < 2 || !toInt(fields[1], 0, 1, on)) return false;
  ToneSettings next = settings;
  if (count == 5 && std::strcmp(fields[0], "EQ") == 0) {
    const int g = ToneSettings::maxGainDb;
    if (!toInt(fields[2], -g, g, next.bass) || !toInt(fields[3], -g, g, next.mid) ||
        !toInt(fields[4], -g, g, next.treble)) return false;
    next.eqOn = on;
  } else if (count == 3 && std::strcmp(fields[0], "BASS") == 0) {
    if (!toInt(fields[2], 1, 3, next.bassBoostLevel)) return false;
    next.bassBoostOn = on;
  } else if (count == 3 && std::strcmp(fields[0], "WIDE") == 0) {
    if (!toInt(fields[2], 1, 3, next.widthLevel)) return false;
    next.widenerOn = on;
  } else {
    return false;
  }
  settings = next;
  return true;
}

class ToneShaper {
 public:
  static constexpr unsigned blockFrames = 32;   // settings ramp and filters update this often

  void configure(unsigned rate) {
    sampleRate = float(std::max(8000U, std::min(48000U, rate)));
    const float blockSeconds = blockFrames / sampleRate;
    dbStep = 48.0f * blockSeconds;     // full ±12 dB swing in 0.25 s
    widthStep = 4.0f * blockSeconds;
    release = 1.0f - std::exp(-1.0f / (0.08f * sampleRate));
    sideHighpassPole = std::exp(-6.2831853f * 150.0f / sampleRate);
    current = Params();
    limiterGain = 1.0f;
    reset();
    updateFilters();
  }

  void setTarget(const ToneSettings &s) {
    static const float boostDb[] = {0, 4, 7, 10};
    static const float widths[] = {1, 1.35f, 1.7f, 2.1f};
    target.bass = s.eqOn ? float(s.bass) : 0;
    target.mid = s.eqOn ? float(s.mid) : 0;
    target.treble = s.eqOn ? float(s.treble) : 0;
    target.boost = s.bassBoostOn ? boostDb[std::max(1, std::min(3, s.bassBoostLevel))] : 0;
    target.width = s.widenerOn ? widths[std::max(1, std::min(3, s.widthLevel))] : 1;
  }

  // Nothing to do: every setting is neutral, nothing is ramping, and the limiter is idle.
  bool idle() const { return current == Params() && target == Params() && limiterGain == 1.0f; }

  // Call before every blockFrames frames: moves settings toward their targets and updates
  // the filters when anything changed.
  void beginBlock() {
    if (current == target) return;
    if (current == Params()) reset();   // leaving bypass: don't replay stale filter state
    auto approach = [](float &value, float goal, float step) {
      value += std::max(-step, std::min(step, goal - value));
    };
    approach(current.bass, target.bass, dbStep);
    approach(current.mid, target.mid, dbStep);
    approach(current.treble, target.treble, dbStep);
    approach(current.boost, target.boost, dbStep);
    approach(current.width, target.width, widthStep);
    updateFilters();
  }

  // One stereo frame, samples in -1..1.
  void process(float &l, float &r) {
    if (current == Params() && limiterGain == 1.0f) return;
    l *= preGain;
    r *= preGain;
    for (Biquad &f : filters) {
      if (!f.active) continue;
      l = f.run(l, f.stateL);
      r = f.run(r, f.stateR);
    }
    if (current.width != 1.0f) {
      // Widen only above ~150 Hz: spreading bass makes it sound thin and hollow.
      const float mid = 0.5f * (l + r), side = 0.5f * (l - r);
      sideLow += (1.0f - sideHighpassPole) * (side - sideLow);
      const float wide = side + (current.width - 1.0f) * (side - sideLow);
      l = mid + wide;
      r = mid - wide;
    }
    // Peak limiter: instant attack, 80 ms release, ceiling just under full scale. Recover first,
    // then check, so the recovering gain can't push this sample over the ceiling.
    float gain = limiterGain < 1.0f ? std::min(1.0f, limiterGain + (1.0f - limiterGain) * release + 1e-6f) : 1.0f;
    const float peak = std::max(std::fabs(l), std::fabs(r)) * gain;
    if (peak > ceiling) gain *= ceiling / peak;
    limiterGain = gain;
    l *= gain;
    r *= gain;
  }

  float limiterReduction() const { return limiterGain; }

 private:
  struct Params {
    float bass = 0, mid = 0, treble = 0, boost = 0, width = 1;
    bool operator==(const Params &o) const {
      return bass == o.bass && mid == o.mid && treble == o.treble && boost == o.boost && width == o.width;
    }
  };

  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float stateL[2] = {0, 0}, stateR[2] = {0, 0};
    bool active = false;
    float run(float x, float *z) {   // transposed direct form II
      const float y = b0 * x + z[0];
      z[0] = b1 * x - a1 * y + z[1];
      z[1] = b2 * x - a2 * y;
      return y;
    }
  };

  enum { BOOST, BASS, MID, TREBLE };
  Biquad filters[4];
  Params current, target;
  float sampleRate = 44100, dbStep = 0, widthStep = 0, release = 0, preGain = 1;
  float limiterGain = 1, sideLow = 0, sideHighpassPole = 0;
  static constexpr float ceiling = 0.944f;   // -0.5 dBFS

  void reset() {
    for (Biquad &f : filters) {
      f.stateL[0] = f.stateL[1] = f.stateR[0] = f.stateR[1] = 0;
    }
    sideLow = 0;
  }

  void updateFilters() {
    // Bass boost and the EQ's bass both lift the low end, so their boosts add up.
    const float lows = std::max(0.0f, current.bass) + current.boost;
    const float largest = std::max({lows, current.mid, current.treble, 0.0f});
    preGain = std::pow(10.0f, -0.5f * largest / 20.0f);
    setPeaking(filters[BOOST], 65, 0.8f, current.boost);
    setShelf(filters[BASS], 120, current.bass, false);
    setPeaking(filters[MID], 1000, 0.7f, current.mid);
    setShelf(filters[TREBLE], 6000, current.treble, true);
  }

  // RBJ audio EQ cookbook filters.
  void setPeaking(Biquad &f, float freq, float q, float db) {
    f.active = db != 0;
    if (!f.active) return;
    const float a = std::pow(10.0f, db / 40.0f), w = 6.2831853f * freq / sampleRate;
    const float alpha = std::sin(w) / (2 * q), cw = std::cos(w), a0 = 1 + alpha / a;
    f.b0 = (1 + alpha * a) / a0;
    f.b1 = -2 * cw / a0;
    f.b2 = (1 - alpha * a) / a0;
    f.a1 = -2 * cw / a0;
    f.a2 = (1 - alpha / a) / a0;
  }

  void setShelf(Biquad &f, float freq, float db, bool high) {
    f.active = db != 0;
    if (!f.active) return;
    const float a = std::pow(10.0f, db / 40.0f), w = 6.2831853f * freq / sampleRate;
    const float cw = std::cos(w), alpha = std::sin(w) / 2 * std::sqrt(2.0f);   // shelf slope 1
    const float s = 2 * std::sqrt(a) * alpha, sign = high ? 1.0f : -1.0f;
    const float a0 = (a + 1) - sign * (a - 1) * cw + s;
    f.b0 = a * ((a + 1) + sign * (a - 1) * cw + s) / a0;
    f.b1 = -2 * sign * a * ((a - 1) + sign * (a + 1) * cw) / a0;
    f.b2 = a * ((a + 1) + sign * (a - 1) * cw - s) / a0;
    f.a1 = 2 * sign * ((a - 1) - sign * (a + 1) * cw) / a0;
    f.a2 = ((a + 1) - sign * (a - 1) * cw - s) / a0;
  }
};
