#pragma once
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

inline unsigned micros() {
  return static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct FakeSerial { template<class... Args> void printf(const char *, Args...) {} };
struct FakeESP {
  unsigned freeHeap = 100000;
  unsigned getFreeHeap() const { return freeHeap; }
  unsigned getMinFreeHeap() const { return 80000; }
};
inline FakeSerial Serial;
inline FakeESP ESP;

namespace audio_tools {
struct AudioInfo { unsigned sample_rate = 44100; int bits_per_sample = 16, channels = 2; };
class AudioOutput {
 public:
  virtual ~AudioOutput() = default;
  virtual size_t write(const uint8_t *, size_t) = 0;
  virtual bool begin() { return true; }
  virtual void end() {}
  virtual int availableForWrite() { return 4096; }
  virtual void setAudioInfo(AudioInfo value) { info = value; }
  virtual AudioInfo audioInfo() { return info; }
 private:
  AudioInfo info;
};
class I2SStream : public AudioOutput {
 public:
  std::vector<uint8_t> written;
  size_t maxWrite = std::numeric_limits<size_t>::max();
  size_t allowance = std::numeric_limits<size_t>::max();
  size_t write(const uint8_t *data, size_t length) override {
    size_t count = std::min(length, std::min(maxWrite, allowance));
    written.insert(written.end(), data, data + count);
    allowance -= count;
    return count;
  }
};
}
