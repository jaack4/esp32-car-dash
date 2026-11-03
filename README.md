# esp32-car-dash

A Bluetooth audio receiver for cars with an aux input, built on an ESP32 with a 0.96" OLED. Music streams from your phone to a PCM5102A DAC connected to the aux jack, and the screen shows the current song, time and weather. An iPhone companion app, Car Dash, sends weather and phone battery info to the board and controls the audio effects.

## Features

- Appears as a Bluetooth speaker named **ESP32 Car Audio** and reconnects automatically
- Displays song, artist, progress, clock, city, weather, rain alerts, phone battery and drive time
- Shows a volume bar when the phone's volume changes
- Bass boost, 3-band EQ, stereo widener and reverb, all off by default. With every effect off, audio passes through unmodified.
- BOOT button: tap to play/pause, hold to skip, double-tap to switch screens

## Hardware

- ESP32 dev board with a built-in SSD1306 OLED. Must be an original ESP32 (e.g. WROOM-32E); the S3 and C3 don't support Bluetooth audio.
- PCM5102A DAC board with a 3.5 mm jack
- USB car charger, micro-USB cable and aux cable

## Wiring

| DAC | ESP32 |
|---|---|
| VIN | 5V / VIN |
| GND | GND |
| LCK | GPIO 25 |
| DIN | GPIO 27 |
| BCK | GPIO 26 |
| SCK | GND |

Set the solder jumpers on the back of the DAC to H1L, H2L, H3H, H4L. H3 controls mute, so check it first if there's no sound.

## Firmware

Build and flash with [PlatformIO](https://platformio.org/):

```
pio run -t upload
```

Run the audio effect tests on a desktop with `sh tests/run.sh`.

## iPhone app

The app is in `ios/` and uses [XcodeGen](https://github.com/yonaskolb/XcodeGen) to generate the Xcode project:

```
cd ios
xcodegen generate
open CarDash.xcodeproj
```

Set `DEVELOPMENT_TEAM` in `ios/project.yml` to your Apple team ID before building. Allow "Always" location access so weather updates in the background.

## Credits

Uses [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) and [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) by Phil Schatzmann, and the Adafruit SSD1306 and GFX libraries. Weather data from [Open-Meteo](https://open-meteo.com/).

## License

MIT
