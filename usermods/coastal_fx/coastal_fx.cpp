/*
 * Coastal FX - custom WLED effects for the house
 * Target: WLED 16.0.1, classic ESP32, SK6812 RGBW pixels
 *
 * Effects (they show up in the WLED effect list, Home Assistant and the Home app):
 * Direction: everything that travels moves from the segment's first pixel toward its last, like
 * WLED's own chases. To make a strip run the other way, tick "Reversed" for its output in
 * LED Preferences (or the segment's reverse button).
 *   Holiday Bulbs    - alternating bulbs in colors 1/2/3 that chase or swap
 *   Holiday Twinkle  - alternating bulbs with warm-white sparkles
 *   Candy Cane       - moving stripes in colors 1/2
 *   Retro C9         - classic multi-color C9 bulbs with a gentle flicker
 *   Fireworks Burst  - bursts that expand and fade in colors 1/2/3
 *   Ocean Swell      - rolling blue/teal waves with white-chip whitecaps
 *   Lightning Storm  - dim storm clouds with real-looking multi-flash strikes
 *   Sunrise          - slow dawn (or dusk) fade over 1-64 minutes
 *   Boom Fireworks   - sound reactive: real bangs launch bursts (mic)
 *   Pool Shimmer     - sunlight-through-water caustics
 *   Northern Lights  - aurora curtains over a night sky
 *   Las Vegas (231-236):
 *   Vegas Marquee    - casino-sign bulb chaser with a sign's routines (chase, blink, fill, flash)
 *   Neon Sign        - neon tubes that buzz, stutter on, animate and sometimes flicker
 *   Jackpot          - slot machine reels spinning and stopping; three of a kind pays out in gold
 *   Roulette         - a ball racing around a red/black wheel, dropping and settling in a pocket
 *   Dancing Fountains- a fountain show with choreographed jets (or dancing to the mic)
 *   Showgirl Sparkle - pink, gold and violet plumes with glittering sequins
 *   Candy Land (237) - candy colors and white frosting: Candy Path, Gumdrop Pop, Lollipop Swirl
 *
 * Colors: effects that use color slots fall back to sensible defaults if a slot is black,
 * so "Holiday Bulbs" is red/green out of the box and Ocean Swell is blue/teal.
 * Warm white comes from the dedicated W chip. Set White management to "Dual" (not "Accurate")
 * in LED Preferences, or WLED overwrites the W values these effects send.
 *
 * Effect IDs: registered in order starting at 220 (first free id in WLED 16.0.1),
 * so presets can refer to them by number. The Info page lists the actual ids.
 */

#include "wled.h"

namespace {

// ---------------------------------------------------------------- helpers

constexpr uint32_t WARM   = RGBW32(0, 0, 0, 255);     // pure W chip
constexpr uint32_t C_RED  = RGBW32(255, 0, 0, 0);
constexpr uint32_t C_GRN  = RGBW32(0, 200, 0, 0);

// a*b/256 (FastLED scale8)
inline uint8_t sc8(uint8_t a, uint8_t b) { return ((uint16_t)a * (b + 1)) >> 8; }

inline void staticFallback() { SEGMENT.fill(SEGCOLOR(0)); }

// Collect the user's color slots. Black slots are skipped; if none are set, use defaults.
// Returns how many colors were written to out[] (1..3).
uint8_t slotColors(uint32_t out[3], uint32_t d0, uint32_t d1, uint32_t d2 = 0) {
  uint8_t n = 0;
  for (uint8_t s = 0; s < 3; s++) {
    uint32_t c = SEGCOLOR(s);
    if (c) out[n++] = c;
  }
  if (n >= 2) return n;
  // only color 1 set (WLED's default secondary is black): pair it with the default second color
  if (n == 1) { out[1] = (out[0] == d0) ? d1 : d0; return 2; }
  out[0] = d0; out[1] = d1; n = 2;
  if (d2) out[n++] = d2;
  return n;
}

// pixels per bulb from a 0-255 slider: 1..8
inline uint8_t bulbSize(uint8_t v) { return 1 + (v >> 5); }

// ---------------------------------------------------------------- Holiday Bulbs
// Alternating bulbs. Speed = how fast colors move along (0 = still), Bulb size = pixels per bulb.
// Checkbox "Smooth" crossfades instead of stepping.
void mode_holiday_bulbs() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t cols[3];
  const uint8_t n = slotColors(cols, C_RED, C_GRN);
  const uint8_t size = bulbSize(SEGMENT.intensity);

  uint32_t phase16 = 0;  // 8.8 fixed-point bulb offset
  if (SEGMENT.speed) {
    const uint32_t period = 4200 - (uint32_t)SEGMENT.speed * 16;  // ms per bulb step: ~4.2s .. ~120ms
    phase16 = (uint32_t)(((uint64_t)strip.now << 8) / period);
  }
  // colors step toward the end of the strip: bulb b shows the color bulb b-1 had
  const uint32_t back = n - (phase16 >> 8) % n;
  const uint8_t frac = SEGMENT.check1 ? (phase16 & 0xFF) : 0;

  for (int i = 0; i < len; i++) {
    const uint32_t bulb = i / size;
    const uint32_t a = cols[(bulb + back) % n];
    if (frac) {
      const uint32_t b = cols[(bulb + back + n - 1) % n];   // the color arriving from behind
      SEGMENT.setPixelColor(i, color_blend(a, b, frac));
    } else {
      SEGMENT.setPixelColor(i, a);
    }
  }
}
const char _data_holiday_bulbs[] PROGMEM =
  "Holiday Bulbs@Chase speed,Bulb size,,,,Smooth;!,!,!;;1;sx=40,ix=0,o1=0";

// ---------------------------------------------------------------- Holiday Twinkle
// Static alternating bulbs; random bulbs flare to warm white and fade back.
// Speed = how fast sparkles fade, Intensity = how many, Custom1 = bulb size.
void mode_holiday_twinkle() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(len)) { staticFallback(); return; }
  uint8_t *spark = SEGENV.data;
  uint32_t cols[3];
  const uint8_t n = slotColors(cols, C_RED, C_GRN);
  const uint8_t size = bulbSize(SEGMENT.custom1);

  // ~50 updates/s for consistent timing regardless of frame rate
  if (strip.now - SEGENV.step >= 20) {
    SEGENV.step = strip.now;
    const uint8_t decay = 2 + (SEGMENT.speed >> 4);
    for (int i = 0; i < len; i++) spark[i] = spark[i] > decay ? spark[i] - decay : 0;
    // spawn: chance per update scales with intensity and strip length
    const uint16_t tries = 1 + (uint32_t)len * SEGMENT.intensity / 2550;
    for (uint16_t t = 0; t < tries; t++) {
      if (hw_random8() < SEGMENT.intensity) {
        const int b = hw_random16(len / size + 1) * size;  // whole bulb sparkles
        for (int k = b; k < b + size && k < len; k++) spark[k] = 255;
      }
    }
  }
  for (int i = 0; i < len; i++) {
    const uint32_t base = cols[(i / size) % n];
    SEGMENT.setPixelColor(i, color_blend(base, WARM, spark[i]));
  }
}
const char _data_holiday_twinkle[] PROGMEM =
  "Holiday Twinkle@Fade speed,Sparkles,Bulb size;!,!,!;;1;sx=96,ix=60,c1=0";

// ---------------------------------------------------------------- Candy Cane
// Moving stripes with soft edges. Speed = movement (0 = still), Intensity = stripe width.
void mode_candy_cane() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t cols[3];
  // red + warm white by default
  slotColors(cols, C_RED, WARM);
  const uint32_t a = cols[0], b = cols[1];
  const uint16_t width = 2 + (SEGMENT.intensity >> 4);   // 2..17 px per stripe
  const uint32_t period = (uint32_t)width * 2;          // one red + one white stripe
  // 8.8 fixed point: how far the stripes have moved toward the end, kept within one period
  const uint32_t per16 = period << 8;
  uint32_t back16 = 0;
  if (SEGMENT.speed) back16 = per16 - (uint32_t)((((uint64_t)strip.now * SEGMENT.speed) >> 7) % per16);
  for (int i = 0; i < len; i++) {
    const uint32_t p16 = ((uint32_t)i << 8) + back16;
    const uint32_t pos = (p16 >> 8) % period;
    const uint8_t frac = p16 & 0xFF;
    const bool inA = pos < width;
    // soften the pixel at each stripe boundary
    if (pos == (uint32_t)width - 1)      SEGMENT.setPixelColor(i, color_blend(a, b, frac));
    else if (pos == period - 1) SEGMENT.setPixelColor(i, color_blend(b, a, frac));
    else                        SEGMENT.setPixelColor(i, inA ? a : b);
  }
}
const char _data_candy_cane[] PROGMEM = "Candy Cane@Speed,Stripe width;!,!;;1;sx=48,ix=48";

// ---------------------------------------------------------------- Retro C9
// Vintage big-bulb look: red, green, cobalt, orange, warm gold. Speed = flicker amount.
void mode_retro_c9() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  static const uint32_t C9[5] = {
    RGBW32(255, 10, 0, 0),    // red
    RGBW32(0, 170, 20, 0),    // green
    RGBW32(10, 40, 255, 0),   // cobalt blue
    RGBW32(255, 70, 0, 0),    // orange
    RGBW32(120, 50, 0, 200),  // warm gold (uses W chip)
  };
  const uint8_t size = bulbSize(SEGMENT.intensity);
  const uint8_t depth = SEGMENT.speed >> 2;  // up to ~25% dimming
  for (int i = 0; i < len; i++) {
    const uint32_t bulb = i / size;
    uint32_t c = C9[bulb % 5];
    if (depth) {
      // each bulb gets its own slow wobble from 2D noise (bulb, time)
      const uint8_t nz = perlin8(bulb * 97, strip.now >> 3);
      const uint8_t dim = sc8(nz, depth);
      c = color_fade(c, 255 - dim, true);
    }
    SEGMENT.setPixelColor(i, c);
  }
}
const char _data_retro_c9[] PROGMEM = "Retro C9@Flicker,Bulb size;;;1;sx=60,ix=0";

// ---------------------------------------------------------------- Fireworks Burst
// Bursts that expand outward from a random point, flash warm white, then fade.
// Speed = launch rate, Intensity = burst size, Custom1 = trail fade.
struct Burst { uint16_t center; uint16_t age; uint8_t radius; uint8_t color; };
constexpr uint8_t MAX_BURSTS = 6;

