#include <Arduino.h>
#include "status_led.h"

// The board's GPIO2 LED is active high. Keep it dim: about 2% peak PWM duty.
static const uint8_t LED_PIN = 2;
static const uint32_t MAX_DUTY = 80;
static bool ready = false, rainFlashing = false;
static uint32_t rainStartedAt = 0, lastUpdate = 0;
static bool wasConnected = true;
static uint32_t reconnectStartedAt = 0;

void startStatusLed() {
  digitalWrite(LED_PIN, LOW);
  pinMode(LED_PIN, OUTPUT);
  ready = ledcAttach(LED_PIN, 5000, 12);
  if (ready) ledcWrite(LED_PIN, 0);
  Serial.println(ready ? "GPIO2 status LED ready (max duty 80/4095)" : "GPIO2 PWM unavailable; LED kept off");
}

void updateStatusLed(bool audioConnected, bool newRainAlert) {
  if (!ready) return;
  uint32_t now = millis();
  bool connectionChanged = audioConnected != wasConnected;
  if (connectionChanged) {
    wasConnected = audioConnected;
    reconnectStartedAt = now;
    Serial.println(audioConnected ? "GPIO2 connected: off" : "GPIO2 reconnecting: dim pulse");
  }
  if (newRainAlert) {
    rainFlashing = true;
    rainStartedAt = now;
    Serial.println("GPIO2 rain alert: two dim flashes");
  }
  if (!connectionChanged && !newRainAlert && now - lastUpdate < 20) return;
  lastUpdate = now;

  uint32_t duty = 0;
  if (rainFlashing) {
    uint32_t age = now - rainStartedAt;
    if (age < 150 || (age >= 350 && age < 500)) duty = MAX_DUTY;
    if (age >= 650) rainFlashing = false;
  }
  if (!rainFlashing && !audioConnected) {
    // A gentle rise/fall over 2.4 seconds, then a 0.6-second dark pause.
    uint32_t phase = (now - reconnectStartedAt) % 3000;
    uint32_t ramp = phase < 1200 ? phase : (phase < 2400 ? 2400 - phase : 0);
    duty = MAX_DUTY * ramp * ramp / (1200UL * 1200UL);
  }
  ledcWrite(LED_PIN, duty);
}

void printStatusLed() {
  Serial.printf("GPIO2 PWM duty %lu/4095, frequency %lu Hz\n",
                (unsigned long)(ready ? ledcRead(LED_PIN) : 0),
                (unsigned long)(ready ? ledcReadFreq(LED_PIN) : 0));
}
