#pragma once

void startStatusLed();
// Call from the main loop; all patterns run without delays.
void updateStatusLed(bool audioConnected, bool newRainAlert);
void printStatusLed();