void mode_fireworks_burst() {
  const int len = SEGLEN;
  if (len < 3 || !SEGENV.allocateData(sizeof(Burst) * MAX_BURSTS)) { staticFallback(); return; }
  Burst *bursts = reinterpret_cast<Burst *>(SEGENV.data);
  uint32_t cols[3];
  const uint8_t n = slotColors(cols, C_RED, WARM, RGBW32(0, 40, 255, 0));  // red, white, blue

  if (SEGENV.call == 0) SEGMENT.fill(BLACK);
  if (strip.now - SEGENV.step < 20) return;  // ~50 updates/s
  SEGENV.step = strip.now;

  SEGMENT.fadeToBlackBy(8 + (SEGMENT.custom1 >> 3));

  // launch
  if (hw_random8() < (SEGMENT.speed >> 3) + 1) {
    for (uint8_t k = 0; k < MAX_BURSTS; k++) {
      if (bursts[k].age == 0) {
        bursts[k].center = hw_random16(len);
        bursts[k].age = 1;
        bursts[k].radius = 3 + ((uint32_t)(SEGMENT.intensity + 1) * min(len, 120) / 512);
        bursts[k].color = hw_random8(n);
        break;
      }
    }
  }

  for (uint8_t k = 0; k < MAX_BURSTS; k++) {
    Burst &b = bursts[k];
    if (!b.age) continue;
    const uint16_t r = b.age;
    if (r == 1) SEGMENT.setPixelColor((int)b.center, WARM);   // launch flash
    if (r <= b.radius) {
      const uint8_t bright = 255 - (uint32_t)r * 200 / (b.radius + 1);
      const uint32_t c = color_fade(cols[b.color], bright, true);
      const int lo = (int)b.center - r, hi = (int)b.center + r;
      if (lo >= 0)  SEGMENT.setPixelColor(lo, c);
      if (hi < len) SEGMENT.setPixelColor(hi, c);
      b.age++;
    } else {
      b.age = 0;  // done, slot is free
    }
  }
}
const char _data_fireworks_burst[] PROGMEM =
  "Fireworks Burst@Launch rate,Burst size,Trail fade;!,!,!;;1;sx=120,ix=128,c1=96";

// ---------------------------------------------------------------- Ocean Swell
// Two sets of waves rolling toward the end of the strip at slightly different speeds, blended
// between colors 1 and 2, with whitecaps from the W chip where their crests meet (the meeting
// points roll the same way). Speed = wave speed, Intensity = whitecaps.
void mode_ocean_swell() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t cols[3];
  slotColors(cols, RGBW32(0, 30, 160, 0), RGBW32(0, 150, 140, 0));  // deep blue, teal
  const uint32_t t = ((uint32_t)strip.now * (SEGMENT.speed + 8)) >> 9;
  const uint8_t foamStart = 255 - (SEGMENT.intensity >> 1);  // higher intensity = more foam
  for (int i = 0; i < len; i++) {
    const uint8_t w1 = sin8_t(i * 6 - t);                   // long swell
    const uint8_t w2 = sin8_t(i * 11 - t - (t >> 1) + 90);  // shorter, a little faster: crests meet further along
    const uint8_t mix = (w1 + w2) >> 1;
    uint32_t c = color_blend(cols[0], cols[1], mix);
    if (SEGMENT.intensity && mix > foamStart) {
      const uint8_t foam = (uint32_t)(mix - foamStart) * 255 / (256 - foamStart);
      c = color_blend(c, WARM, sc8(foam, 200));
    }
    // a slow brightness swell over the whole strip
    c = color_fade(c, 170 + (sin8_t(i * 2 - (t >> 2)) / 3), true);
    SEGMENT.setPixelColor(i, c);
  }
}
const char _data_ocean_swell[] PROGMEM = "Ocean Swell@Wave speed,Whitecaps;!,!;;1;sx=40,ix=100";

// ---------------------------------------------------------------- Lightning Storm
// Dim moving cloud glow in color 1, with lightning: a burst of 2-4 bright flashes on a
// random section, then a random pause. Speed = how often, Intensity = flash size.
struct Storm { uint32_t next; uint16_t start; uint16_t width; uint8_t flashes; uint8_t lit; };

void mode_lightning_storm() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(Storm))) { staticFallback(); return; }
  Storm *s = reinterpret_cast<Storm *>(SEGENV.data);
  uint32_t cols[3];
  slotColors(cols, RGBW32(10, 20, 70, 0), WARM);
  const uint32_t cloud = cols[0];

  if (SEGENV.call == 0) s->next = strip.now + 800;

  // background clouds
  for (int i = 0; i < len; i++) {
    uint32_t c = BLACK;
    if (SEGMENT.check1) {
      const uint8_t nz = perlin8(i * 24, strip.now >> 4);
      c = color_fade(cloud, 40 + sc8(nz, 120), true);
    }
    SEGMENT.setPixelColor(i, c);
  }

  if (strip.now >= s->next) {
    if (s->flashes == 0) {
      // start a new strike
      s->flashes = 2 + hw_random8(3);
      s->width = max(2, (int)((uint32_t)len * (16 + SEGMENT.intensity) / 400));
      s->start = hw_random16(max(1, len - s->width + 1));
      s->lit = 0;
    }
    s->lit = !s->lit;
    if (s->lit) {
      s->next = strip.now + 30 + hw_random8(70);     // flash on
    } else {
      s->flashes--;
      s->next = strip.now + (s->flashes ? 50 + hw_random8(150)          // gap between flashes
                                        : 600 + (uint32_t)(255 - SEGMENT.speed) * 60 + hw_random16(4000)); // pause
    }
  }
  if (s->lit) {
    const uint32_t bolt = cols[1] == WARM ? RGBW32(140, 140, 255, 255) : cols[1];
    for (int i = s->start; i < s->start + s->width && i < len; i++) {
      // brighter core, softer edges
      const int edge = min(i - s->start, s->start + s->width - 1 - i);
      SEGMENT.setPixelColor(i, edge < 2 ? color_fade(bolt, 150, true) : bolt);
    }
  }
}
const char _data_lightning_storm[] PROGMEM =
  "Lightning Storm@Strike rate,Flash size,,,,Cloud glow;!,!;;1;sx=90,ix=96,o1=1";

// ---------------------------------------------------------------- Sunrise
// Night -> deep red -> orange -> warm white over 1-64 minutes (Speed). Checkbox = Sunset
// (runs backwards). Starts over whenever the effect is selected, so it works from a preset
// fired by an alarm or automation.
void mode_sunrise() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(uint32_t))) { staticFallback(); return; }
  uint32_t *start = reinterpret_cast<uint32_t *>(SEGENV.data);
  if (SEGENV.call == 0) *start = strip.now;
  const uint32_t duration = (1 + (SEGMENT.speed >> 2)) * 60000UL;
  const uint32_t el = strip.now - *start;
  uint16_t p = el >= duration ? 65535 : (uint16_t)(((uint64_t)el * 65535) / duration);
  if (SEGMENT.check1) p = 65535 - p;

  // color ramp keyed on progress
  static const uint32_t stops[5] = {
    RGBW32(0, 0, 0, 0),        // night
    RGBW32(90, 4, 0, 0),       // ember
    RGBW32(255, 40, 0, 0),     // red-orange
    RGBW32(255, 110, 10, 90),  // amber + warm white
    RGBW32(120, 60, 10, 255),  // daylight warm white
  };
  const uint32_t seg = (uint32_t)p * 4;          // 0 .. 4*65535
  const uint8_t idx = min(3u, (unsigned)(seg >> 16));
  const uint8_t frac = (seg >> 8) & 0xFF;
  const uint32_t c = (seg >> 16) >= 4 ? stops[4] : color_blend(stops[idx], stops[idx + 1], frac);
  SEGMENT.fill(c);
}
const char _data_sunrise[] PROGMEM = "Sunrise@Minutes,,,,,Sunset;;;1;sx=60,o1=0";

// ---------------------------------------------------------------- Boom Fireworks (sound reactive)
// Listens through the mic (AudioReactive) for sudden, bass-heavy bangs. Each bang launches
// bursts whose size, brightness and count scale with how far the bang rose above the
// background noise. Loud booms launch several bursts at once, crackle, and (optionally)
// flash the whole strip warm white. Colors come from the palette (Rainbow by default).
//   Sensitivity  - higher = reacts to quieter bangs
//   Burst size   - how far bursts spread
//   Fade         - how long trails linger
//   Cooldown     - minimum time between triggers (stops one boom echoing into many)
//   Big-boom flash, Show mic level (tuning aid: a level meter on the first pixels)
// Without the mic (AudioReactive off) it launches at random so it is never just dark.

struct Boom {
  float    pos;      // burst center
  float    maxR;     // final radius in pixels
  uint16_t age;      // frames since launch (0 = slot free)
  uint16_t life;     // frames until gone
  uint8_t  hue;      // palette index
  uint8_t  power;    // 0-255 from the sound
};
struct BoomState {
  float    floor;     // slow-moving background loudness
  float    prev;      // loudness on the previous frame
  uint32_t lastBoom;  // ms of the last trigger
  uint32_t flashUntil;
  uint32_t lastStep;
};
constexpr uint8_t MAX_BOOMS = 12;

// draw c at a fractional pixel position, split across the two nearest pixels
void aaPixel(float x, uint32_t c, int len) {
  if (x < 0.0f || x > len - 1) return;
  const int i = (int)x;
  const uint8_t f = (uint8_t)((x - i) * 255.0f);
  SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), c, 255 - f));
  if (i + 1 < len) SEGMENT.setPixelColor(i + 1, color_blend(SEGMENT.getPixelColor(i + 1), c, f));
}

void launchBoom(Boom *b, int len, uint8_t power) {
  for (uint8_t k = 0; k < MAX_BOOMS; k++) {
    if (b[k].age) continue;
    const float reach = (8 + SEGMENT.intensity) / 263.0f;          // 0.03 .. 1.0
    b[k].pos   = hw_random16(len);
    b[k].maxR  = 2.0f + reach * min(len, 150) * (0.25f + power / 340.0f);
    b[k].age   = 1;
    b[k].life  = 18 + power / 6;                                     // ~0.4 .. 1.1 s at 50 updates/s
    b[k].hue   = hw_random8();
    b[k].power = power;
    return;
  }
}

