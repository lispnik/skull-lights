// Skull eyes for a WS2811 strip.
//
// LEDs are wired in pairs: LED 2k is skull k's left eye, LED 2k+1 is its right eye.
//
// Effects:
//   - Every skull breathes a soft red glow, each at its own speed and phase.
//   - Every so often a random skull flares up into flickering fire
//     (deep red -> orange -> yellow -> white-hot), then calms back down.
//   - Each skull blinks at random intervals (sometimes a quick double blink).
//   - Occasionally a skull winks: one eye closes and holds for a moment.

#include <Arduino.h>
#include <FastLED.h>

// ---------------- Hardware ----------------
#define DATA_PIN    6
#define NUM_LEDS    50
#define COLOR_ORDER RGB   // If red shows up as green/blue, try GRB or BRG.
#define NUM_SKULLS  (NUM_LEDS / 2)

// ---------------- Tuning ----------------
// Master brightness (0-255). Keep a bit of headroom for the fire flares.
const uint8_t MASTER_BRIGHTNESS = 255;
const uint8_t FRAME_MS = 10;  // ~100 fps

// Idle breathing glow.
const uint8_t  PULSE_MIN = 12;           // dimmest point of the breath
const uint8_t  PULSE_MAX = 90;           // brightest point of the breath
const uint16_t PULSE_PERIOD_MIN_MS = 3000;
const uint16_t PULSE_PERIOD_MAX_MS = 6500;

// Fire flares.
const uint8_t  MAX_FIRES = 3;            // max skulls on fire at once
const uint16_t FIRE_GAP_MIN_MS = 1500;   // time between new flares
const uint16_t FIRE_GAP_MAX_MS = 7000;
const uint16_t FIRE_LEN_MIN_MS = 2500;   // how long a flare lasts
const uint16_t FIRE_LEN_MAX_MS = 7000;
const uint16_t FIRE_RAMP_IN_MS = 350;
const uint16_t FIRE_RAMP_OUT_MS = 1200;

// Blinks and winks.
const uint16_t BLINK_GAP_MIN_MS = 4000;  // time between eyelid events per skull
const uint16_t BLINK_GAP_MAX_MS = 16000;
const uint8_t  WINK_PERCENT = 12;        // chance an eyelid event is a wink
const uint8_t  DOUBLE_BLINK_PERCENT = 15;

// ---------------- Fire palette ----------------
// Never goes fully dark: the coolest part of the flame is still a deep red ember.
DEFINE_GRADIENT_PALETTE(fire_gp){
    0,   90,   0,   0,   // deep ember red
    70,  200,  0,   0,   // red
    130, 255,  40,  0,   // red-orange
    180, 255,  110, 0,   // orange
    220, 255,  190, 20,  // yellow
    255, 255,  255, 170  // white-hot
};
static CRGBPalette16 firePalette = fire_gp;

// ---------------- State ----------------
enum LidAction : uint8_t { LID_NONE, LID_BLINK, LID_DOUBLE_BLINK, LID_WINK_LEFT, LID_WINK_RIGHT };

struct Skull {
    uint32_t pulsePhase;  // top 16 bits = position in the breath cycle
    uint32_t pulseRate;   // phase added per millisecond

    bool     onFire;
    uint32_t fireStart;
    uint16_t fireLen;

    LidAction lid;
    uint32_t  lidStart;
    uint32_t  nextLidAt;
};

static CRGB leds[NUM_LEDS];
static Skull skulls[NUM_SKULLS];
static uint8_t sparks[NUM_LEDS];  // short bright pops on top of the fire flicker
static uint32_t nextFireAt;

// ---------------- Helpers ----------------

// Eyelid openness (255 = open, 0 = shut) for a close/hold/open motion.
// Sets `done` once the motion has finished.
static uint8_t lidCurve(uint32_t t, uint16_t closeMs, uint16_t holdMs, uint16_t openMs, bool &done) {
    done = false;
    if (t < closeMs) return 255 - (uint8_t)((t * 255UL) / closeMs);
    t -= closeMs;
    if (t < holdMs) return 0;
    t -= holdMs;
    if (t < openMs) return (uint8_t)((t * 255UL) / openMs);
    done = true;
    return 255;
}

static void scheduleLid(Skull &s, uint32_t now) {
    s.lid = LID_NONE;
    s.nextLidAt = now + random16(BLINK_GAP_MIN_MS, BLINK_GAP_MAX_MS);
}

static void startLid(Skull &s, uint32_t now) {
    uint8_t roll = random8(100);
    if (roll < WINK_PERCENT) {
        s.lid = random8(2) ? LID_WINK_LEFT : LID_WINK_RIGHT;
    } else if (roll < WINK_PERCENT + DOUBLE_BLINK_PERCENT) {
        s.lid = LID_DOUBLE_BLINK;
    } else {
        s.lid = LID_BLINK;
    }
    s.lidStart = now;
}

