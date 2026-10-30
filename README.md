# esp32-car-dash

A Bluetooth-to-AUX car audio receiver with a now-playing screen, built on an ESP32
dev board with a built-in 0.96" OLED. Your phone pairs with it like a Bluetooth
speaker, music goes out through a PCM5102A DAC into the car's AUX input, and the
screen shows the song, artist, time and progress.

Made for a car without Bluetooth (a 2007 Mazda3), but it works with any AUX input.

## Features

- Shows up as **ESP32 Car Audio**; reconnects to your phone automatically when the car starts
- Song title (scrolls when long), artist, elapsed / total time and a progress bar
- BOOT button: **tap** to play/pause, **hold** to skip to the next track
- Phone volume buttons control the volume
- Works with iPhone and Android, no app needed
- Accurate time even when you seek within a song: iPhones don't push the playback
  position, so the firmware asks for it once a second (AVRCP GetPlayStatus) and runs
  its own clock in between

## Parts

| Part | Notes |
|---|---|
| ESP32 dev board with built-in SSD1306 OLED | ESP32-WROOM-32E (original ESP32: Classic Bluetooth audio is required, so not an S3/C3). OLED on I2C SDA 21 / SCL 22. |
| PCM5102A I2S DAC board | the common purple board with a 3.5 mm jack (~$5) |
| USB car charger + micro-USB cable | power from the 12 V socket |
| 3.5 mm male-to-male aux cable | DAC to the car's AUX jack |
| Ground loop isolator (optional) | only if you hear engine whine |

## Wiring

| PCM5102A | ESP32 |
|---|---|
| VIN | 3V3 |
| GND | GND |
| LCK | GPIO 25 |
| DIN | GPIO 27 |
| BCK | GPIO 26 |
| SCK | GND |

```
12 V socket → USB car charger → ESP32 ──I2S──▶ PCM5102A ──3.5 mm──▶ car AUX
                                  └──▶ built-in OLED
```

## Build and flash

Install [PlatformIO](https://platformio.org/), plug in the board, then:

```bash
pio run -t upload
pio device monitor          # optional: connection, track info and audio rate logs
```

On your phone, open Bluetooth settings and pair with **ESP32 Car Audio**.

To change the Bluetooth name or pins, edit the constants at the top of
[`src/main.cpp`](src/main.cpp).

## Installing in the car

- **Insulate the bottoms of both boards** (electrical tape or foam tape) and hot-glue
  the jumper wires at the pins so nothing shorts or shakes loose. Any unused header
  pins can be pushed into plain packing foam (not black anti-static foam, which conducts).
- **Keep metal away from the ESP32's antenna end**: a metal case blocks Bluetooth.
- **Keep it out of direct sun**: a parked car's dashboard gets hotter than the OLED
  can handle (~70 °C).
- An external push button between **GPIO 0 and GND** works exactly like BOOT, which
  is handy once the board is in a case.
- The board may restart while the engine cranks. That's harmless; the phone reconnects
  in a few seconds.

## Troubleshooting

| Problem | Fix |
|---|---|
| Screen shows the song but no sound | Check the solder jumper pads on the back of the DAC board: XSMT (soft mute) must be set high. Also check SCK is tied to GND. |
| Whine that changes with engine speed | Ground loop: add a ground loop isolator on the aux cable. |
| Distortion at high volume | The DAC's line output is hot for some car inputs: lower the phone volume and turn the car up. |
| Doesn't reconnect after many restarts | Tap **ESP32 Car Audio** in the phone's Bluetooth settings. |

## Built with

- [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) and
  [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools) by Phil Schatzmann
- [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306) and Adafruit GFX