void mode_boom_fireworks() {
  const int len = SEGLEN;
  if (len < 3 || !SEGENV.allocateData(sizeof(BoomState) + sizeof(Boom) * MAX_BOOMS)) { staticFallback(); return; }
  BoomState *st = reinterpret_cast<BoomState *>(SEGENV.data);
  Boom *booms = reinterpret_cast<Boom *>(SEGENV.data + sizeof(BoomState));

  if (SEGENV.call == 0) { SEGMENT.fill(BLACK); st->floor = 20.0f; }

  // ---- listen
  um_data_t *um = nullptr;
  const bool haveMic = UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) && um && um->u_size >= 3;
  float level = 0;
  if (haveMic) {
    const float smth = *(float *)um->u_data[0];             // smoothed volume 0-255
    const uint8_t *fft = (const uint8_t *)um->u_data[2];   // 16 frequency channels 0-255
    const uint8_t bass = max(fft[0], max(fft[1], fft[2])); // ~43-215 Hz: where a bang's thump lives
    level = (bass * 2.0f + smth) / 3.0f;
  }

  const uint32_t now = strip.now;
  if (now - st->lastStep < 20) return;   // run the simulation at ~50 updates/s
  st->lastStep = now;

  // ---- detect a bang: a sudden jump well above the background
  const float thresh = 8.0f + (255 - SEGMENT.speed) * 0.45f;        // sensitivity 255 -> 8, 0 -> ~123
  const uint16_t cooldown = 40 + SEGMENT.custom2 * 2;               // 40 .. 550 ms
  const float above = level - st->floor;
  const bool sudden = (level - st->prev) > thresh * 0.5f;
  st->prev = level;
  // background follows quiet sound quickly and loud sound slowly, so booms don't raise it much
  st->floor += (level - st->floor) * (level > st->floor ? 0.004f : 0.05f);

  uint8_t power = 0;
  if (haveMic) {
    if (above > thresh && sudden && now - st->lastBoom > cooldown) {
      const float span = max(20.0f, 255.0f - st->floor - thresh);
      power = (uint8_t)constrain(60.0f + (above - thresh) * 195.0f / span, 60.0f, 255.0f);
    }
  } else if (hw_random8() < 3 && now - st->lastBoom > 400) {
    power = hw_random8(60, 255);   // demo mode
  }
  if (power) {
    st->lastBoom = now;
    const uint8_t count = 1 + power / 90;                            // 1 .. 3 bursts
    for (uint8_t n = 0; n < count; n++) launchBoom(booms, len, power);
    if (SEGMENT.check1 && power > 210) st->flashUntil = now + 60;
  }

  // ---- draw
  SEGMENT.fadeToBlackBy(10 + ((255 - SEGMENT.custom1) >> 3));       // Fade: high = long trails

  for (uint8_t k = 0; k < MAX_BOOMS; k++) {
    Boom &b = booms[k];
    if (!b.age) continue;
    const float t = (float)b.age / b.life;                             // 0 .. 1
    const float r = b.maxR * (1.0f - (1.0f - t) * (1.0f - t));         // fast out, slowing down
    const uint8_t bright = (uint8_t)((1.0f - t * 0.85f) * (155 + b.power * 100 / 255));
    const uint32_t col = color_fade(SEGMENT.color_from_palette(b.hue, false, false, 0), bright, true);

    // launch core: a warm white pop
    if (b.age < 4) {
      const int c = (int)b.pos;
      for (int i = c - 1; i <= c + 1; i++) if (i >= 0 && i < len) SEGMENT.setPixelColor(i, RGBW32(255, 200, 120, 255));
    }
    // the two edges of the ring, anti-aliased
    aaPixel(b.pos - r, col, len);
    aaPixel(b.pos + r, col, len);
    // loud booms crackle: random sparks inside the ring
    if (b.power > 150 && t > 0.3f && hw_random8() < b.power - 120) {
      const int sp = (int)(b.pos + (hw_random8() / 127.5f - 1.0f) * r);
      if (sp >= 0 && sp < len) SEGMENT.setPixelColor(sp, color_blend(col, WARM, 140));
    }
    if (++b.age > b.life) b.age = 0;
  }

  if (now < st->flashUntil) {
    for (int i = 0; i < len; i++) SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), WARM, 170));
  }

  // tuning aid: green bar = current loudness, red pixel = trigger point
  if (SEGMENT.check2) {
    const int bars = min(len, 30);
    const int lv = (int)(level * bars / 255.0f);
    const int tp = constrain((int)((st->floor + thresh) * bars / 255.0f), 0, bars - 1);
    for (int i = 0; i < bars; i++) SEGMENT.setPixelColor(i, i < lv ? RGBW32(0, 60, 0, 0) : BLACK);
    SEGMENT.setPixelColor(tp, RGBW32(120, 0, 0, 0));
  }
}
const char _data_boom_fireworks[] PROGMEM =
  "Boom Fireworks@Sensitivity,Burst size,Fade,Cooldown,,Big-boom flash,Show mic level;;!;1f;"
  "sx=170,ix=140,c1=110,c2=60,o1=1,o2=0,pal=11";


// ---------------------------------------------------------------- Pool Shimmer
// Sunlight through moving water, playing on a wall: thin, bright, constantly shifting
// ribbons of light (caustics) over deep pool blue. Two layers of slowly morphing noise
// drift toward the end of the strip at different speeds; where a layer's value crosses its
// midpoint a sharp bright line forms, and where both layers' lines meet they flare brighter.
//   Speed      - how fast the water moves
//   Sparkle    - brightness and sharpness of the light ribbons
//   Ripple size- width of the ripples (small = choppy, large = lazy swell)
//   Color 1 = light ribbons (default crystal aqua-white), Color 2 = water (default pool blue)
//   Checkbox "Warm sun" tints the highlights with the warm-white chip (late-afternoon look)
inline uint8_t ridge(uint8_t n) {
  // 255 on the noise midline, falling off fast either side: makes thin bright lines
  const uint8_t d = n > 128 ? n - 128 : 128 - n;   // 0..128
  uint16_t r = 255 - (d >= 51 ? 255 : d * 5);     // 255 at midline, 0 a short way off it
  r = (r * r) >> 8;                                // sharpen
  r = (r * r) >> 8;
  return (uint8_t)r;
}

void mode_pool_shimmer() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t cols[3];
  slotColors(cols, RGBW32(150, 235, 255, 60), RGBW32(0, 55, 130, 0));   // aqua-white light, pool blue
  const uint32_t light = cols[0], water = cols[1];

  const uint32_t t = (uint32_t)strip.now * (SEGMENT.speed + 16) >> 8;     // shared clock
  const uint16_t cell = 3 + (SEGMENT.custom1 >> 4);                        // pixels per ripple: 3..18
  const uint16_t step = 256 / cell;                                        // noise units per pixel
  const uint16_t gain = 64 + (SEGMENT.intensity >> 1) + (SEGMENT.intensity >> 2);  // 64..255

  for (int i = 0; i < len; i++) {
    const uint16_t x = i * step;
    // two layers, drifting the same way at different speeds and morphing over time (2nd noise axis)
    const uint8_t a = ridge(perlin8(x - (t >> 1),          t >> 2));             // drifts toward the end
    const uint8_t b = ridge(perlin8(x * 3 / 2 - (t >> 2) + 9000, (t >> 3) + 31000)); // finer and slower
    uint16_t c = ((uint16_t)a + b) / 3 + (((uint16_t)a * b) >> 7);         // thin lines, crossings flare
    c = (c * gain) >> 7;
    if (c > 255) c = 255;

    // the water itself breathes slightly between deep and lighter blue
    const uint8_t depth = perlin8(x / 3 + 20000, t >> 4);
    uint32_t px = color_fade(color_blend(water, color_blend(water, light, 40), depth >> 1), 150 + (depth >> 2), true);
    uint32_t hi = SEGMENT.check1 ? color_blend(light, WARM, 110) : light;
    px = color_blend(px, hi, (uint8_t)c);
    SEGMENT.setPixelColor(i, px);
  }
}
const char _data_pool_shimmer[] PROGMEM =
  "Pool Shimmer@Speed,Sparkle,Ripple size,,,Warm sun;!,!;;1;sx=70,ix=150,c1=80,o1=0";


// ---------------------------------------------------------------- Northern Lights
// Aurora curtains over a dark night sky: broad green glows that slowly drift, fold and
// fade, fringed with purple/pink, with fine "rays" rippling through them and slow surges
// of activity. Optional faint stars twinkle in the dark gaps.
//   Speed        - drift speed of the curtains
//   Activity     - how much of the sky is lit and how bright
//   Curtain size - width of the glowing bands
//   Color 1 = main aurora (default green), Color 2 = fringe (default purple), Color 3 = sky
//   Checkbox "Stars"
void mode_northern_lights() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t green = SEGCOLOR(0) ? SEGCOLOR(0) : RGBW32(10, 255, 90, 0);
  uint32_t fringe = SEGCOLOR(1) ? SEGCOLOR(1) : RGBW32(150, 30, 255, 0);
  uint32_t sky = SEGCOLOR(2) ? SEGCOLOR(2) : RGBW32(0, 2, 10, 0);

  const uint32_t t = (uint32_t)strip.now * (SEGMENT.speed + 8) >> 9;      // slow clock
  const uint16_t cell = 6 + (SEGMENT.custom1 >> 3);                        // pixels per curtain: 6..37
  const uint16_t step = 256 / cell;
  // activity: threshold for where curtains glow; a slow "substorm" surge raises it now and then
  const uint8_t surge = sin8_t((uint8_t)(strip.now >> 9)) >> 2;            // 0..63, ~2 min cycle
  const int thresh = 150 - (SEGMENT.intensity >> 1) - surge;               // lower = more lit

  for (int i = 0; i < len; i++) {
    const uint16_t x = i * step;
    // the curtain body: big, slowly folding noise
    // two layers folding against each other so curtains grow, merge and split instead of scrolling
    const int b1 = perlin8(x - t, t >> 2);                                 // drifts toward the end
    const int b2 = perlin8(x * 2 / 3 + (t >> 1) + 22000, (t >> 3) + 5000);
    int body = b1 > b2 ? b1 : b2;
    int glow = (body - thresh) * 255 / (256 - thresh > 1 ? 256 - thresh : 1);
    if (glow < 0) glow = 0;
    if (glow > 255) glow = 255;
    glow = (glow * glow) >> 8;                                             // soft edges
    // rays: fine, faster ripples inside the curtain
    const uint8_t ray = perlin8(x * 4 - (t << 1), t + 7000);
    glow = (glow * (150 + (ray >> 1) + (ray >> 2))) >> 8;                  // 150..341 / 256
    if (glow > 255) glow = 255;

    // colour: green core, purple/pink where the curtain is thin or the fringe noise says so
    const uint8_t fr = perlin8(x / 2 + 15000, (t >> 3) + 3000);
    // thin parts of a curtain and drifting patches go purple/pink; the bright core stays green
    int mixF = (glow < 170 ? (170 - glow) * 3 / 2 : 0) + (fr > 150 ? (fr - 150) * 2 : 0);
    if (mixF > 230) mixF = 230;
    uint32_t aur = color_blend(green, fringe, (uint8_t)mixF);
    uint32_t px = color_blend(sky, aur, (uint8_t)glow);

    // stars in the dark gaps
    if (SEGMENT.check1 && glow < 20) {
      const uint8_t tw = perlin8(i * 911 + 40000, (strip.now >> 3) + i * 37);
      if (((i * 2654435761u) >> 27) == 3 && tw > 140) px = color_blend(px, WARM, (tw - 140) * 2);
    }
    SEGMENT.setPixelColor(i, px);
  }
}
const char _data_northern_lights[] PROGMEM =
  "Northern Lights@Speed,Activity,Curtain size,,,Stars;!,!,!;;1;sx=60,ix=120,c1=100,o1=1";


