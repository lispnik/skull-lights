#include <Arduino.h>
#include <FastLED.h>

#define NUM_LEDS 50
#define DATA_PIN 6

static CRGB leds[NUM_LEDS];

void setup() {
    FastLED.addLeds<WS2811, DATA_PIN>(leds, NUM_LEDS);
    FastLED.setBrightness(192);
    Serial.begin(115200);
    Serial.println("Ready to receive colors.");
}

void loop() {
    auto color = CRGB::Black;
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    delay(500);
    fill_solid(leds, NUM_LEDS, CRGB::HotPink);
    delay(500);
}