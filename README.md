# esp32-car-dash

My car (a 2007 Mazda3) has an aux jack but no Bluetooth, so I built a receiver out of an ESP32 with a little OLED screen. My phone streams music to it, a PCM5102A DAC sends the audio to the aux input, and the screen shows what's playing along with the time and weather.

There's also a small iPhone app, Car Dash, that sends the board weather and phone battery info and controls the audio effects.

## What it does

- Shows up as a Bluetooth speaker called **ESP32 Car Audio** and reconnects when the car starts
- Screen shows the song, artist, progress, clock, city, weather, rain alerts, phone battery and how long you've been driving
- Volume bar pops up when you change the volume on your phone
- Optional bass boost, 3-band EQ, stereo widener and reverb (concert or club), all off by default. With everything off the audio is passed through untouched.
- BOOT button: tap to play/pause, hold to skip, double-tap to switch screens

## Parts

- ESP32 dev board with a built-in 0.96" SSD1306 OLED. It needs to be an original ESP32 like the WROOM-32E, since the S3 and C3 don't do Bluetooth audio.
- PCM5102A DAC board (the purple one with a 3.5 mm jack)
- USB car charger, micro-USB cable, aux cable

## Wiring

| DAC | ESP32 |
|---|---|
| VIN | 5V / VIN |
| GND | GND |
| LCK | GPIO 25 |
| DIN | GPIO 27 |
| BCK | GPIO 26 |
| SCK | GND |

On my DAC board the solder jumpers on the back are set H1L, H2L, H3H, H4L. If you get silence, check those first (H3 is the mute).

## Firmware

It's a [PlatformIO](https://platformio.org/) project. Plug the board in and run:

```
pio run -t upload
```

`sh tests/run.sh` runs the audio effect tests on your computer.

## iPhone app

The app lives in `ios/`. You need Xcode and [XcodeGen](https://github.com/yonaskolb/XcodeGen):

```
cd ios
xcodegen generate
open CarDash.xcodeproj
```

Change `DEVELOPMENT_TEAM` in `ios/project.yml` to your own team ID before building. Give the app "Always" location access so it can update the weather in the background when you start the car.

## Built with

[ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) and [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) by Phil Schatzmann, and the Adafruit SSD1306 and GFX libraries. Weather is from [Open-Meteo](https://open-meteo.com/).