// ================================================================ Las Vegas
// Six effects with a Las Vegas theme (ids 231-236). Each one has its own signature colors, so it
// looks right the moment it's picked, whatever colors the strip had before. Tick "My colors" to
// use color slots 1-3 instead (black slots are skipped).

constexpr uint32_t GOLD      = RGBW32(255, 100, 0, 120);    // incandescent marquee bulb
constexpr uint32_t BULB_HOT  = RGBW32(255, 190, 110, 255);  // a bulb flashing white-hot
constexpr uint32_t SPARK     = RGBW32(255, 200, 140, 255);  // glitter / spray highlight

// small integer hash: stateless "random" that stays the same for the same input
inline uint32_t vhash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
  return x;
}

// fill out[] with the non-black color slots (if "My colors" is ticked) or the given defaults.
// Returns how many colors (>= 1).
uint8_t vegasColors(bool mine, uint32_t out[3], const uint32_t *defs, uint8_t nDefs) {
  uint8_t n = 0;
  if (mine) for (uint8_t s = 0; s < 3; s++) if (SEGCOLOR(s)) out[n++] = SEGCOLOR(s);
  if (n) return n;
  for (uint8_t k = 0; k < nDefs && k < 3; k++) out[k] = defs[k];
  return nDefs < 3 ? nDefs : 3;
}

// draw a soft dot at a fractional position (adds to what's there), wraps around the strip
void softDot(float x, uint32_t c, int len) {
  while (x < 0) x += len;
  while (x >= len) x -= len;
  const int i = (int)x;
  const uint8_t f = (uint8_t)((x - i) * 255.0f);
  SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), c, 255 - f));
  const int j = (i + 1) % len;
  SEGMENT.setPixelColor(j, color_blend(SEGMENT.getPixelColor(j), c, f));
}

// ---------------------------------------------------------------- Vegas Marquee
// Casino-sign light bulbs. A classic three-circuit chaser (two bulbs on, one off, rolling along),
// and with "Show changes" up it runs a sign's routine: chase, chase back, alternate blink,
// fill-and-empty, flash, and sparkle. "Filament glow" makes the bulbs warm up and cool down
// like real incandescent bulbs, so the off ones never go quite dark.
//   Chase speed, Bulb spacing (1 = every pixel is a bulb, up to 1 bulb in 4 pixels),
//   Show changes (0 = chase only; higher = changes routine more often)
//   Colors (My colors): 1 = bulbs (default gold), 2 = sign background between bulbs (default dark),
//   3 = flash (default white-hot)
struct Marquee { uint32_t sceneStart; uint32_t last; uint8_t scene; };

void mode_vegas_marquee() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(Marquee) + len)) { staticFallback(); return; }
  Marquee *st = reinterpret_cast<Marquee *>(SEGENV.data);
  uint8_t *lvl = SEGENV.data + sizeof(Marquee);            // bulb brightness, one per bulb
  const uint32_t now = strip.now;
  const bool filament = SEGMENT.check1, mine = SEGMENT.check2;
  const uint32_t bulb  = mine && SEGCOLOR(0) ? SEGCOLOR(0) : GOLD;
  const uint32_t bg    = mine ? SEGCOLOR(1) : BLACK;
  const uint32_t flash = mine && SEGCOLOR(2) ? SEGCOLOR(2) : BULB_HOT;
  const int cell = 1 + (SEGMENT.intensity >> 6);           // pixels per bulb: 1..4
  const int nb = (len + cell - 1) / cell;

  if (SEGENV.call == 0) { st->sceneStart = now; st->last = now; st->scene = 0; }
  const uint32_t stepMs = 420 - (uint32_t)SEGMENT.speed * 3 / 2;  // 420 .. 38 ms per step
  if (SEGMENT.custom1) {
    const uint32_t dur = 2500 + (uint32_t)(255 - SEGMENT.custom1) * 50;  // ~15 s .. 2.5 s per routine
    if (now - st->sceneStart >= dur) { st->sceneStart = now; st->scene = (st->scene + 1) % 6; }
  } else st->scene = 0;
  const uint32_t el = now - st->sceneStart;
  const uint32_t steps = el / stepMs;
  const uint32_t fillMs = 1200 + stepMs * 10;               // fill / empty sweep time

  uint32_t color = bulb;
  if (st->scene == 4 && ((steps >> 1) & 1)) color = flash;   // every other flash is white-hot

  // filament: how far a bulb can warm up / cool down since the last frame
  uint32_t dt = now - st->last;
  st->last = now;
  if (dt > 200) dt = 200;
  const uint16_t up = filament ? (uint16_t)min<uint32_t>(255, dt * 255 / 45 + 1) : 255;
  const uint16_t down = filament ? (uint16_t)min<uint32_t>(255, dt * 255 / 140 + 1) : 255;

  for (int b = 0; b < nb; b++) {
    uint8_t target = 0;
    switch (st->scene) {
      case 0: target = ((b + 3 - steps % 3) % 3) ? 255 : 0; break;               // chase
      case 1: target = ((b + steps) % 3) ? 255 : 0; break;                        // chase back
      case 2: target = ((b + steps / 3) & 1) ? 255 : 0; break;                    // alternate blink
      case 3: {                                                                   // fill, then empty
        const uint32_t p = el % (2 * fillMs);
        const int edge = (int)((uint64_t)(p % fillMs) * (nb + 1) / fillMs);
        target = (p < fillMs) == (b < edge) ? 255 : 0;
        break;
      }
      case 4: target = (steps & 1) ? 0 : 255; break;                              // flash
      default: {                                                                  // sparkle
        const uint32_t h = vhash(b * 2654435761u + (now / (stepMs + 40)) * 40503u);
        target = (h & 0xFF) < 40 ? 255 : 120;
        break;
      }
    }
    uint8_t &l = lvl[b];
    if (l < target) l = (target - l > up) ? l + up : target;
    else if (l > target) l = (l - target > down) ? l - down : target;
  }

  // a bulb that's off still glows faintly with filament on
  const uint32_t offC = filament ? color_add(bg, color_fade(color, 14, true)) : bg;
  for (int i = 0; i < len; i++) {
    if (i % cell) { SEGMENT.setPixelColor(i, bg); continue; }
    const uint8_t l = lvl[i / cell];
    // incandescent light rises faster than linear with filament heat
    const uint8_t k = filament ? (uint8_t)(((uint16_t)l * l + 255) >> 8) : l;
    SEGMENT.setPixelColor(i, color_blend(offC, color, k));
  }
}
const char _data_vegas_marquee[] PROGMEM =
  "Vegas Marquee@Chase speed,Bulb spacing,Show changes,,,Filament glow,My colors;Bulbs,Background,Flash;;1;"
  "sx=150,ix=64,c1=160,o1=1,o2=0";

