# esp32-car-dash

A Bluetooth audio receiver for cars with an aux input, built on an ESP32 with a 0.96" OLED. Music streams from your phone to a PCM5102A DAC connected to the aux jack, and the screen shows the current song, time and weather. An iPhone companion app, Car Dash, sends weather and phone battery info to the board and controls the audio effects.

## Features

- Appears as a Bluetooth speaker named **ESP32 Car Audio** and reconnects to your phone when the car starts
- Three screens, switched by double-tapping BOOT:
  - **Combo:** clock, city and temperature along the top, with the song, artist, progress bar and phone battery below
  - **Music:** larger song title and artist with elapsed and total time
  - **Weather:** temperature, conditions, high/low, wind, and rain alerts or drive time
- Shows a volume bar when the phone's volume changes
- Rain alerts ("Rain in 20 min") from a 2-hour forecast, plus two flashes of the onboard LED
- Audio effects, all off by default:
  - **Bass boost:** light, medium or heavy (+4, +7 or +10 dB around 65 Hz)
  - **EQ:** bass, mid and treble, each -12 to +12 dB
  - **Stereo widener:** subtle, wide or extra. Bass stays centered.
  - **Reverb:** concert or club
- With every effect off, audio passes through unmodified. With boosts on, the level drops slightly and a limiter catches peaks so the aux input doesn't distort.

## Controls

| BOOT button | Action |
|---|---|
| Tap | Play / pause |
| Hold | Next track |
| Double-tap | Next screen |

## Hardware

- ESP32 dev board with a built-in SSD1306 OLED (I2C on GPIO 21/22). Must be an original ESP32 such as the WROOM-32E; the S3 and C3 don't support Bluetooth audio.
- PCM5102A DAC board with a 3.5 mm jack (the common purple one)
- USB car charger, micro-USB cable and 3.5 mm aux cable
- Optional: a ground loop isolator if you hear whine that changes with engine speed

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
pio device monitor    # optional: connection, track and audio logs at 115200 baud
```

To change the Bluetooth name or pins, edit the constants at the top of `src/main.cpp`.

The audio effects have desktop tests (sample rates, frequency response, limiter, click-free switching, bit-exact bypass):

```
sh tests/run.sh
```

## iPhone app

The app is in `ios/` and uses [XcodeGen](https://github.com/yonaskolb/XcodeGen) to generate the Xcode project:

```
brew install xcodegen
cd ios
xcodegen generate
open CarDash.xcodeproj
```

Set `DEVELOPMENT_TEAM` in `ios/project.yml` to your Apple team ID, then build to your phone.

On first launch the app finds the board over Bluetooth LE and remembers it. Allow "Always" location access so it can send weather when the car starts without being opened. Weather comes from [Open-Meteo](https://open-meteo.com/), which needs no API key. Avoid swiping the app away in the app switcher, since iOS won't wake it in the background after that.

## Installing in a car

- Power the board from a USB charger in the 12 V socket, and run the aux cable from the DAC to the car's aux input.
- Insulate the bottom of both boards and secure the jumper wires so nothing shorts or shakes loose.
- Don't put the ESP32's antenna inside a metal case, or Bluetooth will drop out.
- Keep it out of direct sun. A parked car's dashboard gets hotter than the OLED is rated for.

## Troubleshooting

| Problem | Fix |
|---|---|
| Song info on screen but no sound | Check the DAC jumpers (H3 high) and that SCK is grounded |
| Whine that rises with engine speed | Add a ground loop isolator on the aux cable |
| Distortion at high volume | Lower the phone volume and turn up the car |
| Phone doesn't reconnect | Tap **ESP32 Car Audio** in the phone's Bluetooth settings |
| No weather on the screen | Open Car Dash and check that it shows Connected and has location access |

## Credits

Uses [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) and [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) by Phil Schatzmann, and the Adafruit SSD1306 and GFX libraries.

## License

MIT
