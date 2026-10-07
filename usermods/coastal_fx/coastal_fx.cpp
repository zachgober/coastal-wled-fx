/*
 * Coastal FX - custom WLED effects for the house
 * Target: WLED 16.0.1, classic ESP32, SK6812 RGBW pixels
 *
 * Effects (they show up in the WLED effect list, Home Assistant and the Home app):
 *   Holiday Bulbs    - alternating bulbs in colors 1/2/3 that chase or swap
 *   Holiday Twinkle  - alternating bulbs with warm-white sparkles
 *   Candy Cane       - moving stripes in colors 1/2
 *   Retro C9         - classic multi-color C9 bulbs with a gentle flicker
 *   Fireworks Burst  - bursts that expand and fade in colors 1/2/3
 *   Ocean Swell      - rolling blue/teal waves with white-chip whitecaps
 *   Lightning Storm  - dim storm clouds with real-looking multi-flash strikes
 *   Sunrise          - slow dawn (or dusk) fade over 1-64 minutes
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
  const uint32_t whole = phase16 >> 8;
  const uint8_t frac = SEGMENT.check1 ? (phase16 & 0xFF) : 0;

  for (int i = 0; i < len; i++) {
    const uint32_t bulb = i / size;
    const uint32_t a = cols[(bulb + whole) % n];
    if (frac) {
      const uint32_t b = cols[(bulb + whole + 1) % n];
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
  uint32_t off16 = 0;                                    // 8.8 fixed point pixel offset
  if (SEGMENT.speed) off16 = (uint32_t)(((uint64_t)strip.now * SEGMENT.speed) >> 7);
  for (int i = 0; i < len; i++) {
    const uint32_t p16 = ((uint32_t)i << 8) + off16;
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
// Two waves rolling against each other, blended between colors 1 and 2, with
// whitecaps from the W chip on the crests. Speed = wave speed, Intensity = whitecaps.
void mode_ocean_swell() {
  const int len = SEGLEN;
  if (len < 1) { staticFallback(); return; }
  uint32_t cols[3];
  slotColors(cols, RGBW32(0, 30, 160, 0), RGBW32(0, 150, 140, 0));  // deep blue, teal
  const uint32_t t = ((uint32_t)strip.now * (SEGMENT.speed + 8)) >> 9;
  const uint8_t foamStart = 255 - (SEGMENT.intensity >> 1);  // higher intensity = more foam
  for (int i = 0; i < len; i++) {
    const uint8_t w1 = sin8_t(i * 6 + t);
    const uint8_t w2 = sin8_t(i * 11 - (t >> 1) + 90);
    const uint8_t mix = (w1 + w2) >> 1;
    uint32_t c = color_blend(cols[0], cols[1], mix);
    if (SEGMENT.intensity && mix > foamStart) {
      const uint8_t foam = (uint32_t)(mix - foamStart) * 255 / (256 - foamStart);
      c = color_blend(c, WARM, sc8(foam, 200));
    }
    // a slow brightness swell over the whole strip
    c = color_fade(c, 170 + (sin8_t(i * 2 + (t >> 2)) / 3), true);
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
// drift against each other; where a layer's value crosses its midpoint a sharp bright
// line forms, and where both layers' lines meet they flare brighter.
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
    // two layers, drifting opposite ways and morphing over time (2nd noise axis)
    const uint8_t a = ridge(perlin8(x + (t >> 1),          t >> 2));
    const uint8_t b = ridge(perlin8(x * 3 / 2 - (t >> 2) + 9000, (t >> 3) + 31000));
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