// ---------------------------------------------------------------- Neon Sign
// A row of neon tubes in classic sign colors with the faint buzz of real neon. When the effect
// starts the tubes stutter on one at a time. With Animation up it runs like an animated sign:
// tubes light one after another, hold, blink three times, go dark, repeat (0 = always on).
// "Faulty tubes" now and then makes one tube flicker like it's on its way out.
//   Animation, Tube length (4-35 pixels, 1 dark pixel between tubes), Faulty tubes
//   Colors (My colors): tubes cycle through slots 1-3. Default: pink, blue, red-orange, lime,
//   violet, turquoise
void mode_neon_sign() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  static const uint32_t NEON[6] = {
    RGBW32(255, 0, 70, 0),    // hot pink
    RGBW32(0, 80, 255, 0),    // electric blue
    RGBW32(255, 30, 0, 0),    // neon red-orange (the real neon gas color)
    RGBW32(90, 255, 0, 0),    // lime
    RGBW32(150, 0, 255, 0),   // violet
    RGBW32(0, 255, 150, 0),   // turquoise
  };
  uint32_t mineCols[3];
  uint8_t n = 0;
  if (SEGMENT.check1) for (uint8_t s = 0; s < 3; s++) if (SEGCOLOR(s)) mineCols[n++] = SEGCOLOR(s);
  const uint32_t *cols = n ? mineCols : NEON;
  if (!n) n = 6;

  const uint32_t now = strip.now;
  if (SEGENV.call == 0) SEGENV.step = now;                  // power-on time
  const uint32_t on = now - SEGENV.step;
  const int tl = 4 + (SEGMENT.intensity >> 3);              // tube length 4..35
  const int pitch = tl + 1;
  const int nt = (len + pitch - 1) / pitch;

  // animated sign: build (5u), hold (6u), blink x3 (3u), dark (1u)
  int lit = nt;                 // tubes 0..lit-1 are on
  bool all = true;              // whole sign on (false while blinking off or dark)
  int igniting = -1;            // tube just switched on: stutters
  uint32_t ignAge = 0;
  if (SEGMENT.speed) {
    const uint32_t u = 1300 - (uint32_t)SEGMENT.speed * 9 / 2;   // 1300 .. 152 ms
    const uint32_t ph = on % (15 * u);
    if (ph < 5 * u) {
      const uint32_t per = 5 * u / nt + 1;
      lit = ph / per + 1;
      igniting = lit - 1;
      ignAge = ph % per;
    } else if (ph < 11 * u) {
      // hold
    } else if (ph < 14 * u) {
      all = ((ph - 11 * u) / (u / 2)) % 2 == 0;
    } else {
      all = false;
    }
  }

  // which tube is faulty right now (changes every 4 s, only some windows have a fault)
  const uint32_t win = now / 4000;
  const uint32_t wh = vhash(win * 7919u + SEGENV.step);
  const bool faultNow = SEGMENT.custom1 && (wh & 0xFF) < SEGMENT.custom1 && (now % 4000) < 300 + ((wh >> 8) & 0x7FF);
  const int faulty = faultNow ? (int)((wh >> 20) % nt) : -1;

  for (int i = 0; i < len; i++) {
    const int t = i / pitch, p = i % pitch;
    if (p == tl) { SEGMENT.setPixelColor(i, BLACK); continue; }    // gap between tubes
    uint8_t bri = 0;
    if (all && t < lit) {
      bri = 224 + (perlin8(t * 211 + p * 9, now >> 1) >> 3);        // gentle buzz: 224..255
      // power-on: each tube stutters on after its own small delay
      const uint32_t delay = 120 + vhash(t + 17) % 1300;
      if (on < delay) bri = 0;
      else if (on < delay + 260 && (vhash((on / 33) * 31 + t) & 3) == 0) bri = 0;
      // the tube the animation just lit stutters on too
      if (t == igniting && ignAge < 220 && (vhash((on / 30) * 17 + t) & 1)) bri = 0;
      // failing tube: rapid irregular flicker, dimmer when it does light
      if (t == faulty) {
        const uint8_t h = vhash((now / 28) * 131 + t) & 7;
        bri = h < 3 ? 0 : (h < 5 ? 70 : 200);
      }
    }
    if (!bri) { SEGMENT.setPixelColor(i, BLACK); continue; }
    // the ends of a tube (electrodes) glow a little less
    if (p == 0 || p == tl - 1) bri = sc8(bri, 175);
    SEGMENT.setPixelColor(i, color_fade(cols[t % n], bri, true));
  }
}
const char _data_neon_sign[] PROGMEM =
  "Neon Sign@Animation,Tube length,Faulty tubes,,,My colors;!,!,!;;1;sx=90,ix=96,c1=60,o1=0";

// ---------------------------------------------------------------- Jackpot
// A slot machine. The strip is split into reels (3 by default) showing colored symbols: cherry
// red, orange, plum, emerald, sapphire and a white BAR. All reels spin, then stop one after
// another with a little bounce. Three of a kind is a jackpot: the machine strobes gold, then
// gold lights chase along the whole strip with a shower of sparkling coins. Otherwise it holds
// the result for a moment (center symbols bright, the rest dim) and spins again.
//   Pace, Jackpot odds (0 = about 1 spin in 30, max = most spins), Reels (3-7)
struct Reel { float p0, pf; uint32_t stop; };
struct SlotGame { uint32_t start, end; uint8_t nr, win; uint8_t sym[8]; Reel reel[8]; };
constexpr uint8_t NSYM = 6;
const uint32_t SYMBOLS[NSYM] = {
  RGBW32(255, 0, 10, 0),     // cherry
  RGBW32(255, 85, 0, 0),     // orange
  RGBW32(150, 0, 255, 0),    // plum
  RGBW32(0, 220, 30, 0),     // emerald
  RGBW32(0, 60, 255, 0),     // sapphire
  RGBW32(40, 40, 40, 255),   // BAR (white chip)
};
// symbol order on reel r: a different shuffle per reel
inline uint8_t reelSym(uint8_t r, int k) { return (uint8_t)((k * 5 + r * 2) % NSYM); }

void mode_jackpot() {
  const int len = SEGLEN;
  if (len < 4 || !SEGENV.allocateData(sizeof(SlotGame))) { staticFallback(); return; }
  SlotGame *g = reinterpret_cast<SlotGame *>(SEGENV.data);
  const uint32_t now = strip.now;
  int nr = 3 + ((SEGMENT.custom1 * 5) >> 8);               // reels 3..7
  if (nr > len / 4) nr = len / 4;
  if (nr < 1) nr = 1;
  const int L = len / nr - (nr > 1 ? 1 : 0);                // pixels per reel window (1-pixel frame between)
  const int s = max(2, (L + 1) / 3);                        // pixels per symbol: 3 symbols in view
  const float C = (float)s * NSYM;                          // one turn of the reel, in pixels
  const float v = s * (8.0f + SEGMENT.speed / 32.0f) / 1000.0f;  // spin speed, pixels per ms
  const uint32_t A = 300, D = 450;                          // spin-up and slow-down times, ms

  // ---- new round
  if (SEGENV.call == 0 || g->nr != nr || (int32_t)(now - g->end) >= 0) {
    const bool fresh = SEGENV.call == 0 || g->nr != nr;
    g->nr = nr;
    g->start = now;
    const uint8_t odds = 8 + ((SEGMENT.intensity * 3) >> 2);   // chance out of 256: 8..199
    g->win = hw_random8() < odds;
    if (g->win) {
      const uint8_t sy = hw_random8(NSYM);
      for (int r = 0; r < nr; r++) g->sym[r] = sy;
    } else {
      for (int r = 0; r < nr; r++) g->sym[r] = hw_random8(NSYM);
      if (hw_random8() < 100) for (int r = 1; r < nr - 1; r++) g->sym[r] = g->sym[0];   // near miss
      bool same = nr > 1;
      for (int r = 1; r < nr; r++) if (g->sym[r] != g->sym[0]) same = false;
      if (same) g->sym[nr - 1] = (g->sym[0] + 1 + hw_random8(NSYM - 1)) % NSYM;      // not a win
    }
    uint32_t prevStop = 0;
    for (int r = 0; r < nr; r++) {
      Reel &rl = g->reel[r];
      // where the result symbol sits centered in the window
      int k = 0;
      while (reelSym(r, k) != g->sym[r]) k++;
      float pf = (float)(k * s + s / 2 - L / 2);
      while (pf < 0) pf += C;
      rl.p0 = fresh ? (float)((hw_random8(NSYM) * s + s / 2 - L / 2 + (int)C * 2) % (int)C) : rl.pf;
      rl.pf = pf;
      // nominal stop time, then nudged later so the reel lands exactly on its symbol
      const uint32_t base = 1600 - SEGMENT.speed * 3;            // 1600 .. 835 ms
      const uint32_t stagger = 650 - (SEGMENT.speed * 3) / 2;    // 650 .. 268 ms
      const uint32_t nom = r == 0 ? base : prevStop + stagger;
      const float dNom = v * (nom - A / 2.0f - D / 2.0f);
      float need = fmodf(pf - rl.p0, C); if (need < 0) need += C;
      float extra = fmodf(need - dNom, C); if (extra < 0) extra += C;
      rl.stop = nom + (uint32_t)(extra / v);
      prevStop = rl.stop;
    }
    g->end = now + prevStop + 400 + (g->win ? 5600 : 1500);
  }

  const uint32_t t = now - g->start;
  const uint32_t settled = g->reel[nr - 1].stop + 400;
  const bool done = t >= settled;
  const uint32_t after = done ? t - settled : 0;
  const bool jackpot = done && g->win;
  const uint32_t frame = jackpot && after < 1400 && (after / 70) % 2 == 0 ? GOLD : RGBW32(40, 22, 0, 12);

  for (int r = 0; r < nr; r++) {
    const Reel &rl = g->reel[r];
    float p;
    if (t < A) p = rl.p0 + v * t * t / (2.0f * A);
    else if (t + D < rl.stop) p = rl.p0 + v * (A / 2.0f) + v * (t - A);
    else if (t < rl.stop) {
      const float x = (float)(t - (rl.stop - D)) / D;
      p = rl.p0 + v * (A / 2.0f) + v * (rl.stop - D - A) + v * D * (x - x * x / 2.0f);
    } else {
      const float a = (float)(t - rl.stop);                       // bounce: a little past, then back
      p = rl.pf + (a < 600 ? 0.15f * s * sinf(a * 0.022f) * expf(-a * 0.009f) : 0.0f);
    }
    const bool spinning = t < rl.stop;
    const int x0 = r * (L + 1);
    float xc = fmodf(L / 2 + p + 0.5f, C);                       // which symbol is on the payline
    if (xc < 0) xc += C;
    const int kc = (int)(xc / s);
    for (int j = 0; j < L && x0 + j < len; j++) {
      float x = fmodf(j + p + 0.5f, C);
      if (x < 0) x += C;
      const int k = (int)(x / s);
      const float in = x - k * s;
      uint32_t c = SYMBOLS[reelSym(r, k)];
      if (s >= 4 && in >= s - 1) c = BLACK;                     // gap between symbols
      else if (spinning) c = color_fade(c, 200, true);           // motion blur look
      if (done) {
        const bool center = k == kc;
        if (!center) c = color_fade(c, jackpot ? 40 : 70, true);  // the rest dims: the payline shows
        else if (!jackpot) c = color_fade(c, 190 + (sin8_t(after >> 2) >> 2), true);
      }
      SEGMENT.setPixelColor(x0 + L - 1 - j, c);             // drawn back to front: symbols roll toward the end
    }
    if (nr > 1 && r < nr - 1 && x0 + L < len) SEGMENT.setPixelColor(x0 + L, frame);
  }
  for (int i = nr * (L + 1) - (nr > 1 ? 1 : 0); i < len; i++) SEGMENT.setPixelColor(i, frame);  // leftover pixels

  // jackpot: strobe, then a gold chase over everything with sparkling coins
  if (jackpot) {
    if (after < 1400) {
      if ((after / 70) % 2 == 0) for (int i = 0; i < len; i++) SEGMENT.setPixelColor(i, color_blend(SEGMENT.getPixelColor(i), BULB_HOT, 150));
    } else {
      const uint32_t chase = (after / 60) % 3;
      const uint8_t fade = after > 5000 ? (uint8_t)min<uint32_t>(255, (after - 5000) * 255 / 600) : 0;
      for (int i = 0; i < len; i++) {
        uint32_t c = SEGMENT.getPixelColor(i);
        if ((i + 3 - chase) % 3 == 0) c = color_blend(c, GOLD, 255 - fade);   // chases toward the end
        const uint32_t h = vhash(i * 977u + (after / 45) * 7151u);
        if ((h & 0xFF) < 12) c = color_blend(c, SPARK, 255 - fade);  // coins
        SEGMENT.setPixelColor(i, c);
      }
    }
  }
}
const char _data_jackpot[] PROGMEM = "Jackpot@Pace,Jackpot odds,Reels;;;1;sx=128,ix=60,c1=0";

