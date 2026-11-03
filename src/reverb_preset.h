#pragma once
#include <cstring>
#include <initializer_list>

enum class ReverbPreset { Concert, Club };

inline const char *reverbPresetName(ReverbPreset preset) {
  return preset == ReverbPreset::Club ? "club" : "concert";
}

inline bool parseReverbCommand(const char *message, bool &enabled, ReverbPreset &preset) {
  if (std::strcmp(message, "R|0") == 0 || std::strcmp(message, "R|1") == 0) {
    enabled = message[2] == '1';
    return true; // legacy command keeps the current preset
  }
  for (auto candidate : {ReverbPreset::Concert, ReverbPreset::Club}) {
    const char *name = reverbPresetName(candidate);
    if (std::strncmp(message, "R|0|", 4) == 0 || std::strncmp(message, "R|1|", 4) == 0) {
      if (std::strcmp(message + 4, name) == 0) {
        enabled = message[2] == '1';
        preset = candidate;
        return true;
      }
    }
  }
  return false;
}