// Works out how open each eye is right now.
static void updateLids(Skull &s, uint32_t now, uint8_t &left, uint8_t &right) {
    left = right = 255;

    if (s.lid == LID_NONE) {
        if ((int32_t)(now - s.nextLidAt) >= 0) startLid(s, now);
        else return;
    }

    uint32_t t = now - s.lidStart;
    bool done = false;
    uint8_t open = 255;

    switch (s.lid) {
        case LID_BLINK:
            open = lidCurve(t, 70, 50, 110, done);
            left = right = open;
            break;

        case LID_DOUBLE_BLINK: {
            const uint16_t one = 60 + 30 + 90;
            const uint16_t gap = 120;
            if (t < one) {
                open = lidCurve(t, 60, 30, 90, done);
                done = false;
            } else if (t < one + gap) {
                open = 255;
            } else {
                open = lidCurve(t - one - gap, 60, 30, 90, done);
            }
            left = right = open;
            break;
        }

        case LID_WINK_LEFT:
        case LID_WINK_RIGHT: {
            // Slow, deliberate close, a held wink, then open.
            open = lidCurve(t, 120, 450, 180, done);
            // The other eye squints slightly for character.
            uint8_t squint = 255 - scale8(255 - open, 60);
            if (s.lid == LID_WINK_LEFT) { left = open; right = squint; }
            else                        { right = open; left = squint; }
            break;
        }

        default:
            break;
    }

    if (done) scheduleLid(s, now);
}

// Soft red breathing glow.
static CRGB pulseColor(Skull &s, uint32_t dt) {
    s.pulsePhase += s.pulseRate * dt;
    uint8_t wave = sin8((uint8_t)(s.pulsePhase >> 24));
    wave = scale8(wave, wave);  // spend longer near dim -- feels more like breathing
    uint8_t v = PULSE_MIN + scale8(wave, PULSE_MAX - PULSE_MIN);
    return CRGB(v, 0, 0);
}

// Flickering fire color for one eye.
static CRGB fireColor(uint8_t eye, uint32_t now) {
    // Two layers of noise: a slow lick of flame and a fast flicker.
    uint8_t slow = inoise8(eye * 2000, (uint16_t)(now * 2));
    uint8_t fast = inoise8(eye * 2000 + 9000, (uint16_t)(now * 7));
    int16_t heat = ((int16_t)slow * 3 + (int16_t)fast * 2) / 5;

    // Noise mostly sits mid-range; stretch it to use the full palette.
    heat = (heat - 70) * 2;
    if (heat < 0) heat = 0;
    if (heat > 255) heat = 255;

    // Random sparks: sudden white-hot pops that quickly die back.
    if (random8() < 5) sparks[eye] = random8(160, 255);
    uint8_t h = qadd8((uint8_t)heat, sparks[eye]);
    sparks[eye] = qsub8(sparks[eye], 25);

    return ColorFromPalette(firePalette, h, 255, LINEARBLEND);
}

// How strongly the fire shows (0-255), easing in and out. Ends the flare when done.
static uint8_t fireEnvelope(Skull &s, uint32_t now) {
    if (!s.onFire) return 0;
    uint32_t t = now - s.fireStart;
    if (t >= s.fireLen) {
        s.onFire = false;
        return 0;
    }
    if (t < FIRE_RAMP_IN_MS) return ease8InOutQuad((uint8_t)((t * 255UL) / FIRE_RAMP_IN_MS));
    uint32_t left = s.fireLen - t;
    if (left < FIRE_RAMP_OUT_MS) return ease8InOutQuad((uint8_t)((left * 255UL) / FIRE_RAMP_OUT_MS));
    return 255;
}

static void maybeStartFire(uint32_t now) {
    if ((int32_t)(now - nextFireAt) < 0) return;
    nextFireAt = now + random16(FIRE_GAP_MIN_MS, FIRE_GAP_MAX_MS);

    uint8_t burning = 0;
    for (uint8_t i = 0; i < NUM_SKULLS; i++) burning += skulls[i].onFire;
    if (burning >= MAX_FIRES) return;

    // Pick a random skull that isn't already burning.
    for (uint8_t tries = 0; tries < 10; tries++) {
        Skull &s = skulls[random8(NUM_SKULLS)];
        if (s.onFire) continue;
        s.onFire = true;
        s.fireStart = now;
        s.fireLen = random16(FIRE_LEN_MIN_MS, FIRE_LEN_MAX_MS);
        return;
    }
}

// ---------------- Main ----------------

void setup() {
    FastLED.addLeds<WS2811, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(MASTER_BRIGHTNESS);
    FastLED.clear(true);

    // Seed the random generator from a floating analog pin.
    uint16_t seed = 0;
    for (uint8_t i = 0; i < 16; i++) seed = (seed << 1) ^ analogRead(A0);
    random16_set_seed(seed);
    randomSeed(seed);

    uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_SKULLS; i++) {
        Skull &s = skulls[i];
        uint16_t period = random16(PULSE_PERIOD_MIN_MS, PULSE_PERIOD_MAX_MS);
        s.pulseRate = 0xFFFFFFFFUL / period;
        s.pulsePhase = ((uint32_t)random16()) << 16;
        s.onFire = false;
        scheduleLid(s, now);
    }
    nextFireAt = now + random16(FIRE_GAP_MIN_MS, FIRE_GAP_MAX_MS);
}

void loop() {
    static uint32_t lastFrame = millis();
    uint32_t now = millis();
    uint32_t dt = now - lastFrame;
    if (dt < FRAME_MS) return;
    lastFrame = now;

    maybeStartFire(now);

    for (uint8_t i = 0; i < NUM_SKULLS; i++) {
        Skull &s = skulls[i];
        uint8_t leftEye = i * 2;
        uint8_t rightEye = leftEye + 1;

        CRGB base = pulseColor(s, dt);
        CRGB left = base;
        CRGB right = base;

        uint8_t fire = fireEnvelope(s, now);
        if (fire) {
            left = blend(base, fireColor(leftEye, now), fire);
            right = blend(base, fireColor(rightEye, now), fire);
        }

        uint8_t openL, openR;
        updateLids(s, now, openL, openR);
        left.nscale8(openL);
        right.nscale8(openR);

        leds[leftEye] = left;
        leds[rightEye] = right;
    }

    FastLED.show();
}