// ---------------------------------------------------------------- Roulette
// A roulette wheel along the strip: red and black pockets with a green zero every 37 pockets,
// turning one way while a white ball races the other way. The ball slows, drops, rattles across
// a few pockets and settles. The winning pocket flashes and every pocket of the winning color
// lights up, then the croupier spins again.
//   Spin speed, Pocket size (2-9 pixels)
//   Colors (My colors): 1 = red pockets, 2 = black pockets, 3 = zero. Defaults red, dark, green.
struct Wheel { uint32_t start; float b0, w0, vb0, tDrop, bDrop, target; uint16_t win; uint8_t phase; };

void mode_roulette() {
  const int len = SEGLEN;
  if (len < 6 || !SEGENV.allocateData(sizeof(Wheel))) { staticFallback(); return; }
  Wheel *w = reinterpret_cast<Wheel *>(SEGENV.data);
  const uint32_t now = strip.now;
  const bool mine = SEGMENT.check1;
  const uint32_t red  = mine && SEGCOLOR(0) ? SEGCOLOR(0) : RGBW32(255, 0, 0, 0);
  const uint32_t blk  = mine ? SEGCOLOR(1) : BLACK;
  const uint32_t zero = mine && SEGCOLOR(2) ? SEGCOLOR(2) : RGBW32(0, 200, 30, 0);
  const uint32_t BALL = RGBW32(200, 200, 200, 255);
  int pw = 2 + (SEGMENT.intensity >> 5);                     // pocket width 2..9
  if (pw > len / 3) pw = max(1, len / 3);
  const int np = len / pw;                                   // pockets on the wheel
  const float W = (float)np * pw;                            // wheel circumference, pixels
  const float tauB = 2200.0f, tauW = 5000.0f;                // how quickly ball and wheel slow (ms)
  const float vDrop = pw * 0.006f + 0.02f;                   // ball drops below this speed (px/ms)
  const float settleMs = 1300.0f, showMs = 2800.0f;

  if (SEGENV.call == 0 || w->vb0 <= 0) {
    w->b0 = hw_random16(len); w->w0 = 0; w->phase = 2; w->start = now - 100000; w->tDrop = 0;
  }
  float t = (float)(now - w->start);
  // ball and wheel, both slowing exponentially (wheel turns backwards, slowly)
  auto wheelAt = [&](float tt) { return w->w0 - w->vb0 * 0.18f * tauW * (1.0f - expf(-tt / tauW)); };
  if (w->phase == 2 && t > w->tDrop + settleMs + showMs) {
    // new spin: start from where the ball and wheel are now
    const float wNow = wheelAt(t);
    float bNow = w->phase == 2 && w->vb0 > 0 ? w->target + wNow : w->b0;
    w->w0 = fmodf(wNow, W); if (w->w0 < 0) w->w0 += W;
    w->b0 = fmodf(bNow, (float)len); if (w->b0 < 0) w->b0 += len;
    w->vb0 = max(0.12f, len * 0.0011f) * (0.55f + SEGMENT.speed / 300.0f);   // px per ms
    w->start = now; t = 0;
    w->tDrop = tauB * logf(w->vb0 / vDrop);
    if (w->tDrop < 1200) w->tDrop = 1200;
    w->phase = 0;
  }
  const float wNow = wheelAt(t);
  float ball;            // screen position of the ball
  float ballV = 0;       // ball speed, for its tail
  if (w->phase == 0 && t < w->tDrop) {
    ball = w->b0 + w->vb0 * tauB * (1.0f - expf(-t / tauB));
    ballV = w->vb0 * expf(-t / tauB);
  } else {
    if (w->phase == 0) {
      // the drop: note where the ball is on the wheel and pick its pocket a few hops ahead
      const float bScreen = w->b0 + w->vb0 * tauB * (1.0f - expf(-w->tDrop / tauB));
      float bw = fmodf(bScreen - wheelAt(w->tDrop), W); if (bw < 0) bw += W;
      w->bDrop = bw;
      int k = ((int)(bw / pw) + 1 + hw_random8(3)) % np;
      w->win = k;
      w->target = k * pw + pw / 2.0f;
      if (w->target < bw) w->target += W;    // keep moving forward
      w->phase = 1;
    }
    const float a = t - w->tDrop;
    float bw;
    if (a < settleMs) {
      // rattle: hops over the pockets toward the target, bouncing back and forth, dying out
      const float x = a / settleMs;
      const float ease = 1.0f - (1.0f - x) * (1.0f - x);
      const float bounce = (w->target - w->bDrop) * 0.35f * cosf(a * 0.02f) * expf(-a * 0.004f);
      bw = w->bDrop + (w->target - w->bDrop) * ease + bounce * (1.0f - x);
    } else {
      bw = w->target;
      w->phase = 2;
    }
    ball = bw + wNow;
  }

  // draw the wheel
  const bool show = w->phase == 2;
  const uint8_t winKind = (w->win % 37 == 0) ? 0 : ((w->win & 1) ? 1 : 2);   // 0 zero, 1 red, 2 black
  const float showT = t - w->tDrop - settleMs;
  for (int i = 0; i < len; i++) {
    float xw = fmodf(i + 0.5f - wNow, W); if (xw < 0) xw += W;
    const int k = (int)(xw / pw) % np;
    const uint8_t kind = (k % 37 == 0) ? 0 : ((k & 1) ? 1 : 2);
    uint32_t c = kind == 0 ? zero : (kind == 1 ? red : blk);
    uint8_t bri = 150;
    if (show) {
      bri = kind == winKind ? 255 : 70;
      if (k == w->win) bri = ((uint32_t)showT / 220) % 2 ? 255 : 90;   // winning pocket flashes
      if (k == w->win && kind == 2 && !blk) c = RGBW32(60, 60, 60, 60); // a black win still shows
    }
    if (pw >= 4 && (int)xw % pw == pw - 1) bri >>= 2;                   // pocket dividers
    SEGMENT.setPixelColor(i, color_fade(c, bri, true));
  }
  // the ball, with a short tail while it's racing
  if (ballV > 0.05f) {
    const int tail = min(6, (int)(ballV * 25));
    for (int k = tail; k >= 1; k--) softDot(ball - k, color_fade(BALL, 200 - k * 30, true), len);
  }
  softDot(ball, BALL, len);
}
const char _data_roulette[] PROGMEM =
  "Roulette@Spin speed,Pocket size,,,,My colors;Red,Black,Zero;;1;sx=128,ix=64,o1=0";

// ---------------------------------------------------------------- Dancing Fountains
// Rows of water jets doing a fountain show: waves rolling down the line, everything bursting
// together, call-and-response, rapid "shooters" one after another, and a big finale. Each jet
// leaps and falls with gravity and leaves a mist behind; the crowns sparkle. With "Dance to
// music" ticked (needs the mic / AudioReactive) every jet follows its own slice of the music.
//   Tempo, Jets (fewer/more), Jet height
//   Colors (My colors): 1 = water (default icy white), 2 = underwater lights (default blue)
struct Jet { float h, v; };
constexpr uint8_t MAX_JETS = 48;

void mode_dancing_fountains() {
  const int len = SEGLEN;
  if (len < 3 || !SEGENV.allocateData(sizeof(Jet) * MAX_JETS + sizeof(uint32_t))) { staticFallback(); return; }
  Jet *jets = reinterpret_cast<Jet *>(SEGENV.data);
  uint32_t *last = reinterpret_cast<uint32_t *>(SEGENV.data + sizeof(Jet) * MAX_JETS);
  const uint32_t now = strip.now;
  static const uint32_t DEF[2] = { RGBW32(120, 190, 255, 200), RGBW32(0, 40, 255, 0) };
  uint32_t cols[3];
  const uint8_t nc = vegasColors(SEGMENT.check2, cols, DEF, 2);
  const uint32_t water = cols[0], light = nc > 1 ? cols[1] : DEF[1];

  const int spacing = 10 + (255 - SEGMENT.intensity) / 6;   // pixels per jet: 10..52
  const int nj = constrain(len / spacing, 1, (int)MAX_JETS);
  const float span = (float)len / nj;
  const float hMax = span * (0.2f + 0.3f * SEGMENT.custom1 / 255.0f);   // reach either side
  const uint32_t beatMs = 120000UL / (120 + SEGMENT.speed);  // 60..160 beats a minute
  const float bpm = 60000.0f / beatMs;
  const float g = 4.0f * (bpm / 90.0f) * (bpm / 90.0f) / 1e6f;   // gravity, height per ms^2
  const auto shoot = [&](Jet &j, float h) { if (j.h < h) j.v = sqrtf(2.0f * g * (h - j.h)) ; };

  if (SEGENV.call == 0) { SEGMENT.fill(BLACK); *last = now; }
  uint32_t dtMs = now - *last;
  *last = now;
  if (dtMs > 100) dtMs = 100;
  const float dt = (float)dtMs;

  // ---- music
  um_data_t *um = nullptr;
  const uint8_t *fft = nullptr;
  if (SEGMENT.check1 && UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) && um && um->u_size >= 3)
    fft = (const uint8_t *)um->u_data[2];

  // ---- choreography: 5 routines of 8 beats each (integer clock: stays smooth after weeks)
  const uint32_t beat = now / beatMs;
  const float beats = (beat % 64) + (float)(now % beatMs) / beatMs;   // wraps every 64 beats
  const uint8_t routine = (beat / 8) % 5;
  const bool onBeat = beat != (now - dtMs) / beatMs;
  const uint32_t halfMs = beatMs / 2;
  const uint32_t half = now / halfMs;
  const bool onHalf = half != (now - dtMs) / halfMs;

  for (int j = 0; j < nj; j++) {
    Jet &jt = jets[j];
    if (fft) {
      const uint8_t band = 1 + (j * 13) / max(1, nj - 1);     // bands 1..14
      const float lv = fft[min((int)band, 15)] / 255.0f;
      const float target = (lv - 0.06f) * 1.4f;                // quiet bands stay down
      if (target > jt.h + 0.05f) shoot(jt, min(1.0f, target));
    } else switch (routine) {
      case 0: {   // wave rolling down the line
        const float ph = beats * 0.5f - j * 0.15f;           // each jet a bit behind the last
        const float target = 0.15f + 0.8f * (0.5f + 0.5f * sinf(ph * 6.2832f));
        jt.h += (target - jt.h) * min(1.0f, dt / 120.0f); jt.v = 0;
        break;
      }
      case 1:     // everything together on the beat
        if (onBeat) shoot(jt, (beat & 3) == 3 ? 1.0f : 0.7f);
        break;
      case 2:     // call and response
        if (onBeat && (int)((beat + j) & 1)) shoot(jt, 0.85f);
        break;
      case 3:     // shooters running down the line, two per beat
        if (onHalf && (int)(half % nj) == j) shoot(jt, 1.0f);
        if (onHalf && nj > 6 && (int)((half + nj / 2) % nj) == j) shoot(jt, 1.0f);
        break;
      default: {  // finale: rising ripples, then everything to the top
        const uint32_t b = beat % 8;
        if (b < 6) { if (onBeat) shoot(jt, 0.3f + b * 0.11f + ((j & 1) ? 0.05f : 0)); }
        else if (jt.v <= 0 && jt.h < 0.9f) shoot(jt, 1.0f);
        break;
      }
    }
    // gravity
    if (!(routine == 0 && !fft)) {
      jt.h += jt.v * dt - 0.5f * g * dt * dt;
      jt.v -= g * dt;
      if (jt.h <= 0) { jt.h = 0; jt.v = 0; }
      if (jt.h > 1.05f) { jt.h = 1.05f; if (jt.v > 0) jt.v = 0; }
    }
  }

  // ---- draw: mist fades, jets drawn on top
  SEGMENT.fadeToBlackBy(28);
  for (int j = 0; j < nj; j++) {
    const Jet &jt = jets[j];
    if (jt.h <= 0.01f) continue;
    const float c = (j + 0.5f) * span;
    const float r = jt.h * hMax + 0.6f;
    const int lo = max(0, (int)(c - r - 1)), hi = min(len - 1, (int)(c + r + 1));
    for (int i = lo; i <= hi; i++) {
      const float d = fabsf(i + 0.5f - c);
      if (d > r + 0.5f) continue;
      const float x = d / r;                                  // 0 at the jet, 1 at the spray edge
      // lit from below at the base, white water higher up, brightest foam at the crown
      uint32_t px = color_blend(light, water, (uint8_t)min(255.0f, 90 + x * 200));
      uint8_t bri = (uint8_t)(110 + 145 * x);
      if (d > r - 0.5f) bri = (uint8_t)(bri * max(0.0f, (r + 0.5f - d)));   // soft edge
      if (jt.v < 0) bri = (uint8_t)(bri * (0.65f + 0.35f * jt.h));           // falling water thins
      px = color_fade(px, bri, true);
      if (x > 0.7f && (vhash(i * 7919u + (now / 40) * 104729u) & 0xFF) < 70) px = color_blend(px, SPARK, 180);
      // keep the brighter of mist and jet
      const uint32_t old = SEGMENT.getPixelColor(i);
      const auto lum = [](uint32_t q) { return ((q >> 16) & 0xFF) + ((q >> 8) & 0xFF) + (q & 0xFF) + ((q >> 24) & 0xFF) * 2; };
      SEGMENT.setPixelColor(i, lum(px) >= lum(old) ? px : old);
    }
  }
}
const char _data_dancing_fountains[] PROGMEM =
  "Dancing Fountains@Tempo,Jets,Jet height,,,Dance to music,My colors;Water,Lights;;1f;"
  "sx=110,ix=140,c1=200,o1=0,o2=0";

// ---------------------------------------------------------------- Showgirl Sparkle
// Feathers and sequins: soft plumes of pink, gold and violet sway along the strip while
// sequins catch the light and glint. Every few seconds a "spotlight" sweeps across and the
// sequins it passes flash in a wave.
//   Shimmer (speed), Sequins (how many), Feather size
//   Colors (My colors): the plume colors. Default hot pink, gold, violet.
void mode_showgirl_sparkle() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(len + sizeof(uint32_t))) { staticFallback(); return; }
  uint32_t *last = reinterpret_cast<uint32_t *>(SEGENV.data);
  uint8_t *spark = SEGENV.data + sizeof(uint32_t);
  const uint32_t now = strip.now;
  static const uint32_t DEF[3] = { RGBW32(255, 0, 110, 0), RGBW32(255, 110, 0, 90), RGBW32(130, 0, 255, 0) };
  uint32_t cols[3];
  const uint8_t n = vegasColors(SEGMENT.check1, cols, DEF, 3);

  if (SEGENV.call == 0) *last = now;
  const uint32_t t = (uint32_t)now * (SEGMENT.speed + 16) >> 8;
  const uint16_t cell = 8 + (SEGMENT.custom1 >> 2);          // pixels per plume: 8..71
  const uint16_t step = 256 / cell;                          // noise units per pixel

  // sweeping spotlight: crosses the strip, then rests
  const uint32_t sweepMs = 2600 - SEGMENT.speed * 6;          // 2600 .. 1070 ms to cross
  const uint32_t cyc = now % (sweepMs * 3);
  const float spot = cyc < sweepMs ? (float)cyc / sweepMs * (len + 20) - 10 : -1000.0f;

  // sequins: decay and spawn ~60 times a second
  if (now - *last >= 16) {
    const uint8_t decay = 10 + (SEGMENT.speed >> 3);
    *last = now;
    for (int i = 0; i < len; i++) {
      spark[i] = spark[i] > decay ? spark[i] - decay : 0;
      int chance = 1 + (SEGMENT.intensity >> 4);                // per 1024
      if (fabsf(i - spot) < 4) chance += 120;
      if ((int)(vhash(i * 2246822519u + now * 3266489917u) & 1023) < chance) spark[i] = 255;
    }
  }

  for (int i = 0; i < len; i++) {
    // plume color: noise picks a spot along the color list, blending smoothly
    const uint8_t nz = perlin8(i * step - (t >> 2), t >> 3);
    const uint16_t pos = ((uint32_t)nz * n * 2 + (t >> 5)) % (n * 256);   // drifts through all the colors
    const uint8_t a = (pos >> 8) % n, f = pos & 0xFF;
    uint32_t c = n == 1 ? cols[0] : color_blend(cols[a], cols[(a + 1) % n], f);
    // feathers ripple in brightness
    const uint8_t br = 110 + (perlin8(i * step * 3 + 9000, (t >> 1) + 4000) >> 1);
    c = color_fade(c, br, true);
    if (spark[i]) c = color_blend(c, SPARK, spark[i]);
    SEGMENT.setPixelColor(i, c);
  }
}
const char _data_showgirl_sparkle[] PROGMEM =
  "Showgirl Sparkle@Shimmer,Sequins,Feather size,,,My colors;!,!,!;;1;sx=96,ix=128,c1=100,o1=0";

// ================================================================ Candy Land (237)
// Candy colors with white frosting from the W chip, in three looks picked with the Look slider:
//   0-63    Candy Path     - game-board spaces: 3 candy pixels and a frosting pixel, hopping one
//                            space at a time along the strip, with sugar sparkles on top
//   64-127  Gumdrop Pop    - soft pastel frosting drifting along while gumdrops pop up in random
//                            spots, flash white in the middle and melt away
//   128-191 Lollipop Swirl - stripes of 2 candy pixels and 1 frosting pixel drifting along and
//                            swaying back and forth
//   192-255 Tour           - the three in turn, 15 s each, with a short crossfade (the default)
//   Speed, Sprinkles (sugar glints on the candy, candy sprinkles on the frosting), Look
//   Checkbox "My colors": the candy colors come from slots 1-3 instead of the candy set
// Everything travels toward the end of the segment. Speed changes are smooth: the effect keeps
// its own clock ("candy time") that runs faster or slower with Speed.
struct Rgbw { float r, g, b, w; };
inline Rgbw unpackC(uint32_t c) { return { float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF), float(c >> 24) }; }
inline Rgbw mixC(const Rgbw &a, const Rgbw &b, float k) {
  return { a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.w + (b.w - a.w) * k };
}
inline Rgbw scaleC(const Rgbw &a, float k) { return { a.r * k, a.g * k, a.b * k, a.w * k }; }
inline uint8_t clamp8(float v) { return v <= 0.0f ? 0 : (v >= 255.0f ? 255 : (uint8_t)(v + 0.5f)); }
inline uint32_t packC(const Rgbw &c) { return RGBW32(clamp8(c.r), clamp8(c.g), clamp8(c.b), clamp8(c.w)); }
// stateless random number 0..1 from three integers
inline float h01(uint32_t a, uint32_t b, uint32_t c) {
  return (vhash(a * 2654435761u + vhash(b * 40503u + c * 0x9E3779B1u + 12345u)) >> 8) / 16777216.0f;
}
inline float easeC(float x) { return x * x * (3.0f - 2.0f * x); }

const uint32_t CANDY[8] = {
  RGBW32(255, 20, 60, 0),    // cherry
  RGBW32(255, 100, 0, 0),    // tangerine
  RGBW32(255, 190, 0, 0),    // lemon
  RGBW32(80, 220, 20, 0),    // lime
  RGBW32(20, 80, 255, 0),    // blueberry
  RGBW32(150, 30, 255, 0),   // grape
  RGBW32(255, 50, 150, 0),   // bubblegum
  RGBW32(40, 255, 150, 0),   // mint
};
const uint32_t PASTEL[4] = { RGBW32(255, 90, 170, 40), RGBW32(90, 255, 190, 40), RGBW32(255, 215, 90, 40), RGBW32(170, 120, 255, 40) };
const uint8_t SWIRL_ORDER[7] = { 0, 2, 4, 6, 3, 1, 5 };   // cherry, lemon, blueberry, bubblegum, lime, tangerine, grape
constexpr uint8_t MAX_GUMDROPS = 40;

struct CandyState { uint32_t last, ct; float rem; };
struct Gumdrop { float cen, env, tau; Rgbw col; };

struct CandyCtx {
  int len; uint32_t ct, now, seed; float den;
  Rgbw cand[8]; uint8_t nc; bool mine;
  Rgbw frost;
  Gumdrop drops[MAX_GUMDROPS]; uint8_t nd; bool dropsReady;
  // the colors each look cycles through: the user's slots, or the candy set
  Rgbw pathCol(uint32_t k) const { return mine ? cand[k % nc] : cand[k % 6]; }
  uint8_t pathN() const { return mine ? nc : 6; }
  Rgbw swirlCol(uint32_t k) const { return mine ? cand[k % nc] : cand[SWIRL_ORDER[k % 7]]; }
  uint8_t swirlN() const { return mine ? nc : 7; }
  Rgbw gumCol(float u) const { return cand[min((int)(u * nc), nc - 1)]; }   // all 8 candy colors (or the user's)
};

// sugar on top: white glints on the candy, candy-colored sprinkles on the frosting.
// Runs on real time so it twinkles the same at any Speed.
Rgbw sprinkle(const CandyCtx &x, Rgbw c, int i, float den, bool frosting) {
  const uint32_t L = 380;
  const uint32_t u = x.now + (uint32_t)(h01(i, x.seed, 1) * L);
  const uint32_t slot = u / L;
  if (h01(i, slot, x.seed + 2) >= den * 0.13f) return c;
  const float f = (float)(u % L) / L, k = (1.0f - f) * (1.0f - f);
  const Rgbw s = frosting ? x.gumCol(h01(i, slot, x.seed + 3)) : Rgbw{ 255, 255, 255, 255 };
  return mixC(c, s, k);
}

Rgbw candyPath(const CandyCtx &x, int i) {
  // one space (4 pixels) per beat of 1150 candy-ms, the hop eased over the first third of the beat
  const uint32_t beat = x.ct / 1150;
  float f = (float)(x.ct % 1150) / (1150.0f * 0.32f);
  if (f > 1.0f) f = 1.0f;
  const uint32_t np = x.pathN(), period = 4 * np;
  const uint32_t off = (beat % np) * 4 + (uint32_t)(easeC(f) * 4.0f);   // 0 .. period
  const uint32_t p = (uint32_t)i + 2 * period - off;                     // i - off, kept positive
  const bool frost = p % 4 == 3;
  return sprinkle(x, frost ? x.frost : x.pathCol((p / 4) % np), i, x.den, frost);
}

void gumdropSetup(CandyCtx &x) {
  int nd = (int)lroundf(x.len / 7.0f * (0.45f + x.den));
  nd = constrain(nd, 2, (int)MAX_GUMDROPS);
  x.nd = nd;
  for (int j = 0; j < nd; j++) {
    Gumdrop &d = x.drops[j];
    const uint32_t P = 2300 + (uint32_t)(h01(j, x.seed, 4) * 1700);       // candy-ms between pops
    const uint32_t tt = x.ct + (uint32_t)(h01(j, x.seed, 5) * P);
    const uint32_t cyc = tt / P;
    d.tau = (float)(tt % P) / 1000.0f;                                     // seconds since this pop
    d.cen = h01(j, cyc, x.seed + 6) * (x.len - 1);
    d.col = x.gumCol(h01(j, cyc, x.seed + 7));
    d.env = d.tau < 0.14f ? d.tau / 0.14f : expf(-(d.tau - 0.14f) * 2.3f);
  }
  x.dropsReady = true;
}

Rgbw gumdrop(CandyCtx &x, int i) {
  if (!x.dropsReady) gumdropSetup(x);
  // pastel frosting drifting along (one trip through the colors every ~22 s at speed 1)
  float u = i / 17.0f - (x.ct % 200000) * 0.000045f + (x.seed % 97) * 0.21f;
  u -= floorf(u);
  Rgbw c;
  if (x.mine) {
    const float v = u * x.nc; const int a = (int)v % x.nc;
    c = scaleC(mixC(x.cand[a], x.cand[(a + 1) % x.nc], v - (int)v), 0.3f);
    c.w += 12.0f;
  } else {
    const float v = u * 4; const int a = (int)v % 4;
    c = scaleC(mixC(unpackC(PASTEL[a]), unpackC(PASTEL[(a + 1) % 4]), v - (int)v), 0.3f);
  }
  for (int j = 0; j < x.nd; j++) {
    const Gumdrop &d = x.drops[j];
    if (d.env <= 0.01f) continue;
    const float dist = fabsf(i - d.cen), w = 1.0f - dist / 1.75f;
    if (w <= 0.0f) continue;
    const float k = w * d.env;
    c.r += d.col.r * k; c.g += d.col.g * k; c.b += d.col.b * k; c.w += d.col.w * k;
    if (dist < 0.6f && d.tau < 0.35f) c.w += 140.0f * (1.0f - d.tau / 0.35f);   // white flash in the middle
  }
  return c;
}

Rgbw lollipop(const CandyCtx &x, int i) {
  // 2.4 px per candy-second along the strip (wraps after 126 px: a whole number of stripe
  // cycles for 1, 2, 3 or 7 colors) plus a sway of +-7 px every 8 candy-seconds
  const float phase = (x.ct % 52500) * 0.0024f + sinf((x.ct % 8000) * (6.2831853f / 8000.0f)) * 7.0f;
  const float p = i - phase + 504.0f;                                      // kept positive
  const uint32_t q = (uint32_t)p;
  const float fr = p - q;
  const uint32_t ns = x.swirlN();
  auto at = [&](uint32_t s) { return s % 3 == 2 ? x.frost : x.swirlCol((s / 3) % ns); };
  const Rgbw c = mixC(at(q), at(q + 1), fr);                               // smooth as the stripes slide
  return sprinkle(x, c, i, x.den * 0.6f, (fr < 0.5f ? q : q + 1) % 3 == 2);
}

Rgbw candyLook(CandyCtx &x, uint8_t look, int i) {
  switch (look) {
    case 0:  return candyPath(x, i);
    case 1:  return gumdrop(x, i);
    default: return lollipop(x, i);
  }
}

void mode_candy_land() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(CandyState))) { staticFallback(); return; }
  CandyState *st = reinterpret_cast<CandyState *>(SEGENV.data);
  const uint32_t now = strip.now;
  if (SEGENV.call == 0) { st->last = now; st->ct = 4000; st->rem = 0.0f; }   // start with gumdrops already popping
  uint32_t dt = now - st->last;
  st->last = now;
  if (dt > 200) dt = 200;
  // candy time: Speed 128 = real time, 0 = a quarter, 255 = 1.75x
  const float adv = dt * (0.25f + SEGMENT.speed / 170.0f) + st->rem;
  st->ct += (uint32_t)adv;
  st->rem = adv - (uint32_t)adv;

  static CandyCtx x;              // big (gumdrops): static, not on the stack
  x.len = len; x.ct = st->ct; x.now = now; x.den = SEGMENT.intensity / 255.0f;
  x.seed = 1 + SEGMENT.start * 7919u;   // each segment sparkles differently
  x.frost = unpackC(WARM);
  x.nc = 0;
  if (SEGMENT.check1) for (uint8_t s = 0; s < 3; s++) if (SEGCOLOR(s)) x.cand[x.nc++] = unpackC(SEGCOLOR(s));
  x.mine = x.nc > 0;
  if (!x.mine) { for (uint8_t k = 0; k < 8; k++) x.cand[k] = unpackC(CANDY[k]); x.nc = 8; }
  x.dropsReady = false;

  const uint8_t look = SEGMENT.custom1 >> 6;     // 0 path, 1 gumdrop, 2 swirl, 3 tour
  uint8_t a = look, b = 255;
  float k = 1.0f;
  if (look == 3) {                                // tour: 15 s each, 1.4 s crossfade from the last one
    const uint32_t per = 15000, f = now % per;
    a = (now / per) % 3;
    if (f < 1400) { b = (a + 2) % 3; k = f / 1400.0f; }
  }
  for (int i = 0; i < len; i++) {
    Rgbw c = candyLook(x, a, i);
    if (b != 255) c = mixC(candyLook(x, b, i), c, k);
    SEGMENT.setPixelColor(i, packC(c));
  }
}
const char _data_candy_land[] PROGMEM =
  "Candy Land@Speed,Sprinkles,Look,,,My colors;!,!,!;;1;sx=128,ix=140,c1=255,o1=0";

}  // namespace

// ---------------------------------------------------------------- usermod

class CoastalFxUsermod : public Usermod {
  uint8_t firstId = 255, lastId = 255;

  void add(void (*fn)(), const char *data) {
    // Ask for the next id at the end of the list so ids stay predictable (220, 221, ...)
    const uint8_t id = strip.addEffect(strip.getModeCount(), fn, data);
    if (id == 255) return;
    if (firstId == 255) firstId = id;
    lastId = id;
  }

 public:
  void setup() override {
    add(&mode_holiday_bulbs,    _data_holiday_bulbs);
    add(&mode_holiday_twinkle,  _data_holiday_twinkle);
    add(&mode_candy_cane,       _data_candy_cane);
    add(&mode_retro_c9,         _data_retro_c9);
    add(&mode_fireworks_burst,  _data_fireworks_burst);
    add(&mode_ocean_swell,      _data_ocean_swell);
    add(&mode_lightning_storm,  _data_lightning_storm);
    add(&mode_sunrise,          _data_sunrise);
    add(&mode_boom_fireworks,   _data_boom_fireworks);   // new effects go at the end so ids don't shift
    add(&mode_pool_shimmer,     _data_pool_shimmer);
    add(&mode_northern_lights,  _data_northern_lights);
    // Las Vegas (231-236)
    add(&mode_vegas_marquee,     _data_vegas_marquee);
    add(&mode_neon_sign,         _data_neon_sign);
    add(&mode_jackpot,           _data_jackpot);
    add(&mode_roulette,          _data_roulette);
    add(&mode_dancing_fountains, _data_dancing_fountains);
    add(&mode_showgirl_sparkle,  _data_showgirl_sparkle);
    add(&mode_candy_land,        _data_candy_land);       // 237
  }

  void loop() override {}

  // Info page: shows which effect ids were assigned, to check presets against
  void addToJsonInfo(JsonObject &root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray arr = user.createNestedArray(F("Coastal FX"));
    if (firstId == 255) { arr.add(F("not loaded")); return; }
    char buf[24];
    snprintf_P(buf, sizeof(buf), PSTR("ids %u-%u"), firstId, lastId);
    arr.add(buf);
  }
};

static CoastalFxUsermod coastal_fx;
REGISTER_USERMOD(coastal_fx);
