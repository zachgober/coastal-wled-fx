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
 *   UCF Knights (238)- black and gold game day that moves to sound (mic): Charge On, Bounce House, Crowd Roar
 *   Autumn Harvest (239) - fall colors: Falling Leaves, Turning Leaves, Harvest Bulbs
 *   Star Wars (240)  - Lightsaber Duel, Blaster Fight, Hyperspace, Twin Suns
 *   Window matrix (2D, for the LED matrix in the office window):
 *   Window Clock (241)   - big clock with an info line (weather, pool) from the segment name
 *   Holiday Window (242) - animated scenes: Halloween, Thanksgiving, Christmas, Fireworks, Valentine's,
 *                          St. Patrick's, Coastal sunset; Auto picks the scene by the date
 *
 * Strips in step: every strip (segment) running the same effect at the same Speed keeps the
 * same timing, however far apart they were switched on: the clocks, tours, routines, game rounds
 * and lightning strikes are shared (see "strips in step" below). Each strip still draws its own
 * stars, leaves, duels and positions.
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

// ---------------------------------------------------------------- strips in step
// WLED runs each segment (strip) on its own, so an effect's own clock, the start of a routine
// (a look, a show, a game round) and its pauses used to count from when that strip got the
// effect. Two strips set a few seconds apart (one group, then the other; or one after the other
// from Home Assistant) then stayed that far apart for good: the Hyperspace jump, the marquee
// chase, the reels... happened at different moments on each.
// Now every strip running the same effect at the same Speed shares one timeline:
//   fxClock()  the effect's own Speed-scaled clock
//   fxMark()   when a routine started; a strip that starts it while another strip is running it
//              picks up the same start, so it joins mid-way instead of starting its own
//   fxRound()  rounds that start together (a spin, a game, a lightning strike)
// A strip on its own carries on from its own time, so nothing jumps when it's the only one.
// What each strip draws can still differ (its own stars, leaves, duel, positions); the timing
// is what's shared.
enum : uint8_t {   // effect numbers for the shared timeline (not the WLED ids)
  SX_STORM, SX_SUNRISE, SX_MARQUEE, SX_NEON, SX_JACKPOT, SX_ROULETTE, SX_CANDY, SX_AUTUMN, SX_STARWARS
};
enum : uint8_t { SK_CLOCK = 250, SK_ROUND = 251 };    // keys; a routine's key is its look number

struct FxSync { uint32_t at, seen, lastNow; float rem; uint8_t fx, key, speed, used; };
static FxSync fxSyncs[16];
constexpr uint32_t SYNC_LIVE_MS = 600;                // a strip that ran in the last 0.6 s is still running

inline bool fxLive(const FxSync &s) { return s.used && strip.now - s.seen <= SYNC_LIVE_MS; }

// the slot for (effect, key, speed); `fresh` says it was just made (or taken over)
FxSync *fxSlot(uint8_t fx, uint8_t key, uint8_t speed, bool &fresh) {
  fresh = false;
  FxSync *spare = nullptr;
  for (auto &s : fxSyncs) {
    if (s.used && s.fx == fx && s.key == key && s.speed == speed) return &s;
    if (!s.used) { if (!spare || spare->used) spare = &s; }
    else if (strip.now - s.seen > 5000 && (!spare || (spare->used && s.seen < spare->seen))) spare = &s;
  }
  if (!spare) return nullptr;                          // all in use: the strip keeps its own time
  spare->used = 1; spare->fx = fx; spare->key = key; spare->speed = speed;
  spare->rem = 0; spare->lastNow = strip.now; spare->seen = strip.now;
  fresh = true;
  return spare;
}

// The effect's clock (ms of effect time), shared by every strip running it at this Speed.
// rate = effect ms per real ms; `mine` is this strip's own clock, used when no other strip is running.
uint32_t fxClock(uint8_t fx, uint8_t speed, float rate, uint32_t mine) {
  bool fresh;
  FxSync *s = fxSlot(fx, SK_CLOCK, speed, fresh);
  if (!s) return mine;
  const uint32_t now = strip.now;
  if (fresh || !fxLive(*s)) { s->at = mine; s->rem = 0; s->lastNow = now; }
  else if (now != s->lastNow) {                        // advance once a frame, whichever strip comes first
    uint32_t d = now - s->lastNow;
    if (d > 200) d = 200;                              // paused: don't catch up
    const float adv = d * rate + s->rem;
    s->at += (uint32_t)adv; s->rem = adv - (uint32_t)adv; s->lastNow = now;
  }
  s->seen = now;
  return s->at;
}

// When routine `key` started, on the caller's timeline. `mine` is this strip's own value (or a new
// start when it's just starting the routine); while another strip is running the routine, its
// start wins.
uint32_t fxMark(uint8_t fx, uint8_t key, uint8_t speed, uint32_t mine) {
  bool fresh;
  FxSync *s = fxSlot(fx, key, speed, fresh);
  if (!s) return mine;
  if (fresh || !fxLive(*s)) s->at = mine;
  s->seen = strip.now;
  return s->at;
}

// Rounds that start together. `over` = this strip is ready for its next round. Returns true when
// the strip should start one now, with `start` set to when it began: a round another strip started
// since this strip's last one (it joins it, if that was at most `late` ms ago), else a new one
// starting now (the others will join).
bool fxRound(uint8_t fx, uint8_t speed, uint32_t &start, bool over, uint32_t late) {
  bool fresh;
  FxSync *s = fxSlot(fx, SK_ROUND, speed, fresh);
  const uint32_t now = strip.now;
  if (!s) { if (over) start = now; return over; }
  const bool newer = !fresh && fxLive(*s) && (int32_t)(s->at - start) > 0 && now - s->at <= late;
  s->seen = now;
  if (!over) return false;
  if (newer) start = s->at;
  else { start = now; s->at = now; }
  return true;
}

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
// The strikes are shared by every strip in a storm at the same Strike rate, so a flash lights
// them all at once (like the whole sky), each on its own section. A strike's flashes and the pause
// after it come from when it began, so every strip works out the same ones.
struct Storm { uint32_t at, posFor; uint16_t start, width; };

inline uint32_t stormHash(uint32_t x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16; return x; }
// is the strike that began at `at` lit `d` ms in? (2-4 flashes, each 30-99 ms, 50-199 ms apart)
bool stormLit(uint32_t at, uint32_t d, uint32_t &length) {
  const uint8_t n = 2 + stormHash(at) % 3;
  uint32_t x = 0;
  bool lit = false;
  for (uint8_t k = 0; k < n; k++) {
    const uint32_t on = 30 + stormHash(at + 977u * (k + 1)) % 70;
    if (d >= x && d < x + on) lit = true;
    x += on;
    if (k + 1 < n) x += 50 + stormHash(at + 1931u * (k + 1)) % 150;
  }
  length = x;
  return lit;
}

void mode_lightning_storm() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(Storm))) { staticFallback(); return; }
  Storm *s = reinterpret_cast<Storm *>(SEGENV.data);
  uint32_t cols[3];
  slotColors(cols, RGBW32(10, 20, 70, 0), WARM);
  const uint32_t cloud = cols[0];
  const uint32_t now = strip.now;

  // background clouds
  for (int i = 0; i < len; i++) {
    uint32_t c = BLACK;
    if (SEGMENT.check1) {
      const uint8_t nz = perlin8(i * 24, now >> 4);
      c = color_fade(cloud, 40 + sc8(nz, 120), true);
    }
    SEGMENT.setPixelColor(i, c);
  }

  // The strikes follow one chain: each starts when the last one's flashes and pause are over (the
  // pause is longer at a lower Strike rate). The first comes 0.8 s after the storm starts, unless a
  // storm is already on in another strip: then this strip follows its chain.
  bool fresh;
  FxSync *sh = fxSlot(SX_STORM, SK_ROUND, SEGMENT.speed, fresh);
  const bool others = sh && !fresh && fxLive(*sh);
  if (SEGENV.call == 0) s->at = others ? sh->at : now + 800;
  else if (others && (int32_t)(sh->at - s->at) > 0) s->at = sh->at;   // another strip's chain is ahead: follow it
  uint32_t length;
  for (uint8_t guard = 0; guard < 50 && (int32_t)(now - s->at) >= 0; guard++) {
    stormLit(s->at, 0, length);
    const uint32_t next = s->at + length + 600 + (uint32_t)(255 - SEGMENT.speed) * 60 + stormHash(s->at + 7) % 4000;
    if ((int32_t)(now - next) < 0) break;
    s->at = next;
  }
  if (sh) { if (!others || (int32_t)(s->at - sh->at) > 0) sh->at = s->at; sh->seen = now; }
  if (s->posFor != s->at) {                          // a new strike: where it hits this strip
    s->posFor = s->at;
    s->width = max(2, (int)((uint32_t)len * (16 + SEGMENT.intensity) / 400));
    s->start = hw_random16(max(1, len - s->width + 1));
  }
  const bool lit = (int32_t)(now - s->at) >= 0 && stormLit(s->at, now - s->at, length);
  if (lit) {
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
// fired by an alarm or automation. A strip that starts it while another strip is already in a
// sunrise (same Minutes) joins that one, so the whole house brightens together.
void mode_sunrise() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(uint32_t))) { staticFallback(); return; }
  uint32_t *start = reinterpret_cast<uint32_t *>(SEGENV.data);
  if (SEGENV.call == 0) *start = strip.now;
  // a strip that starts the sunrise while another strip is in one joins it, so they brighten together
  *start = fxMark(SX_SUNRISE, 0, SEGMENT.speed, *start);
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
  // when the show started: shared with the other strips running Vegas Marquee at this Speed, so
  // the chase steps and the routines change together on every strip
  st->sceneStart = fxMark(SX_MARQUEE, 0, SEGMENT.speed, st->sceneStart);
  const uint32_t stepMs = 420 - (uint32_t)SEGMENT.speed * 3 / 2;  // 420 .. 38 ms per step
  uint32_t el = now - st->sceneStart;                       // time into the routine
  if (SEGMENT.custom1) {
    const uint32_t dur = 2500 + (uint32_t)(255 - SEGMENT.custom1) * 50;  // ~15 s .. 2.5 s per routine
    st->scene = (el / dur) % 6;
    el %= dur;
  } else st->scene = 0;
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
  // shared with the other strips running Neon Sign at this Speed, so the sign animates in step
  SEGENV.step = fxMark(SX_NEON, 0, SEGMENT.speed, SEGENV.step);
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
struct Reel { float p0, pf, v; uint32_t stop; };     // v: this reel's spin speed (pixels per ms)
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

  // ---- new round: every strip with Jackpot at this Speed plays the same game together. The first
  // strip whose round is over deals the next one (outcome and end), the others join it.
  static struct { uint32_t at, end, stop[8]; uint8_t nr, win, sym[8]; } dealt = { 0, 0, {}, 0, 0, {} };
  const bool fresh = SEGENV.call == 0 || g->nr != nr;
  uint32_t roundAt = fresh ? now - 100000 : g->start;
  if (fxRound(SX_JACKPOT, SEGMENT.speed, roundAt, fresh || (int32_t)(now - g->end) >= 0, fresh ? UINT32_MAX : 3000)) {
    g->nr = nr;
    g->start = roundAt;
    const bool join = dealt.at == roundAt && dealt.nr == nr;   // another strip dealt this round
    if (join) {
      g->win = dealt.win;
      for (int r = 0; r < nr; r++) g->sym[r] = dealt.sym[r];
    } else {
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
      float need = fmodf(pf - rl.p0, C); if (need < 0) need += C;
      if (join) {
        // stop when the dealer's reel stops: spin a touch faster or slower to land on the symbol
        rl.stop = dealt.stop[r];
        const float run = rl.stop - A / 2.0f - D / 2.0f;       // the reel covers v * run pixels
        float turns = roundf((v * run - need) / C);
        if (need + turns * C <= 0) turns += 1;
        rl.v = (need + turns * C) / run;
      } else {
        const float dNom = v * (nom - A / 2.0f - D / 2.0f);
        float extra = fmodf(need - dNom, C); if (extra < 0) extra += C;
        rl.stop = nom + (uint32_t)(extra / v);
        rl.v = v;
      }
      prevStop = rl.stop;
    }
    if (join) g->end = dealt.end;                      // finish together, so the next round starts together
    else {
      g->end = roundAt + prevStop + 400 + (g->win ? 5600 : 1500);
      dealt.at = roundAt; dealt.end = g->end; dealt.nr = nr; dealt.win = g->win;
      for (int r = 0; r < nr; r++) { dealt.sym[r] = g->sym[r]; dealt.stop[r] = g->reel[r].stop; }
    }
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
    const float rv = rl.v;
    if (t < A) p = rl.p0 + rv * t * t / (2.0f * A);
    else if (t + D < rl.stop) p = rl.p0 + rv * (A / 2.0f) + rv * (t - A);
    else if (t < rl.stop) {
      const float x = (float)(t - (rl.stop - D)) / D;
      p = rl.p0 + rv * (A / 2.0f) + rv * (rl.stop - D - A) + rv * D * (x - x * x / 2.0f);
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
  // new spin: when this wheel's last one is over; every strip with Roulette at this Speed spins
  // together (a strip that's done joins a spin another strip just started)
  uint32_t spinAt = w->start;
  const bool over = w->phase == 2 && t > w->tDrop + settleMs + showMs;
  if (fxRound(SX_ROULETTE, SEGMENT.speed, spinAt, over, SEGENV.call == 0 ? UINT32_MAX : 3000)) {
    // start from where the ball and wheel are now
    const float wNow = wheelAt(t);
    float bNow = w->phase == 2 && w->vb0 > 0 ? w->target + wNow : w->b0;
    w->w0 = fmodf(wNow, W); if (w->w0 < 0) w->w0 += W;
    w->b0 = fmodf(bNow, (float)len); if (w->b0 < 0) w->b0 += len;
    w->vb0 = max(0.12f, len * 0.0011f) * (0.55f + SEGMENT.speed / 300.0f);   // px per ms
    w->start = spinAt; t = (float)(now - spinAt);
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
  // candy time: Speed 128 = real time, 0 = a quarter, 255 = 1.75x; shared with the other strips
  // running Candy Land at this Speed, so the path, gumdrops and swirl move in step
  const float rate = 0.25f + SEGMENT.speed / 170.0f;
  const float adv = dt * rate + st->rem;
  st->ct += (uint32_t)adv;
  st->rem = adv - (uint32_t)adv;
  st->ct = fxClock(SX_CANDY, SEGMENT.speed, rate, st->ct);

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

// ================================================================ UCF Knights (238)
// A black and gold game-day effect that moves to sound through the microphone (AudioReactive):
// the drumline, music and the crowd. A beat is a sudden jump in the bass above the room's
// background noise (as in Boom Fireworks). Looks, picked with the Look slider:
//   0-63    Charge On     - every beat launches a gold charge from pixel 1 that races down the
//                           strip; harder hits run brighter, longer and faster, the biggest with a
//                           white-hot head; gold embers glow underneath with the loudness
//   64-127  Bounce House  - stadium sections (6 pixels and a gap) jump on the kick, alternating
//                           like the two sides of the stands, then drop with a couple of rebounds
//   128-191 Crowd Roar    - gold sparkles as loud as the crowd, a burst on each beat (white glints
//                           on big ones); when the noise stays up a gold wave sweeps down the strip
//   192-255 Tour          - the three in turn, 20 s each, with a short crossfade (the default)
// With no beat for a few seconds (between songs) it settles into slow gold and black stripes.
// Without the microphone (AudioReactive off) it plays its own game-day drumline.
//   Speed, Sensitivity (higher = quieter beats count), Look
//   Checkbox "My colors": slot 1 replaces the gold, slot 2 the white flash (another team's colors)
//   Checkbox "Show mic level": tuning aid, a meter on the first pixels (gold = bass, red = trigger)
// Colors: UCF Bold Gold (#EDB80D) as these LEDs show it, plus the white chip for the flashes.
struct KComet { float x, v, p, len; uint8_t hot; };
constexpr uint8_t MAX_KCOMETS = 10;
struct KnightState {
  uint32_t lastStep, lastBeat, beats, kickT[2];
  float floor, prev, vol, level, pow, roar, waveX, idleX, kickP[2];
  uint8_t wave, nComets;
  KComet comets[MAX_KCOMETS];
};

// what the microphone would hear from a drumline at 128 bpm with crowd swells and a big roar
// for the last 8 seconds of every 36 (used when AudioReactive isn't running)
void knightDemo(uint32_t now, float &level, float &bass) {
  float b = 0, sn = 0;
  const uint64_t t = now, k1 = t * 16 / 1875, k0 = t > 500 ? (t - 500) * 16 / 1875 : 0;   // sixteenths: 117.1875 ms
  for (uint64_t k = k0; k <= k1; k++) {
    const uint64_t te = k * 1875 / 16;
    if (te > t) continue;
    const uint32_t beat = k / 4, sub = k % 4, bar = beat / 4, inBar = beat % 4;
    const bool roar = te % 36000 > 28000, fill = bar % 8 == 7 && inBar >= 2;
    const float d = (t - te) / 1000.0f;
    if (sub == 0 && (inBar % 2 == 0 || roar)) {
      const float p = roar ? 1.0f : 0.78f + 0.12f * ((vhash((uint32_t)k) & 255) / 255.0f);
      b = max(b, p * expf(-d / 0.09f));
    } else if (sub == 0) sn = max(sn, 0.7f * expf(-d / 0.07f));
    else if (fill) sn = max(sn, (0.35f + 0.1f * sub) * expf(-d / 0.07f));
  }
  const float tt = (now % 36000) / 1000.0f;
  float r = 0;
  if (tt > 28.0f) r = constrain((tt - 28.0f) / 1.5f, 0.0f, 1.0f) * constrain((36.0f - tt) / 1.2f, 0.0f, 1.0f);
  const float c = 0.22f + 0.12f * perlin8((uint16_t)(now / 20), 777) / 255.0f + 0.62f * r;
  level = constrain(255.0f * (0.55f * c + 0.45f * b + 0.3f * sn), 0.0f, 255.0f);
  bass = constrain(255.0f * (b + 0.18f * c), 0.0f, 255.0f);
}

// one bounce of a section (0..1): up and down, then two small rebounds
inline float kBounce(float d) {
  if (d < 0) return 0;
  if (d < 0.34f) return sinf(3.14159f * d / 0.34f);
  if (d < 0.54f) return 0.3f * sinf(3.14159f * (d - 0.34f) / 0.2f);
  if (d < 0.66f) return 0.09f * sinf(3.14159f * (d - 0.54f) / 0.12f);
  return 0;
}

void mode_ucf_knights() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(KnightState) + len * 2)) { staticFallback(); return; }
  KnightState *st = reinterpret_cast<KnightState *>(SEGENV.data);
  uint8_t *spark = SEGENV.data + sizeof(KnightState), *white = spark + len;
  const uint32_t now = strip.now;
  if (SEGENV.call == 0) {
    memset(SEGENV.data, 0, sizeof(KnightState) + len * 2);
    st->lastStep = now; st->lastBeat = now - 2500; st->floor = 20.0f;
    st->kickT[0] = st->kickT[1] = now - 5000;
    SEGMENT.fill(BLACK);
  }
  if (now - st->lastStep > 200) st->lastStep = now - 20;           // paused: don't catch up
  if (now - st->lastStep < 20) return;                             // listen and move 50 times a second

  um_data_t *um = nullptr;
  const bool haveMic = UsermodManager::getUMData(&um, USERMOD_ID_AUDIOREACTIVE) && um && um->u_size >= 3;
  const float sp = 0.4f + SEGMENT.speed * (1.6f / 255.0f);          // 0.4x .. 2x, 1x at 96
  const float thresh = 8.0f + (255 - SEGMENT.intensity) * 0.45f;   // Sensitivity 255 -> 8, 0 -> ~123
  const Rgbw GOLD_C = SEGMENT.check1 && SEGCOLOR(0) ? unpackC(SEGCOLOR(0)) : Rgbw{ 255, 140, 0, 30 };
  const Rgbw HOT_C = SEGMENT.check1 && SEGCOLOR(1) ? unpackC(SEGCOLOR(1)) : Rgbw{ 255, 200, 120, 255 };

  for (uint8_t guard = 0; now - st->lastStep >= 20 && guard < 10; guard++) {
    st->lastStep += 20;
    const float dt = 0.02f;
    // ---- listen
    float smth, bass;
    if (haveMic) {
      const uint8_t *fft = (const uint8_t *)um->u_data[2];
      smth = *(float *)um->u_data[0];
      bass = max(fft[0], max(fft[1], fft[2]));                     // ~43-215 Hz: the kick drum
    } else knightDemo(st->lastStep, smth, bass);
    const float level = (bass * 2.0f + smth) / 3.0f;
    st->vol += (constrain(smth, 0.0f, 255.0f) / 255.0f - st->vol) * 0.25f;
    st->level = level;
    const float above = level - st->floor;
    const bool sudden = level - st->prev > thresh * 0.5f;
    st->prev = level;
    st->floor += (level - st->floor) * (level > st->floor ? 0.004f : 0.05f);
    bool beat = false;
    if (above > thresh && sudden && st->lastStep - st->lastBeat > 160) {
      const float span = max(20.0f, 255.0f - st->floor - thresh);
      st->pow = constrain(0.35f + (above - thresh) / span * 0.75f, 0.35f, 1.0f);
      st->lastBeat = st->lastStep; st->beats++; beat = true;
    }
    // ---- move
    if (beat) {
      if (st->nComets < MAX_KCOMETS) {
        KComet &c = st->comets[st->nComets++];
        c.x = -1; c.v = (34 + 50 * st->pow) * sp; c.p = st->pow; c.len = 4 + 8 * st->pow; c.hot = st->pow > 0.8f;
      }
      st->kickT[st->beats % 2] = st->lastStep; st->kickP[st->beats % 2] = st->pow;
      const int burst = (int)(len * (0.06f + 0.2f * st->pow));
      for (int k = 0; k < burst; k++) {
        const int i = hw_random16(len);
        spark[i] = 255; white[i] = st->pow > 0.8f && k % 3 == 0;
      }
    }
    for (uint8_t k = 0; k < st->nComets; ) {
      KComet &c = st->comets[k];
      c.x += c.v * dt;
      if (c.x - c.len >= len) st->comets[k] = st->comets[--st->nComets]; else k++;
    }
    const uint16_t chance = (uint16_t)min(65535.0f, (0.006f + 0.12f * st->vol * st->vol) * 65536.0f);
    for (int i = 0; i < len; i++) {
      spark[i] = (uint16_t)spark[i] * 229 >> 8;                    // fades in ~0.2 s
      if (hw_random16() < chance) { const uint8_t v = 140 + hw_random8(115); if (v > spark[i]) { spark[i] = v; white[i] = 0; } }
    }
    st->roar = max(0.0f, st->roar + dt * (st->vol - 0.42f) * 1.6f);   // loud for ~2-3 s -> the wave
    if (st->roar > 1.0f && !st->wave) { st->wave = 1; st->waveX = -8; st->roar = 0; }
    if (st->wave) { st->waveX += 24 * sp * dt; if (st->waveX > len + 10) st->wave = 0; }
    st->idleX += 2.2f * sp * dt; if (st->idleX >= 8.0f) st->idleX -= 8.0f;
  }

  // ---- draw
  const uint8_t look = SEGMENT.custom1 >> 6;                       // 0 charge, 1 bounce, 2 crowd, 3 tour
  uint8_t a = look, b = 255;
  float kf = 1.0f;
  if (look == 3) {
    const uint32_t per = 20000, f = now % per;
    a = (now / per) % 3;
    if (f < 1400) { b = (a + 2) % 3; kf = f / 1400.0f; }
  }
  const float quiet = constrain(((int32_t)(now - st->lastBeat) - 3000) / 2000.0f, 0.0f, 1.0f);
  const float age[2] = { (now - st->kickT[0]) / 1000.0f * sp, (now - st->kickT[1]) / 1000.0f * sp };
  auto drawLook = [&](uint8_t which, int i) -> Rgbw {
    switch (which) {
      case 0: {                                                    // Charge On
        const float nz = perlin8((uint16_t)(i * 230), (uint16_t)(now >> 2)) / 255.0f;
        Rgbw c = scaleC(GOLD_C, 0.06f + 0.14f * st->vol * nz);
        for (uint8_t k = 0; k < st->nComets; k++) {
          const KComet &m = st->comets[k];
          const float d = m.x - i;
          if (d < -1 || d > m.len) continue;
          Rgbw add;
          if (d < 1) add = scaleC(m.hot ? HOT_C : GOLD_C, d < 0 ? 1 + d : 1);     // the head, between pixels
          else { const float q = 1 - d / m.len; add = scaleC(GOLD_C, q * q * (0.35f + 0.65f * m.p)); }
          c.r += add.r; c.g += add.g; c.b += add.b; c.w += add.w;
        }
        return c;
      }
      case 1: {                                                    // Bounce House
        const int s = i / 7, j = i % 7;
        if (j == 6) return Rgbw{ 0, 0, 0, 0 };                     // the gap between sections
        const float h = kBounce(age[s % 2]) * st->kickP[s % 2];
        const float half = 0.7f + 2.8f * h, d = fabsf(j - 2.5f);
        const float lit = constrain(half - d + 0.5f, 0.0f, 1.0f);
        Rgbw c = scaleC(GOLD_C, 0.08f + 0.1f * st->vol + lit * (0.3f + 0.7f * h));
        if (h > 0.82f && d < 1) c = mixC(c, HOT_C, min(1.0f, (h - 0.82f) * 4));
        return c;
      }
      default: {                                                   // Crowd Roar
        Rgbw c = scaleC(GOLD_C, 0.05f);
        if (spark[i] > 5) {
          const float v = spark[i] / 255.0f;
          const Rgbw add = scaleC(white[i] ? HOT_C : GOLD_C, v * v);
          c.r += add.r; c.g += add.g; c.b += add.b; c.w += add.w;
        }
        if (st->wave) {
          const float d = i - st->waveX, w = expf(-(d * d) / 14.0f);
          const Rgbw add = scaleC(GOLD_C, 0.9f * w);
          c.r += add.r; c.g += add.g; c.b += add.b; c.w += add.w;
          if (fabsf(d) < 0.8f) { const Rgbw h = scaleC(HOT_C, 0.5f * (1 - fabsf(d) / 0.8f)); c.r += h.r; c.g += h.g; c.b += h.b; c.w += h.w; }
        }
        return c;
      }
    }
  };
  for (int i = 0; i < len; i++) {
    Rgbw c = drawLook(a, i);
    if (b != 255) c = mixC(drawLook(b, i), c, kf);
    if (quiet > 0) {                                               // between songs: slow gold and black stripes
      float f = fmodf(i - st->idleX + 800.0f, 8.0f);
      c = mixC(c, scaleC(GOLD_C, f < 5 ? 0.4f : 0.02f), quiet);
    }
    SEGMENT.setPixelColor(i, packC(c));
  }
  // tuning aid: gold bar = what it hears (bass and loudness), red pixel = where a beat counts
  if (SEGMENT.check2) {
    const int bars = min(len, 30);
    const int lv = (int)(st->level * bars / 255.0f);
    const int tp = constrain((int)((st->floor + thresh) * bars / 255.0f), 0, bars - 1);
    for (int i = 0; i < bars; i++) SEGMENT.setPixelColor(i, i < lv ? RGBW32(60, 34, 0, 0) : BLACK);
    SEGMENT.setPixelColor(tp, RGBW32(120, 0, 0, 0));
    if (now - st->lastBeat < 120) SEGMENT.setPixelColor(bars - 1 < len ? bars - 1 : 0, RGBW32(0, 0, 0, 160));
  }
}
const char _data_ucf_knights[] PROGMEM =
  "UCF Knights@Speed,Sensitivity,Look,,,My colors,Show mic level;Gold,Flash;;1f;sx=96,ix=160,c1=255,o1=0,o2=0";


// ================================================================ Autumn Harvest (239)
// Fall colors (maple red, pumpkin, amber, gold, cranberry, harvest yellow), warmed by the W chip,
// in three looks picked with the Look slider:
//   0-63    Falling Leaves - leaves drifting along the strip on the breeze over a low ember glow,
//                            fluttering as they turn over; gusts carry them all faster for a
//                            moment, and each one settles where it lands and fades away
//   64-127  Turning Leaves - every pixel a leaf slowly turning through the fall colors, rustling;
//                            now and then a gust runs down the strip and catches the light
//   128-191 Harvest Bulbs  - fall string lights (maple red, gold, pumpkin, warm clear) glowing like
//                            filament bulbs, the colors stepping slowly along, with twinkles
//   192-255 Tour           - the three in turn, 20 s each, with a short crossfade (the default)
//   Speed, Leaves (how many leaves, how often a gust comes, how many twinkles), Look
//   Checkbox "My colors": the leaf and bulb colors come from slots 1-3 instead of the fall set
// Everything travels toward the end of the segment. Like Candy Land it keeps its own clock
// ("fall time") that runs faster or slower with Speed, so speed changes never jump.
const uint32_t FALL[6] = {
  RGBW32(255, 14, 0, 0),     // maple red
  RGBW32(255, 58, 0, 0),     // pumpkin
  RGBW32(255, 100, 0, 10),   // amber
  RGBW32(255, 145, 0, 30),   // gold
  RGBW32(160, 0, 10, 0),     // cranberry
  RGBW32(255, 175, 12, 70),  // harvest yellow
};
// Turning Leaves goes around this loop: gold, amber, pumpkin, maple red, cranberry, then back
// through rust (cranberry into amber) to gold
const uint8_t TURN_ORDER[6] = { 3, 2, 1, 0, 4, 2 };
// Harvest Bulbs, along the string: maple red, gold, pumpkin, then a clear warm filament bulb
const uint32_t HARVEST_BULBS[4] = { RGBW32(255, 14, 0, 6), RGBW32(255, 145, 0, 36), RGBW32(255, 58, 0, 6), RGBW32(255, 80, 0, 150) };
constexpr uint8_t MAX_LEAVES = 24;

struct Leaf { float x, v, ph, sway, flip, life, age; uint8_t col; };
struct AutumnState {
  uint32_t last, ct;                                  // fall time in whole ms (stays exact for weeks)
  float rem, gust, rustleX, nextRustle;
  uint8_t rustling, n;
  Leaf leaf[MAX_LEAVES];
};

// a new leaf somewhere along the strip; the first ones start part-way through their drift
void leafSpawn(Leaf &l, int len, uint8_t nc, bool first) {
  l.x = hw_random16(len * 16) / 16.0f - 0.5f;
  l.v = 0.7f + hw_random8() / 255.0f * 1.5f;      // pixels per fall-second before the wind
  l.ph = hw_random8() / 40.0f;
  l.sway = 2.2f + hw_random8() / 255.0f * 2.0f;  // back-and-forth flutter, radians per second
  l.flip = 3.0f + hw_random8() / 255.0f * 4.0f;  // how fast it turns over (catching the light)
  l.life = 5.0f + hw_random8() / 255.0f * 6.0f;  // seconds drifting before it lands
  l.age = first ? hw_random8() / 255.0f * l.life : 0.0f;
  l.col = hw_random8(nc);
}

void mode_autumn_harvest() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(AutumnState))) { staticFallback(); return; }
  AutumnState *st = reinterpret_cast<AutumnState *>(SEGENV.data);
  const uint32_t now = strip.now;
  const uint32_t seed = 1 + SEGMENT.start * 7919u;   // each segment rustles differently
  const float den = SEGMENT.intensity / 255.0f;
  const float sp = 0.25f + SEGMENT.speed / 170.0f;   // fall time: Speed 128 = real time, 0 = a quarter, 255 = 1.75x
  Rgbw col[6];
  uint8_t nc = 0;
  if (SEGMENT.check1) for (uint8_t s = 0; s < 3; s++) if (SEGCOLOR(s)) col[nc++] = unpackC(SEGCOLOR(s));
  const bool mine = nc > 0;
  if (!mine) { for (uint8_t k = 0; k < 6; k++) col[k] = unpackC(FALL[k]); nc = 6; }

  const bool first = SEGENV.call == 0;
  if (first) {
    memset(st, 0, sizeof(AutumnState));
    st->last = now; st->ct = 30000; st->nextRustle = 32.0f;
  }
  float dt = (now - st->last) / 1000.0f;
  st->last = now;
  if (dt > 0.2f) dt = 0.2f;                          // paused: don't catch up
  const float adv = dt * sp * 1000.0f + st->rem;
  st->ct += (uint32_t)adv;
  st->rem = adv - (uint32_t)adv;
  const float ft = st->ct / 1000.0f, fdt = dt * sp;
  // noise inputs from fall time, through uint32 so they wrap instead of overflowing the float conversion
  auto tq = [&](float k) { return (uint16_t)(uint32_t)(ft * k); };
  // Turning Leaves and Harvest Bulbs run on the fall time every strip with Autumn Harvest at this
  // Speed shares, so their colors turn and step together; the falling leaves and gusts are each
  // strip's own
  const uint32_t S = fxClock(SX_AUTUMN, SEGMENT.speed, sp, st->ct);
  auto sq = [&](float k) { return (uint16_t)(uint32_t)(S / 1000.0f * k); };

  // the wind: calm most of the time, a gust every so often
  float g = (perlin8(tq(40.0f), (uint16_t)seed) / 255.0f - 0.5f) / 0.3f;
  g = easeC(constrain(g, 0.0f, 1.0f));
  st->gust += (g - st->gust) * min(1.0f, dt * 3.0f);

  // falling leaves
  const int want = constrain((int)lroundf(len / 3.2f * (0.3f + den)), 2, (int)MAX_LEAVES);
  while (st->n < want) leafSpawn(st->leaf[st->n++], len, nc, first);
  if (st->n > want) st->n = want;
  const float wind = 0.55f + 2.2f * st->gust;
  for (uint8_t k = 0; k < st->n; k++) {
    Leaf &l = st->leaf[k];
    l.age += fdt;
    if (l.age < l.life) l.x += l.v * wind * fdt;
    if (l.age > l.life + 2.6f || l.x > len + 1.5f) leafSpawn(l, len, nc, false);
  }
  // turning leaves: a gust running down the strip now and then (more Leaves = more often)
  if (!st->rustling && ft >= st->nextRustle) { st->rustling = 1; st->rustleX = -4.0f; }
  if (st->rustling) {
    st->rustleX += 10.0f * fdt;
    if (st->rustleX > len + 4) { st->rustling = 0; st->nextRustle = ft + 4.0f + (1.0f - den) * 10.0f + hw_random8() / 255.0f * 5.0f; }
  }

  auto falling = [&](int i) -> Rgbw {
    const float nz = perlin8((uint16_t)(i * 70 + seed), tq(25.0f)) / 255.0f;
    Rgbw c = { 255.0f * (0.07f + 0.06f * nz), 255.0f * (0.018f + 0.016f * nz), 0.0f, 5.0f };   // a low ember glow
    for (uint8_t k = 0; k < st->n; k++) {
      const Leaf &l = st->leaf[k];
      const bool down = l.age >= l.life;
      const float fade = min(1.0f, l.age / 0.7f) * (down ? max(0.0f, 1.0f - (l.age - l.life) / 2.6f) : 1.0f);
      if (fade <= 0.0f) continue;
      const float xd = down ? l.x : l.x + sinf(l.age * l.sway + l.ph) * 0.55f;
      const float w = 1.0f - fabsf(i - xd) / 1.5f;
      if (w <= 0.0f) continue;
      const float face = 0.5f + 0.5f * sinf(l.age * l.flip + l.ph * 2.0f);
      const float k2 = w * fade * (down ? 0.6f : 0.62f + 0.38f * face * face);
      const Rgbw &lc = col[l.col % nc];
      c.r += lc.r * k2; c.g += lc.g * k2; c.b += lc.b * k2; c.w += lc.w * k2;
    }
    return c;
  };
  auto turning = [&](int i) -> Rgbw {
    // where this leaf is in its turn: neighbours drift together, each with its own lag
    float u = perlin8((uint16_t)(i * 48 + seed), sq(6.0f)) / 255.0f * 1.6f + h01(i, seed, 11) * 0.5f + (S % 100000) * 1e-5f;
    u -= floorf(u);
    Rgbw c;
    if (mine) { const float v = u * nc; const int a = (int)v % nc; c = mixC(col[a], col[(a + 1) % nc], easeC(v - (int)v)); }
    else { const float v = u * 6; const int a = (int)v % 6; c = mixC(col[TURN_ORDER[a]], col[TURN_ORDER[(a + 1) % 6]], easeC(v - (int)v)); }
    float bri = 0.62f + 0.38f * perlin8((uint16_t)(i * 97 + 500), sq(90.0f)) / 255.0f;   // rustling
    if (st->rustling) {
      const float d = i - st->rustleX, w = expf(-d * d / 6.0f);
      bri += 0.5f * w;
      if (w > 0.3f && h01(i, now / 70, seed + 12) < 0.35f) c.w += 90.0f * w;   // light glinting through
    }
    return scaleC(c, bri);
  };
  auto bulbs = [&](int i) -> Rgbw {
    const uint8_t n = mine ? nc : 4;
    auto bc = [&](int64_t k) { k %= n; if (k < 0) k += n; return mine ? col[k] : unpackC(HARVEST_BULBS[k]); };
    // the colors step one bulb toward the end every 6 fall-seconds, crossfading over 1.2 s
    const int64_t s = S / 6000;
    float f = (S % 6000) / 1200.0f;
    f = f >= 1.0f ? 1.0f : easeC(f);
    Rgbw c = mixC(bc(i - s + 1), bc(i - s), f);
    // filament: each bulb glows a little unevenly, with a faint flicker
    float bri = 0.82f + 0.13f * perlin8((uint16_t)(i * 83 + seed), sq(20.0f)) / 255.0f
              + 0.05f * h01(i, now / 90, seed + 5);
    const uint32_t L = 1100, t = now + (uint32_t)(h01(i, seed, 6) * L), slot = t / L;
    if (h01(i, slot, seed + 7) < den * 0.16f) {      // a twinkle
      const float e = sinf(3.14159f * (t % L) / L);
      bri += 0.3f * e * e; c.w += 120.0f * e * e;
    }
    return scaleC(c, bri);
  };
  auto look = [&](uint8_t which, int i) -> Rgbw {
    switch (which) { case 0: return falling(i); case 1: return turning(i); default: return bulbs(i); }
  };

  const uint8_t lk = SEGMENT.custom1 >> 6;          // 0 falling, 1 turning, 2 bulbs, 3 tour
  uint8_t a = lk, b = 255;
  float kf = 1.0f;
  if (lk == 3) {                                     // tour: 20 s each, 1.4 s crossfade from the last one
    const uint32_t per = 20000, f = now % per;
    a = (now / per) % 3;
    if (f < 1400) { b = (a + 2) % 3; kf = f / 1400.0f; }
  }
  for (int i = 0; i < len; i++) {
    Rgbw c = look(a, i);
    if (b != 255) c = mixC(look(b, i), c, kf);
    SEGMENT.setPixelColor(i, packC(c));
  }
}
const char _data_autumn_harvest[] PROGMEM =
  "Autumn Harvest@Speed,Leaves,Look,,,My colors;!,!,!;;1;sx=128,ix=128,c1=255,o1=0";


// ================================================================ Star Wars (240)
// A galaxy far, far away, in four looks picked with the Look slider (in fifths):
//   0-50    Lightsaber Duel - a blue saber ignites from pixel 1 and a red one from the far end. They
//                             meet in the middle and fight: strikes that clash in white-hot flashes
//                             with sparks flying, flurries, blade locks that crackle while one pushes
//                             the other back, stepping apart to circle. Then the finale: the loser's
//                             blade goes out, the winner's after it, and the next duel starts with a
//                             new hero color (blue, green, purple)
//   51-101  Blaster Fight   - red bolts from one end and green ones from the other, hitting cover and
//                             exploding at the far end; now and then a blue saber lights up in the
//                             middle and sends the green bolts back where they came from
//   102-152 Hyperspace      - drifting stars stretch into streaks, a flash, and the jump: blue and
//                             white hyperspace rushing along the strip, then back out to the stars
//   153-203 Twin Suns       - a desert sunset: two suns sliding slowly along the strip in a gold,
//                             coral and deep purple sky (a calm one for the whole evening)
//   204-255 Tour            - the four in turn, 25 s each, each from its beginning (the default)
//   Speed, Action (how fierce: how often they strike and shoot, how many sparks and stars), Look
//   Checkbox "My colors": slot 1 = the hero's saber, slot 2 = the villain's saber
// Everything travels toward the end of the segment, on its own clock ("star time") that runs
// faster or slower with Speed.
const uint32_t SW_HERO[3] = { RGBW32(0, 70, 255, 0), RGBW32(0, 255, 40, 0), RGBW32(150, 0, 255, 0) };   // blue, green, purple
constexpr uint32_t SW_VILLAIN = RGBW32(255, 0, 0, 0);
constexpr uint8_t SW_SPARKS = 28, SW_BOLTS = 14, SW_BOOMS = 6, SW_STARS = 24;
struct SwSpark { float x, v, life, max; };
struct SwBolt { float x, v, stop; uint8_t side; };
struct SwBoom { float x, t, p; };
enum : uint8_t { SD_DARK, SD_IGN_H, SD_IGN_V, SD_SQUARE, SD_FIGHT, SD_FINALE, SD_LOSE_OFF, SD_WIN_HOLD, SD_WIN_OFF };
enum : uint8_t { SM_STRIKE, SM_LOCK, SM_BACK };
struct StarWarsState {
  uint32_t last, st;                                  // star time in whole ms
  float rem;
  uint8_t look;                                       // the look that's running (to start each from the top)
  // duel: the blades meet around c with a gap g between the tips; fh, fv = how far each is lit
  uint8_t phase, move, flurry, contact, winner, duels, nSparks;
  uint32_t moveEnd, fightEnd, ignH, ignV, clashT;
  float c, g, cT, gT, rate, fh, fv, push, clashX, clashP;
  SwSpark sparks[SW_SPARKS];
  // blaster fight
  uint8_t nBolts, nBooms;
  SwBolt bolts[SW_BOLTS];
  SwBoom booms[SW_BOOMS];
  float jediX, jediT, nextJedi;
  // hyperspace (hyperAt is on the shared clock, sharedT the shared clock last frame)
  uint32_t hyperAt, sharedT;
  float dist, flow;
};

inline float swRnd() { return hw_random16() / 65536.0f; }
void swSparks(StarWarsState *s, float x, int n, float power) {
  for (int k = 0; k < n && s->nSparks < SW_SPARKS; k++) {
    SwSpark &p = s->sparks[s->nSparks++];
    p.x = x; p.v = (swRnd() < 0.5f ? -1.0f : 1.0f) * (4.0f + swRnd() * 12.0f) * power;
    p.max = p.life = 0.25f + swRnd() * 0.4f;
  }
}

void mode_star_wars() {
  const int len = SEGLEN;
  if (len < 1 || !SEGENV.allocateData(sizeof(StarWarsState))) { staticFallback(); return; }
  StarWarsState *s = reinterpret_cast<StarWarsState *>(SEGENV.data);
  const uint32_t now = strip.now;
  const uint32_t seed = 1 + SEGMENT.start * 7919u;
  const float L = len, mid = L * 0.5f - 0.5f;
  const float act = SEGMENT.intensity / 255.0f;
  const float sp = 0.25f + SEGMENT.speed / 170.0f;   // Speed 128 = real time
  if (SEGENV.call == 0) {
    memset(s, 0, sizeof(StarWarsState));
    s->last = now; s->st = 100000; s->look = 255; s->nextJedi = 4.0f; s->jediT = -1.0f;
    s->phase = SD_DARK; s->moveEnd = s->st + 600; s->c = mid; s->g = L * 0.3f;
  }
  float dt = (now - s->last) / 1000.0f;
  s->last = now;
  if (dt > 0.2f) dt = 0.2f;
  const float adv = dt * sp * 1000.0f + s->rem;
  s->st += (uint32_t)adv;
  s->rem = adv - (uint32_t)adv;
  const float fdt = dt * sp;
  const uint32_t T = s->st;                           // this strip's own time: its duel and blaster fight
  // the time every strip running Star Wars at this Speed shares: Hyperspace and Twin Suns
  const uint32_t S = fxClock(SX_STARWARS, SEGMENT.speed, sp, s->st);
  const bool rejoined = (int32_t)(S - s->sharedT) < 0 || S - s->sharedT > 1000;   // just started, or picked up another strip's time
  s->sharedT = S;

  // which look (Tour: 25 s each with a 1.5 s crossfade); a look that just came up starts from the top
  const uint8_t lk = (uint16_t)SEGMENT.custom1 * 5 >> 8;   // 0 duel, 1 blasters, 2 hyperspace, 3 suns, 4 tour
  uint8_t a = lk, b = 255;
  float kf = 1.0f;
  if (lk == 4) {
    const uint32_t per = 25000, f = now % per;
    a = (now / per) % 4;
    if (f < 1500) { b = (a + 3) % 4; kf = f / 1500.0f; }
  }
  const bool entered = a != s->look;
  if (entered) {
    if (a == 0) { s->phase = SD_DARK; s->moveEnd = T + 500; s->fh = s->fv = 0; s->nSparks = 0; }
    s->look = a;
  }
  // Hyperspace starts from the top (the jump about 3 s in), or in step with a strip already in it
  if (a == 2 || b == 2)
    s->hyperAt = fxMark(SX_STARWARS, 2, SEGMENT.speed, (entered && a == 2) || rejoined ? S - 1500 : s->hyperAt);

  // ---------------- the duel
  Rgbw hero = unpackC(SW_HERO[s->duels % 3]), villain = unpackC(SW_VILLAIN);
  if (SEGMENT.check1 && SEGCOLOR(0)) hero = unpackC(SEGCOLOR(0));
  if (SEGMENT.check1 && SEGCOLOR(1)) villain = unpackC(SEGCOLOR(1));
  auto clash = [&](float power) {
    s->clashX = s->c; s->clashT = T; s->clashP = power;
    swSparks(s, s->c, 2 + (int)(power * (3.0f + 6.0f * act)), power);
  };
  auto pickMove = [&]() {
    const float r = swRnd(), pause = 1.0f - 0.55f * act;   // more Action, quicker moves
    if (s->flurry) {                                     // a flurry: quick strikes in a row
      s->flurry--; s->move = SM_STRIKE; s->cT = mid + (swRnd() - 0.5f) * L * 0.2f; s->gT = 0; s->rate = 22;
      s->moveEnd = T + 260 + hw_random16(120);
    } else if (r < 0.42f) {                              // a strike: blades swing in and clash
      s->move = SM_STRIKE; s->cT = mid + (swRnd() - 0.5f) * L * 0.24f; s->gT = 0; s->rate = 15;
      s->moveEnd = T + (uint32_t)((450 + hw_random16(500)) * pause) + 250;
    } else if (r < 0.6f) {
      s->flurry = 2 + hw_random8(3); s->moveEnd = T;     // starts on the next frame
    } else if (r < 0.8f) {                               // a blade lock: pushing, crackling
      s->move = SM_LOCK; s->cT = mid + (swRnd() - 0.5f) * L * 0.16f; s->gT = 0; s->rate = 10;
      s->push = (swRnd() < 0.5f ? -1.0f : 1.0f) * (0.6f + swRnd() * 1.2f);
      s->moveEnd = T + 1400 + hw_random16(1200);
    } else {                                             // step apart and circle
      s->move = SM_BACK; s->cT = mid + (swRnd() - 0.5f) * L * 0.1f; s->gT = L * (0.16f + swRnd() * 0.14f); s->rate = 4;
      s->moveEnd = T + (uint32_t)((900 + hw_random16(700)) * pause);
    }
  };
  switch (s->phase) {
    case SD_DARK:
      s->fh = s->fv = 0;
      if (T >= s->moveEnd) { s->phase = SD_IGN_H; s->c = s->cT = mid; s->g = s->gT = L * 0.28f; s->ignH = T; s->moveEnd = T + 600; }
      break;
    case SD_IGN_H:
      if (T >= s->moveEnd) { s->phase = SD_IGN_V; s->ignV = T; s->moveEnd = T + 800; }
      break;
    case SD_IGN_V:
      if (T >= s->moveEnd) { s->phase = SD_SQUARE; s->moveEnd = T + 700; }
      break;
    case SD_SQUARE:
      if (T >= s->moveEnd) { s->phase = SD_FIGHT; s->fightEnd = T + 15000 + hw_random16(4000); s->moveEnd = T; }
      break;
    case SD_FIGHT:
      if (T >= s->fightEnd && s->move != SM_LOCK) {      // the finale: the winner drives the other back
        s->phase = SD_FINALE; s->winner = hw_random8(2); s->gT = 0; s->rate = 3.5f;
        s->cT = s->winner == 0 ? L * 0.8f : L * 0.2f; s->moveEnd = T + 1700; s->flurry = 0;
      } else if (T >= s->moveEnd) {
        if (s->move == SM_LOCK && s->contact) { clash(1.0f); s->gT = 3.0f + swRnd() * 3.0f; s->rate = 9; }   // shoved apart
        pickMove();
      }
      break;
    case SD_FINALE:
      if (T >= s->moveEnd) { clash(1.3f); s->phase = SD_LOSE_OFF; s->moveEnd = T + 700; }
      break;
    case SD_LOSE_OFF:
      if (T >= s->moveEnd) { s->phase = SD_WIN_HOLD; s->moveEnd = T + 1500; }
      break;
    case SD_WIN_HOLD:
      if (T >= s->moveEnd) { s->phase = SD_WIN_OFF; s->moveEnd = T + 700; }
      break;
    case SD_WIN_OFF:
      if (T >= s->moveEnd) { s->phase = SD_DARK; s->moveEnd = T + 1500; s->duels++; }
      break;
  }
  // blades follow their targets; ignition and retraction take ~0.35 s
  const float ign = fdt / 0.35f;
  if (s->phase >= SD_IGN_H && s->phase <= SD_FINALE) s->fh = min(1.0f, s->fh + ign);
  if (s->phase >= SD_IGN_V && s->phase <= SD_FINALE) s->fv = min(1.0f, s->fv + ign);
  if (s->phase == SD_LOSE_OFF || s->phase == SD_WIN_HOLD) { float &f = s->winner == 0 ? s->fv : s->fh; f = max(0.0f, f - fdt / 0.45f); }
  if (s->phase == SD_WIN_OFF) { s->fh = max(0.0f, s->fh - fdt / 0.45f); s->fv = max(0.0f, s->fv - fdt / 0.45f); }
  if (s->phase == SD_DARK) s->fh = s->fv = 0;
  if (s->move == SM_LOCK && s->phase == SD_FIGHT && s->contact) s->cT += s->push * fdt + (swRnd() - 0.5f) * 0.3f;
  s->cT = constrain(s->cT, 2.0f, L - 3.0f);
  const float kk = min(1.0f, s->rate * fdt);
  s->c += (s->cT - s->c) * kk;
  s->g += (s->gT - s->g) * kk;
  const bool fighting = s->phase == SD_FIGHT || s->phase == SD_FINALE;
  if (fighting && s->g < 0.4f && !s->contact) {
    s->contact = 1;
    clash(s->move == SM_LOCK ? 0.7f : s->flurry ? 0.6f : 0.85f);
    if (s->move == SM_STRIKE) { s->gT = 1.5f + swRnd() * 2.5f; s->rate = 9; }   // recoil
  }
  if (s->g > 0.9f) s->contact = 0;
  if (fighting && s->contact && (s->move == SM_LOCK || s->phase == SD_FINALE) && hw_random8() < 50 + 80 * act) {
    swSparks(s, s->c + (swRnd() - 0.5f), 1, 0.5f);       // crackle
    if (hw_random8() < 90) { s->clashX = s->c; s->clashT = T; s->clashP = 0.35f + 0.3f * swRnd(); }
  }
  for (uint8_t k = 0; k < s->nSparks; ) {
    SwSpark &p = s->sparks[k];
    p.x += p.v * fdt; p.v *= max(0.0f, 1.0f - 2.5f * fdt); p.life -= fdt;
    if (p.life <= 0 || p.x < -2 || p.x > L + 1) s->sparks[k] = s->sparks[--s->nSparks]; else k++;
  }

  // ---------------- the blaster fight
  {
    const float rate = (0.35f + 1.8f * act) * fdt;      // bolts per side per star-second
    for (uint8_t side = 0; side < 2; side++) {
      if (s->nBolts < SW_BOLTS && swRnd() < rate) {
        SwBolt &q = s->bolts[s->nBolts++];
        q.side = side; q.x = side ? L - 0.5f : -0.5f;
        q.v = (side ? -1.0f : 1.0f) * (26.0f + swRnd() * 16.0f);
        q.stop = swRnd() < 0.3f ? L * (0.3f + swRnd() * 0.4f) : (side ? -0.5f : L - 0.5f);   // cover, or the far end
      }
    }
    if (s->jediT < 0.0f || s->jediT > 6.0f) {
      s->jediT = -1.0f;
      if ((s->nextJedi -= fdt) <= 0.0f) { s->jediT = 0.0f; s->jediX = L * (0.3f + swRnd() * 0.25f); s->nextJedi = 9.0f + swRnd() * 8.0f; }
    } else s->jediT += fdt;
    const bool jedi = s->jediT >= 0.3f && s->jediT <= 5.6f;
    auto boom = [&](float x, float p) {
      if (s->nBooms < SW_BOOMS) { SwBoom &o = s->booms[s->nBooms++]; o.x = x; o.t = 0; o.p = p; }
      swSparks(s, x, 2 + (int)(3 * p), 0.6f);
    };
    for (uint8_t k = 0; k < s->nBolts; ) {
      SwBolt &q = s->bolts[k];
      const float was = q.x;
      q.x += q.v * fdt;
      if (jedi && q.side == 1 && q.v < 0 && was >= s->jediX + 2.0f && q.x < s->jediX + 2.0f) {   // deflected
        q.v = -q.v; q.x = s->jediX + 2.0f; q.stop = L - 0.5f;
        s->clashX = q.x; s->clashT = T; s->clashP = 0.6f;
      }
      const bool hit = q.v > 0 ? q.x >= q.stop : q.x <= q.stop;
      if (hit) { boom(q.stop, q.stop > 0.5f && q.stop < L - 1.0f ? 0.55f : 1.0f); s->bolts[k] = s->bolts[--s->nBolts]; } else k++;
    }
    for (uint8_t k = 0; k < s->nBooms; ) {
      SwBoom &o = s->booms[k];
      o.t += fdt;
      if (o.t > 0.9f) s->booms[k] = s->booms[--s->nBooms]; else k++;
    }
  }

  // ---------------- hyperspace (a 17 s cycle: cruise, stretch, flash, tunnel, drop out)
  const uint32_t h = (S - s->hyperAt) % 17000;
  float hs;                                             // how fast the stars go by
  if (h < 4500 || h >= 15000) hs = 1.0f;
  else if (h < 6500) { const float u = (h - 4500) / 2000.0f; hs = 1.0f + 90.0f * u * u * u; }
  else if (h < 13500) hs = 91.0f;
  else { const float u = 1.0f - (h - 13500) / 1500.0f; hs = 1.0f + 90.0f * u * u; }
  s->dist += hs * fdt;
  if (s->dist > 1.0e6f) s->dist -= 1.0e6f;
  s->flow += fdt * 45.0f;
  if (s->flow > 1.0e5f) s->flow -= 1.0e5f;

  // ---------------- drawing
  const float tipH = -0.5f + s->fh * (s->c - s->g * 0.5f + 0.5f);
  const float tipV = (L - 0.5f) - s->fv * ((L - 0.5f) - (s->c + s->g * 0.5f));
  const float flash = s->clashP * expf(-(float)(T - s->clashT) / 90.0f);
  auto add = [](Rgbw &c, const Rgbw &d, float k) { c.r += d.r * k; c.g += d.g * k; c.b += d.b * k; c.w += d.w * k; };
  const Rgbw HOT = { 255, 210, 160, 255 }, SPARK_C = { 255, 140, 30, 90 };
  auto sparkle = [&](Rgbw &c, int i) {
    for (uint8_t k = 0; k < s->nSparks; k++) {
      const SwSpark &p = s->sparks[k];
      const float w = 1.0f - fabsf(i - p.x);
      if (w > 0) add(c, SPARK_C, w * p.life / p.max);
    }
  };
  auto duel = [&](int i) -> Rgbw {
    Rgbw c = { 0, 0, 0, 0 };
    for (uint8_t side = 0; side < 2; side++) {
      const float cov = side == 0 ? constrain(tipH - i + 0.5f, 0.0f, 1.0f) : constrain(i - tipV + 0.5f, 0.0f, 1.0f);
      if (cov <= 0) continue;
      const uint32_t ig = side == 0 ? s->ignH : s->ignV;
      float bri = 0.84f + 0.1f * perlin8((uint16_t)(i * 30 + side * 5000), (uint16_t)(now / 3)) / 255.0f
                + 0.06f * sinf(now * 0.06f + side * 2.0f);                       // the hum
      if (T - ig < 600) bri += 0.3f * expf(-(float)(T - ig) / 150.0f);           // snap-hiss on ignition
      bri *= 1.0f + 0.35f * flash;
      add(c, side == 0 ? hero : villain, cov * bri);
      c.w += 30.0f * cov * bri;                                                  // the white core
    }
    const float d = fabsf(i - s->clashX);
    if (flash > 0.02f && d < 2.5f) add(c, HOT, flash * (1.0f - d / 2.5f));
    sparkle(c, i);
    return c;
  };
  auto blasters = [&](int i) -> Rgbw {
    Rgbw c = { 2, 3, 9, 0 };                                                     // smoke in the dark
    const Rgbw BOLT_R = { 255, 10, 0, 30 }, BOLT_G = { 20, 255, 0, 20 }, FIRE = { 255, 110, 10, 60 };
    for (uint8_t k = 0; k < s->nBolts; k++) {
      const SwBolt &q = s->bolts[k];
      const float dd = (q.x - i) * (q.v > 0 ? 1.0f : -1.0f);                    // behind the head
      if (dd < -0.6f || dd > 3.0f) continue;
      const float w = dd < 0 ? 1.0f + dd / 0.6f : powf(1.0f - dd / 3.0f, 1.5f);
      add(c, q.side ? BOLT_G : BOLT_R, w);
      c.w += 50.0f * w * (dd < 0.5f);
    }
    for (uint8_t k = 0; k < s->nBooms; k++) {
      const SwBoom &o = s->booms[k];
      const float r = 0.8f + min(2.4f, o.t * 7.0f), d = fabsf(i - o.x);
      if (d > r) continue;
      const float e = o.p * expf(-o.t / 0.22f) * (1.0f - d / (r + 0.01f));
      add(c, o.t < 0.08f ? HOT : FIRE, e);
    }
    if (s->jediT >= 0.0f) {                                                      // the Jedi's saber, 5 px
      const float on = constrain(s->jediT / 0.3f, 0.0f, 1.0f) * constrain((6.0f - s->jediT) / 0.3f, 0.0f, 1.0f);
      const float half = 2.5f * on, d = fabsf(i - s->jediX);
      const float cov = constrain(half - d + 0.5f, 0.0f, 1.0f);
      if (cov > 0) {
        const float bri = 0.85f + 0.15f * perlin8((uint16_t)(i * 40), (uint16_t)(now / 3)) / 255.0f;
        add(c, unpackC(SW_HERO[0]), cov * bri); c.w += 30.0f * cov * bri;
      }
    }
    const float d = fabsf(i - s->clashX);
    if (flash > 0.02f && d < 2.0f) add(c, HOT, flash * (1.0f - d / 2.0f));
    sparkle(c, i);
    return c;
  };
  auto hyperspace = [&](int i) -> Rgbw {
    Rgbw c = { 0, 0, 3, 0 };
    const bool tunnel = h >= 6500 && h < 14200;
    float starK = h < 6500 ? 1.0f : h < 13500 ? 0.0f : constrain((h - 13500) / 400.0f, 0.0f, 1.0f);
    if (starK > 0) {
      const int n = constrain((int)(L / 3.0f * (0.5f + act)), 4, (int)SW_STARS);
      const Rgbw STAR = { 140, 170, 255, 120 };
      for (int j = 0; j < n; j++) {
        const float depth = 0.5f + h01(j, seed, 22), bri = 0.3f + 0.7f * h01(j, seed, 23);
        float x = fmodf(h01(j, seed, 21) * (L + 6.0f) + s->dist * depth, L + 6.0f) - 3.0f;
        const float sl = 0.4f + min(10.0f, (hs - 1.0f) * 0.2f) * depth;          // streak length
        const float dd = x - i;
        if (dd < -0.7f || dd > sl + 0.7f) continue;
        const float w = dd < 0 ? 1.0f + dd / 0.7f : (dd < sl ? 1.0f - 0.7f * dd / (sl + 0.01f) : 0.3f * (1.0f - (dd - sl) / 0.7f));
        const float tw = 0.65f + 0.35f * perlin8((uint16_t)(j * 900), (uint16_t)(now / 4)) / 255.0f;
        add(c, STAR, w * bri * tw * starK * min(1.0f, 0.7f + hs * 0.01f) / (1.0f + sl * 0.12f));
      }
    }
    if (tunnel) {
      const float in = constrain((h - 6500) / 300.0f, 0.0f, 1.0f), out = constrain((14200 - (float)h) / 700.0f, 0.0f, 1.0f);
      const float u = i * 0.2f - s->flow * 0.2f;
      const float n1 = perlin8((uint16_t)(int32_t)(u * 256.0f), (uint16_t)(now / 5)) / 255.0f;
      const float band = constrain((n1 - 0.42f) / 0.33f, 0.0f, 1.0f);
      Rgbw t = { 0, 18, 90, 0 };
      add(t, Rgbw{ 60, 150, 255, 90 }, band * band);
      if (h01(i, now / 60, seed + 24) < 0.06f + 0.06f * act) t.w += 160.0f;     // glints rushing by
      add(c, t, in * out);
    }
    if (h >= 6500 && h < 7300) add(c, Rgbw{ 150, 180, 255, 255 }, expf(-(h - 6500) / 160.0f));   // the jump
    if (h >= 13500 && h < 14000) add(c, Rgbw{ 120, 150, 255, 200 }, 0.6f * expf(-(h - 13500) / 120.0f));
    return c;
  };
  auto suns = [&](int i) -> Rgbw {
    const float span = L + 14.0f, p1 = fmodf((S % 100000) / 100000.0f * span, span) - 7.0f, p2 = p1 - L * 0.13f - 1.5f;
    const float d = min(fabsf(i - p1), fabsf(i - p2)) / max(10.0f, L * 0.45f);
    const Rgbw GOLD = { 255, 110, 8, 30 }, CORAL = { 230, 40, 50, 0 }, DUSK = { 50, 12, 100, 0 }, NIGHT = { 14, 4, 48, 0 };
    Rgbw c = d < 0.5f ? mixC(GOLD, CORAL, d * 2.0f) : d < 1.0f ? mixC(CORAL, DUSK, (d - 0.5f) * 2.0f)
           : mixC(DUSK, NIGHT, constrain((d - 1.0f) / 0.6f, 0.0f, 1.0f));
    c = scaleC(c, 0.88f + 0.12f * perlin8((uint16_t)(i * 60 + seed), (uint16_t)(S / 40)) / 255.0f);   // heat shimmer
    add(c, Rgbw{ 255, 190, 110, 190 }, expf(-(i - p1) * (i - p1) / 0.9f));
    add(c, Rgbw{ 255, 140, 50, 110 }, expf(-(i - p2) * (i - p2) / 0.6f));
    return c;
  };
  auto look = [&](uint8_t which, int i) -> Rgbw {
    switch (which) { case 0: return duel(i); case 1: return blasters(i); case 2: return hyperspace(i); default: return suns(i); }
  };
  for (int i = 0; i < len; i++) {
    Rgbw c = look(a, i);
    if (b != 255) c = mixC(look(b, i), c, kf);
    SEGMENT.setPixelColor(i, packC(c));
  }
}
const char _data_star_wars[] PROGMEM =
  "Star Wars@Speed,Action,Look,,,My colors;Hero,Villain,!;;1;sx=128,ix=128,c1=255,o1=0";


// ================================================================ Window matrix (2D, 241-242)
// Effects for the LED matrix in the office window: a grid of SK6812 RGBW chips set up as a 2D
// matrix in LED Preferences (24 x 24 planned). They draw on a 24 x 24 design grid that's scaled
// to the matrix and centered, so they work on other sizes too. On a strip (1D) they show color 1.
//   Window Clock (241)   - the time, big, with an info line (weather, pool...) from the segment name
//   Holiday Window (242) - animated scenes: Halloween, Thanksgiving, Christmas, Fireworks,
//                          Valentine's, St. Patrick's, Coastal sunset; Auto picks by the date
// Both keep a float picture of the matrix (the canvas) and send it out once a frame.

constexpr int WIN_MAX_PX = 4096;                     // up to 64 x 64

struct Canvas {
  int wd, ht;                                        // matrix pixels
  float pu;                                          // design units per pixel (24 / the short side)
  float ox, oy;                                      // pixels left/above the centered design square
  Rgbw *fb;
  // design units <-> pixels
  float px(float u) const { return u / pu + ox - 0.5f; }
  float py(float v) const { return v / pu + oy - 0.5f; }
  float du(int x) const { return (x + 0.5f - ox) * pu; }
  float dv(int y) const { return (y + 0.5f - oy) * pu; }
};

void cvInit(Canvas &cv, void *mem) {
  cv.wd = SEG_W; cv.ht = SEG_H;
  const int s = min(cv.wd, cv.ht);
  cv.pu = 24.0f / s;
  cv.ox = (cv.wd - s) * 0.5f; cv.oy = (cv.ht - s) * 0.5f;
  cv.fb = reinterpret_cast<Rgbw *>(mem);
}

// paint every pixel from fn(u, v) in design units; small matrices get 2 x 2 samples per pixel so
// round shapes come out smooth
template <class Fn> void cvPaint(Canvas &cv, Fn fn) {
  const bool ss = min(cv.wd, cv.ht) <= 40;
  const float q = cv.pu * 0.25f;
  for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) {
    const float u = cv.du(x), v = cv.dv(y);
    Rgbw c;
    if (ss) {
      const Rgbw a = fn(u - q, v - q), b = fn(u + q, v - q), d = fn(u - q, v + q), e = fn(u + q, v + q);
      c = { (a.r + b.r + d.r + e.r) * 0.25f, (a.g + b.g + d.g + e.g) * 0.25f, (a.b + b.b + d.b + e.b) * 0.25f, (a.w + b.w + d.w + e.w) * 0.25f };
    } else c = fn(u, v);
    cv.fb[x + y * cv.wd] = c;
  }
}

inline void cvAt(Canvas &cv, int x, int y, const Rgbw &c, float k, bool over) {
  if (x < 0 || y < 0 || x >= cv.wd || y >= cv.ht || k <= 0.0f) return;
  Rgbw &p = cv.fb[x + y * cv.wd];
  if (over) p = mixC(p, c, k > 1.0f ? 1.0f : k);
  else { p.r += c.r * k; p.g += c.g * k; p.b += c.b * k; p.w += c.w * k; }
}
// a point at (u, v) spread over the 4 nearest pixels: added (light) or painted over (silhouettes)
void cvDot(Canvas &cv, float u, float v, const Rgbw &c, float k, bool over = false) {
  const float x = cv.px(u), y = cv.py(v);
  const int x0 = (int)floorf(x), y0 = (int)floorf(y);
  const float fx = x - x0, fy = y - y0;
  cvAt(cv, x0, y0, c, k * (1 - fx) * (1 - fy), over);
  cvAt(cv, x0 + 1, y0, c, k * fx * (1 - fy), over);
  cvAt(cv, x0, y0 + 1, c, k * (1 - fx) * fy, over);
  cvAt(cv, x0 + 1, y0 + 1, c, k * fx * fy, over);
}
// a soft glow of radius r (design units) added at (u, v)
void cvGlow(Canvas &cv, float u, float v, const Rgbw &c, float k, float r) {
  const float x = cv.px(u), y = cv.py(v), rp = r / cv.pu;
  const int x0 = (int)floorf(x - rp * 2), x1 = (int)ceilf(x + rp * 2), y0 = (int)floorf(y - rp * 2), y1 = (int)ceilf(y + rp * 2);
  const float inv = 1.0f / (rp * rp + 0.05f);
  for (int yy = y0; yy <= y1; yy++) for (int xx = x0; xx <= x1; xx++) {
    const float dx = xx - x, dy = yy - y;
    cvAt(cv, xx, yy, c, k * expf(-(dx * dx + dy * dy) * inv), false);
  }
}
// a line of dots from (u0, v0) to (u1, v1), painted over
void cvLine(Canvas &cv, float u0, float v0, float u1, float v1, const Rgbw &c, float k) {
  const float len = sqrtf((u1 - u0) * (u1 - u0) + (v1 - v0) * (v1 - v0)) / cv.pu;
  const int n = 1 + (int)(len * 1.5f);
  for (int i = 0; i <= n; i++) { const float f = (float)i / n; cvDot(cv, u0 + (u1 - u0) * f, v0 + (v1 - v0) * f, c, k, true); }
}
void cvFlush(const Canvas &cv) {
  for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) SEGMENT.setPixelColorXY(x, y, packC(cv.fb[x + y * cv.wd]));
}

// the window effects' own clock (scene time), faster or slower with Speed, so Speed never jumps
struct WinState { uint32_t last, ct; float rem; };
float winClock(WinState *st, float rate) {
  const uint32_t now = strip.now;
  if (SEGENV.call == 0) { st->last = now; st->ct = 0; st->rem = 0; }
  uint32_t d = now - st->last; st->last = now;
  if (d > 200) d = 200;                               // paused: don't catch up
  const float adv = d * rate + st->rem;
  st->ct += (uint32_t)adv; st->rem = adv - (uint32_t)adv;
  return st->ct / 1000.0f;
}
// noise from a float clock: through uint32 so it wraps the same way on the ESP32
inline uint16_t nq(float x) { return (uint16_t)(uint32_t)(x); }
inline float sstep(float e0, float e1, float x) { x = constrain((x - e0) / (e1 - e0), 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
inline float fract1(float x) { return x - floorf(x); }
// inside a triangle pointing up (apex above the base) or down
inline bool inTri(float u, float v, float cx, float apexY, float baseY, float hw) {
  const float f = (v - apexY) / (baseY - apexY);
  return f >= 0.0f && f <= 1.0f && fabsf(u - cx) <= f * hw;
}
// the classic heart curve, centered at (cx, cy), size s (half width ~ s), point down
inline bool inHeart(float u, float v, float cx, float cy, float s) {
  const float x = (u - cx) / s * 1.12f, y = -(v - cy) / s * 1.12f + 0.12f;
  const float a = x * x + y * y - 1.0f;
  return a * a * a - x * x * y * y * y <= 0.0f;
}

// ---------------------------------------------------------------- tiny fonts
// 3 x 5 letters for the info line. Each glyph: width, then 5 rows of 3 bits (4 = left column).
// Lower case shows as upper case; "°" (or *) is the degree sign.
struct WinGlyph { char ch; uint8_t w, r[5]; };
const WinGlyph FONT35[] PROGMEM = {
  {' ',2,{0,0,0,0,0}}, {'0',3,{7,5,5,5,7}}, {'1',3,{2,6,2,2,7}}, {'2',3,{7,1,7,4,7}}, {'3',3,{7,1,3,1,7}},
  {'4',3,{5,5,7,1,1}}, {'5',3,{7,4,7,1,7}}, {'6',3,{7,4,7,5,7}}, {'7',3,{7,1,1,2,2}}, {'8',3,{7,5,7,5,7}},
  {'9',3,{7,5,7,1,7}}, {'A',3,{2,5,7,5,5}}, {'B',3,{6,5,6,5,6}}, {'C',3,{3,4,4,4,3}}, {'D',3,{6,5,5,5,6}},
  {'E',3,{7,4,6,4,7}}, {'F',3,{7,4,6,4,4}}, {'G',3,{3,4,5,5,3}}, {'H',3,{5,5,7,5,5}}, {'I',3,{7,2,2,2,7}},
  {'J',3,{1,1,1,5,2}}, {'K',3,{5,5,6,5,5}}, {'L',3,{4,4,4,4,7}}, {'M',3,{5,7,7,5,5}}, {'N',3,{6,5,5,5,5}},
  {'O',3,{2,5,5,5,2}}, {'P',3,{6,5,6,4,4}}, {'Q',3,{2,5,5,6,3}}, {'R',3,{6,5,6,5,5}}, {'S',3,{3,4,2,1,6}},
  {'T',3,{7,2,2,2,2}}, {'U',3,{5,5,5,5,7}}, {'V',3,{5,5,5,5,2}}, {'W',3,{5,5,7,7,5}}, {'X',3,{5,5,2,5,5}},
  {'Y',3,{5,5,2,2,2}}, {'Z',3,{7,1,2,4,7}}, {'.',1,{0,0,0,0,2}}, {':',1,{0,2,0,2,0}}, {'-',3,{0,0,7,0,0}},
  {'/',3,{1,1,2,4,4}}, {'%',3,{5,1,2,4,5}}, {'+',3,{0,2,7,2,0}}, {'!',1,{2,2,2,0,2}}, {'?',3,{7,1,3,0,2}},
  {',',1,{0,0,0,2,2}}, {'\'',1,{2,2,0,0,0}}, {'\x01',3,{2,5,2,0,0}},   // \x01 = degree sign
};
// 4 x 7 digits for the time; each row is 4 bits (8 = left column)
const uint8_t DIGITS47[10][7] PROGMEM = {
  {6,9,9,9,9,9,6}, {2,6,2,2,2,2,7}, {6,9,1,2,4,8,15}, {14,1,1,6,1,1,14}, {2,6,10,10,15,2,2},
  {15,8,14,1,1,9,6}, {6,8,8,14,9,9,6}, {15,1,1,2,4,4,4}, {6,9,9,6,9,9,6}, {6,9,9,7,1,1,6},
};
const uint8_t DIGITS35[10][5] PROGMEM = {
  {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,3,1,7}, {5,5,7,1,1}, {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,2,2}, {7,5,7,5,7}, {7,5,7,1,7},
};

const WinGlyph *glyphFor(char ch) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (ch == '*') ch = '\x01';
  for (const WinGlyph &g : FONT35) if (g.ch == ch) return &g;
  return nullptr;
}
// the info text cleaned up for the small font: "°" (UTF-8 C2 B0) becomes the degree glyph,
// anything the font doesn't have is dropped
uint8_t infoText(const char *in, char *out, uint8_t cap) {
  uint8_t n = 0;
  for (const uint8_t *p = (const uint8_t *)in; p && *p && n + 1 < cap; p++) {
    char ch = (char)*p;
    if (*p == 0xC2) continue;
    if (*p == 0xB0 || *p == 0xBA) ch = '\x01';
    if (glyphFor(ch)) out[n++] = ch;
  }
  out[n] = 0;
  return n;
}
int textWidth(const char *s) {
  int w = 0;
  for (; *s; s++) { const WinGlyph *g = glyphFor(*s); if (g) w += g->w + 1; }
  return w > 0 ? w - 1 : 0;
}
// draw text at pixel (x, y), whole pixels only (crisp on the LEDs); clipped to the matrix
void drawText(Canvas &cv, const char *s, int x, int y, const Rgbw &c) {
  for (; *s; s++) {
    const WinGlyph *g = glyphFor(*s);
    if (!g) continue;
    for (int r = 0; r < 5; r++) for (int k = 0; k < g->w; k++) {
      const int bit = g->w == 1 ? 2 : (4 >> k);
      if (g->r[r] & bit) cvAt(cv, x + k, y + r, c, 1.0f, true);
    }
    x += g->w + 1;
  }
}

// ---------------------------------------------------------------- the calendar
// day of week, 0 = Sunday (Sakamoto)
int dowOf(int y, int m, int d) {
  static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
enum : uint8_t { HW_AUTO, HW_HALLOWEEN, HW_THANKS, HW_XMAS, HW_FIREWORKS, HW_VALENTINE, HW_PADDY, HW_COASTAL };
// which holiday the date belongs to (Coastal the rest of the year, or before the clock is set)
uint8_t holidayFor(time_t t) {
  if (t < 1600000000) return HW_COASTAL;
  const int m = month(t), d = day(t), y = year(t);
  const int thanks = 1 + (4 - dowOf(y, 11, 1) + 7) % 7 + 21;   // fourth Thursday of November
  if (m == 10) return HW_HALLOWEEN;
  if (m == 11) return d <= thanks ? HW_THANKS : HW_XMAS;
  if (m == 12) return d == 31 ? HW_FIREWORKS : HW_XMAS;
  if (m == 1 && d == 1) return HW_FIREWORKS;
  if (m == 2 && d <= 14) return HW_VALENTINE;
  if (m == 3 && d <= 17) return HW_PADDY;
  if (m == 7 && d <= 5) return HW_FIREWORKS;
  return HW_COASTAL;
}

// ================================================================ Window Clock (241)
// The time in big 4 x 7 digits, with an info line under it in small letters and an animated band
// along the bottom. The info line is the segment's name: Home Assistant writes the weather and the
// pool temperature there (e.g. "78° POOL 84°"); longer text scrolls. With no name it shows the
// weekday and date. The time comes from WLED's clock (Config > Time & Macros: NTP and time zone).
//   Speed       - how fast the background moves (and the info line scrolls)
//   Glow        - how bright the background is (0 = black)
//   Look        - background, in quarters: Coastal waves, Starry night, Holiday colors (by date), Plain
//   My colors   - time, info and background accent from color slots 1-3
//   24 hour     - 24 h time (otherwise 12 h without AM/PM)
//   Blink colon - the colon blinks every second
void mode_window_clock() {
  const int wd = SEG_W, ht = SEG_H;
  if (!SEGMENT.is2D() || wd * ht > WIN_MAX_PX || !SEGENV.allocateData(sizeof(WinState) + wd * ht * sizeof(Rgbw))) { staticFallback(); return; }
  WinState *st = reinterpret_cast<WinState *>(SEGENV.data);
  Canvas cv; cvInit(cv, SEGENV.data + sizeof(WinState));
  const float t = winClock(st, 0.25f + SEGMENT.speed / 170.0f);
  const float glow = SEGMENT.intensity / 255.0f;
  const uint8_t look = SEGMENT.custom1 >> 6;
  const bool mine = SEGMENT.check1;
  const Rgbw timeTop = mine && SEGCOLOR(0) ? unpackC(SEGCOLOR(0)) : Rgbw{ 255, 150, 0, 60 };
  const Rgbw timeBot = mine && SEGCOLOR(0) ? scaleC(unpackC(SEGCOLOR(0)), 0.8f) : Rgbw{ 255, 70, 0, 0 };
  const Rgbw infoC = mine && SEGCOLOR(1) ? unpackC(SEGCOLOR(1)) : Rgbw{ 0, 170, 230, 0 };

  // ---- layout: time on top, info under it, the background band along the bottom
  const time_t now = localTime;
  int hh = hour(now), mm = minute(now);
  if (!SEGMENT.check2) { hh %= 12; if (hh == 0) hh = 12; }
  char digits[4]; uint8_t nd = 0;
  if (hh >= 10 || SEGMENT.check2) digits[nd++] = hh / 10;
  digits[nd++] = hh % 10; digits[nd++] = mm / 10; digits[nd++] = mm % 10;
  // pick the biggest font that fits: 4 x 7 with a space each side of the colon, then without,
  // then 3 x 5 the same way
  bool big = true; int cgap = 1, tw = 0;
  for (int pass = 0; pass < 4; pass++) {
    big = pass < 2; cgap = (pass & 1) ? 0 : 1;
    tw = nd * (big ? 4 : 3) + nd - 2 + 2 * cgap + 1;          // digits 1 px apart, the colon with cgap each side
    if (tw <= wd) break;
  }
  const int th = big ? 7 : 5;
  const int yTime = max(0, ht * 2 / 24);
  const int yInfo = yTime + th + max(2, ht * 3 / 24);
  const bool showInfo = yInfo + 5 <= ht;
  const int yBand = showInfo ? yInfo + 5 + max(1, ht * 2 / 24) : yTime + th + 2;

  // ---- background
  const uint8_t hol = holidayFor(now);
  Rgbw hc[3];
  switch (hol) {
    case HW_HALLOWEEN: hc[0] = { 255, 60, 0, 0 };   hc[1] = { 90, 0, 140, 0 };  hc[2] = { 60, 200, 0, 0 }; break;
    case HW_THANKS:    hc[0] = { 255, 14, 0, 0 };   hc[1] = { 255, 100, 0, 10 }; hc[2] = { 160, 0, 10, 0 }; break;
    case HW_XMAS:      hc[0] = { 255, 0, 0, 0 };    hc[1] = { 0, 160, 20, 0 };  hc[2] = { 0, 0, 0, 160 }; break;
    case HW_FIREWORKS: hc[0] = { 255, 0, 0, 0 };    hc[1] = { 0, 0, 0, 170 };   hc[2] = { 0, 30, 255, 0 }; break;
    case HW_VALENTINE: hc[0] = { 255, 0, 40, 0 };   hc[1] = { 255, 40, 120, 30 }; hc[2] = { 0, 0, 0, 120 }; break;
    case HW_PADDY:     hc[0] = { 0, 200, 30, 0 };   hc[1] = { 255, 140, 0, 20 }; hc[2] = { 120, 255, 40, 0 }; break;
    default:           hc[0] = { 0, 90, 110, 0 };   hc[1] = { 0, 25, 80, 0 };   hc[2] = { 0, 0, 0, 90 }; break;
  }
  if (mine && SEGCOLOR(2)) hc[0] = hc[1] = unpackC(SEGCOLOR(2));
  const float bandH = max(1, ht - yBand);
  for (int y = 0; y < ht; y++) for (int x = 0; x < wd; x++) {
    Rgbw c = { 0, 0, 0, 0 };
    const bool inBand = y >= yBand;
    if (look == 0) {                                     // coastal: a night sky, the sea rolling in along the bottom
      c = { 0, 2.0f + 3.0f * y / ht, 8.0f + 14.0f * y / ht, 0 };
      if (inBand) {
        const float d = (y - yBand + 0.5f) / bandH;      // 0 = the top of the sea
        const float n = perlin8(nq(x * 34.0f + t * 22.0f), nq(y * 52.0f - t * 30.0f), nq(t * 9.0f)) / 255.0f;
        c = mixC(Rgbw{ 0, 40, 110, 0 }, Rgbw{ 0, 130, 150, 0 }, constrain(n * 1.4f - 0.2f - d * 0.3f, 0.0f, 1.0f));
        const float crest = sstep(0.62f, 0.8f, n) * (1.0f - d * 0.5f);
        c.w += 150.0f * crest; c.b += 40.0f * crest;
        c = scaleC(c, 0.25f + 0.75f * (1.0f - d * 0.45f));
      }
    } else if (look == 1) {                              // starry night
      c = { 0, 0, 6.0f + 6.0f * y / ht, 0 };
      const bool nearText = (y >= yTime - 1 && y <= yTime + th) || (showInfo && y >= yInfo - 1 && y <= yInfo + 5);
      if (!nearText && h01(x, y, 777) < 0.09f) {
        const float tw2 = 0.5f + 0.5f * sinf(t * (1.3f + h01(x, y, 778) * 2.0f) + h01(x, y, 779) * 6.28f);
        c.w += 40.0f + 120.0f * tw2 * tw2; c.b += 30.0f * tw2;
      }
    } else if (look == 2 && inBand) {                    // holiday colors: stripes drifting along the band
      const float s = fract1((x + (y - yBand) * 0.8f) / 6.0f - t * 0.25f) * 3.0f;
      const int i = (int)s; const float f = sstep(0.75f, 1.0f, s - i);
      c = mixC(hc[i % 3], hc[(i + 1) % 3], f);
      c = scaleC(c, 0.55f + 0.45f * perlin8(nq(x * 40.0f), nq(t * 40.0f)) / 255.0f);
    }
    cv.fb[x + y * wd] = scaleC(c, glow);
  }

  // ---- the time
  const bool colon = !SEGMENT.check3 || (strip.now % 1000) < 500;
  int x = (wd - tw + 1) / 2;
  for (int i = 0; i < nd; i++) {
    if (i == nd - 2) {                                  // the colon before the minutes
      const int cx = x - 1 + cgap;
      if (colon) for (int r = 0; r < th; r++) {
        const bool on = big ? (r == 2 || r == 4) : (r == 1 || r == 3);
        if (on) cvAt(cv, cx, yTime + r, mixC(timeTop, timeBot, (float)r / (th - 1)), 1.0f, true);
      }
      x = cx + 1 + cgap + 1;
    }
    const uint8_t dgt = digits[i];
    const int gw = big ? 4 : 3;
    for (int r = 0; r < th; r++) {
      const uint8_t bits = big ? DIGITS47[dgt][r] : DIGITS35[dgt][r];
      for (int k = 0; k < gw; k++) if (bits & ((big ? 8 : 4) >> k)) cvAt(cv, x + k, yTime + r, mixC(timeTop, timeBot, (float)r / (th - 1)), 1.0f, true);
    }
    x += gw + 1;
  }

  // ---- the info line: the segment's name, or the weekday and date
  if (showInfo) {
    char txt[WLED_MAX_SEGNAME_LEN + 1];
    if (SEGMENT.name && SEGMENT.name[0]) infoText(SEGMENT.name, txt, sizeof(txt));
    else txt[0] = 0;
    if (!txt[0]) {
      static const char *const WD[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
      if (now > 1600000000) snprintf(txt, sizeof(txt), "%s %d", WD[(weekday(now) + 6) % 7], day(now));
    }
    const int iw = textWidth(txt);
    if (iw <= wd) drawText(cv, txt, (wd - iw + 1) / 2, yInfo, infoC);
    else {                                               // too long: scroll it right to left, with a gap between repeats
      const int loop = iw + 8;
      const int off = (int)(t * 7.0f) % loop;
      drawText(cv, txt, wd - off, yInfo, infoC);
      drawText(cv, txt, wd - off + loop, yInfo, infoC);
    }
  }
  cvFlush(cv);
}
const char _data_window_clock[] PROGMEM =
  "Window Clock@Speed,Glow,Look,,,My colors,24 hour,Blink colon;Time,Info,Accent;;2;sx=128,ix=160,c1=0,o1=0,o2=0,o3=0";

// ================================================================ Holiday Window (242)
// Animated scenes for the window, picked with the Scene slider (in eighths):
//   0-31    Auto          - by the date: Halloween all October, Thanksgiving through Thanksgiving
//                           Day, Christmas until the 30th, Fireworks on New Year's Eve and Day and
//                           July 1-5, Valentine's Feb 1-14, St. Patrick's Mar 1-17, else Coastal
//   32-63   Halloween     - a jack-o'-lantern flickering by candlelight, bats crossing the moon
//   64-95   Thanksgiving  - a turkey fanning its tail, leaves blowing past
//   96-127  Christmas     - a tree with twinkling bulbs and a shining star, snow falling
//   128-159 Fireworks     - rockets and bursts: peonies, rings, gold willows, crackle
//   160-191 Valentine's   - a beating heart, little hearts floating up
//   192-223 St. Patrick's - a shamrock with a shimmer, gold glints falling
//   224-255 Coastal       - a sunset over the water with a palm, the sun going down and coming back
//   Speed (1x at 128), Sparkle (snow, stars, twinkles, bats, leaves, how busy the fireworks are)
struct HolCtx { float t, den, flick, bob, sway, beat, sunY; };

// one scene's picture at (u, v) on the 24 x 24 design grid
Rgbw holidayPx(uint8_t sc, float u, float v, const HolCtx &k) {
  switch (sc) {
    case HW_HALLOWEEN: {
      Rgbw c = mixC(Rgbw{ 8, 0, 26, 0 }, Rgbw{ 46, 2, 64, 0 }, sstep(0.0f, 21.0f, v));
      const float md = sqrtf((u - 18.6f) * (u - 18.6f) + (v - 4.6f) * (v - 4.6f));
      if (md < 3.3f) {
        const float cr = perlin8(nq(u * 90.0f), nq(v * 90.0f)) / 255.0f;
        c = mixC(Rgbw{ 255, 200, 90, 70 }, Rgbw{ 190, 140, 50, 30 }, sstep(0.5f, 0.8f, cr));
      } else {                                         // moonlight around it
        const float g = expf(-(md - 3.3f) * 1.1f);
        c.r += 70.0f * g; c.g += 45.0f * g; c.b += 20.0f * g;
      }
      const float ground = 21.4f + 0.6f * sinf(u * 0.45f + 1.0f);
      if (v > ground) c = { 6, 12, 2, 0 };
      // the pumpkin
      const float pu = (u - 12.0f) / 8.7f, pv = (v - 15.7f) / 6.6f, e = pu * pu + pv * pv;
      if (e < 1.0f) {
        const float rib = 0.6f + 0.4f * fabsf(sinf(asinf(constrain(pu / sqrtf(max(0.05f, 1.0f - pv * pv)), -1.0f, 1.0f)) * 2.6f + 1.57f));
        c = scaleC(Rgbw{ 255, 52, 0, 0 }, rib * (0.8f - 0.4f * e * e) * (0.85f + 0.15f * k.flick));
        const Rgbw fire = scaleC(Rgbw{ 255, 190, 40, 150 }, k.flick);
        bool face = inTri(u, v, 8.5f, 11.7f, 14.6f, 1.9f) || inTri(u, v, 15.5f, 11.7f, 14.6f, 1.9f) || inTri(u, v, 12.0f, 14.6f, 16.3f, 1.0f);
        const float m = (u - 12.0f) / 6.2f;
        if (fabsf(m) < 1.0f) {
          const float top = 17.0f + 0.9f * (1.0f - m * m), bot = 17.4f + 2.5f * sqrtf(1.0f - m * m);
          if (v > top && v < bot && !(u > 9.4f && u < 10.8f && v < top + 0.9f) && !(u > 13.2f && u < 14.6f && v > bot - 0.9f)) face = true;
        }
        if (face) c = fire;
      }
      if (u > 11.2f && u < 12.9f && v > 7.6f && v < 9.6f - (u - 11.2f) * 0.2f) c = { 70, 110, 0, 0 };   // stem
      // candle light on the ground in front
      if (v > ground) { const float g = expf(-((u - 12.0f) * (u - 12.0f)) / 30.0f); c.r += 60.0f * g * k.flick; c.g += 18.0f * g * k.flick; }
      return c;
    }
    case HW_THANKS: {
      Rgbw c = mixC(Rgbw{ 3, 2, 16, 0 }, Rgbw{ 34, 8, 4, 0 }, sstep(2.0f, 21.0f, v));           // dusk
      if (v > 21.6f) c = (h01((int)(u * 2.0f), (int)(v * 2.0f), 5) < 0.25f) ? Rgbw{ 120, 70, 0, 10 } : Rgbw{ 45, 18, 0, 0 };   // hay
      // tail fan: rings of red, orange and gold feathers with rounded tips, swaying a little
      const float fx = u - 12.0f, fy = v - 15.6f, r = sqrtf(fx * fx + fy * fy);
      const float th = atan2f(fx, -fy) + 0.06f * sinf(k.t * 1.3f);    // 0 = straight up
      const float R = 11.3f * (0.965f + 0.035f * sinf(k.t * 0.9f)) * (0.9f + 0.1f * fabsf(cosf(th * 4.5f)));
      if (r < R && fabsf(th) < 1.9f) {
        const float q = r / R;
        c = q > 0.80f ? Rgbw{ 220, 6, 0, 0 } : q > 0.62f ? Rgbw{ 255, 75, 0, 0 } : Rgbw{ 255, 160, 0, 40 };
        if (fabsf(fract1(th * 4.5f / 3.1416f + 0.5f) - 0.5f) < 0.07f && q > 0.5f) c = scaleC(c, 0.35f);   // between feathers
      }
      // body, with a darker wing
      const float bd = (u - 12.0f) * (u - 12.0f) / 25.0f + (v - 16.2f) * (v - 16.2f) / 26.0f;
      if (bd < 1.0f) {
        c = scaleC(Rgbw{ 150, 58, 8, 0 }, 1.0f - 0.35f * bd);
        const float wx = (u - 12.0f) / 3.2f, wy = (v - 17.6f) / 2.6f;
        if (wx * wx + wy * wy < 1.0f && fabsf(u - 12.0f) > 0.6f) c = { 95, 34, 4, 0 };
      }
      // head with a dark outline, white eyes, beak and the red wattle
      const float hy = 10.4f + k.bob;
      const float hd = (u - 12.0f) * (u - 12.0f) + (v - hy) * (v - hy);
      if (hd < 9.0f && v < hy + 2.4f) c = { 25, 8, 0, 0 };
      if (hd < 5.3f) c = { 110, 52, 14, 0 };
      if (fabsf(fabsf(u - 12.0f) - 1.5f) < 0.5f && fabsf(v - hy + 0.9f) < 0.5f) c = { 0, 0, 0, 255 };   // eyes
      if (fabsf(u - 12.0f) < 1.0f && v > hy + 0.1f && v < hy + 1.2f) c = { 255, 190, 0, 40 };      // beak
      if (u > 12.0f && u < 13.0f && v > hy + 1.2f && v < hy + 3.3f) c = { 255, 0, 0, 0 };            // wattle
      // feet
      if (v > 20.8f && v < 22.0f && (fabsf(u - 10.8f) < 0.45f || fabsf(u - 13.2f) < 0.45f)) c = { 255, 110, 0, 0 };
      return c;
    }
    case HW_XMAS: {
      Rgbw c = mixC(Rgbw{ 0, 2, 18, 0 }, Rgbw{ 0, 10, 44, 0 }, sstep(0.0f, 21.0f, v));
      const float ground = 21.5f + 0.5f * sinf(u * 0.5f);
      if (v > ground) c = { 10, 16, 32, 28 };
      // three tiers
      const float tiers[3][3] = { { 4.0f, 10.4f, 4.4f }, { 7.4f, 15.0f, 6.6f }, { 10.8f, 19.8f, 8.8f } };
      for (const auto &tr : tiers) {
        if (inTri(u, v, 12.0f, tr[0], tr[1], tr[2])) {
          const float f = (v - tr[0]) / (tr[1] - tr[0]);
          c = mixC(Rgbw{ 0, 150, 30, 0 }, Rgbw{ 0, 70, 14, 0 }, constrain(f * 0.8f + fabsf(u - 12.0f) / (tr[2] + 0.1f) * 0.3f, 0.0f, 1.0f));
        }
      }
      if (u > 10.9f && u < 13.1f && v > 19.8f && v < 21.9f) c = { 90, 40, 6, 0 };            // trunk
      // presents
      if (v > 19.9f && v < 22.2f && u > 4.4f && u < 7.6f) c = (fabsf(u - 6.0f) < 0.4f || fabsf(v - 21.0f) < 0.35f) ? Rgbw{ 255, 160, 0, 40 } : Rgbw{ 180, 0, 10, 0 };
      if (v > 20.6f && v < 22.2f && u > 16.6f && u < 19.4f) c = (fabsf(u - 18.0f) < 0.4f) ? Rgbw{ 0, 0, 0, 160 } : Rgbw{ 0, 40, 200, 0 };
      return c;
    }
    case HW_FIREWORKS: {
      Rgbw c = mixC(Rgbw{ 0, 0, 4, 0 }, Rgbw{ 2, 4, 22, 0 }, sstep(4.0f, 23.0f, v));
      // a low skyline (palms and rooftops) along the bottom
      const float sky = 22.0f - 1.2f * (sinf(u * 0.9f) > 0.6f) - 0.8f * (fmodf(u, 7.0f) < 3.0f);
      if (v > sky) c = { 1, 1, 3, 0 };
      return c;
    }
    case HW_VALENTINE: {
      Rgbw c = mixC(Rgbw{ 18, 0, 8, 0 }, Rgbw{ 40, 0, 18, 0 }, sstep(0.0f, 24.0f, v));
      for (int i = 0; i < 6; i++) {                     // little hearts floating up
        const float sp = 1.4f + h01(i, 3, 41) * 1.4f;
        const float y = 26.0f - fmodf(k.t * sp + h01(i, 4, 41) * 30.0f, 30.0f);
        const float x = 2.0f + h01(i, 5, 41) * 20.0f + sinf(k.t * 1.1f + i) * 1.2f;
        if (inHeart(u, v, x, y, 1.5f)) c = (i % 3 == 0) ? Rgbw{ 0, 0, 0, 150 } : (i % 3 == 1 ? Rgbw{ 255, 40, 110, 20 } : Rgbw{ 200, 0, 30, 0 });
      }
      const float s = 7.8f * k.beat;
      if (inHeart(u, v, 12.0f, 12.4f, s)) {
        const float hx = (u - 12.0f) / s, hy = (v - 12.4f) / s;
        c = mixC(Rgbw{ 255, 0, 30, 0 }, Rgbw{ 255, 70, 130, 60 }, constrain(0.9f - hy * 0.9f - hx * 0.5f - 0.6f, 0.0f, 1.0f) * 0.9f);
        c = scaleC(c, 0.75f + 0.25f * k.beat / 1.08f);
      }
      return c;
    }
    case HW_PADDY: {
      Rgbw c = mixC(Rgbw{ 0, 8, 4, 0 }, Rgbw{ 0, 22, 8, 0 }, sstep(0.0f, 24.0f, v));
      const float cx = 12.0f, cy = 10.8f;
      // stem: a gentle curve down to the right
      const float sv = (v - cy) / 10.0f;
      if (sv > 0.0f && sv < 1.0f && fabsf(u - (cx + 2.2f * sv * sv)) < 0.75f) c = { 0, 140, 25, 0 };
      // three heart-shaped leaves pointing out from the middle
      for (int i = 0; i < 3; i++) {
        const float a = -1.5708f + i * 2.0944f;        // up, lower right, lower left
        const float ca = cosf(a), sa = sinf(a);
        const float ru = u - cx, rv = v - cy;
        // rotate so this leaf points up, then test a heart (point toward the middle)
        const float lu = ru * (-sa) + rv * ca, lv = -(ru * ca + rv * sa);
        if (inHeart(lu, lv, 0.0f, -4.5f, 3.9f)) {
          const float sh = 0.5f + 0.5f * sinf((u + v) * 0.5f - k.t * 2.2f);
          c = mixC(Rgbw{ 0, 170, 30, 0 }, Rgbw{ 60, 255, 80, 20 }, sh * sh * 0.7f);
          if (fabsf(lu) < 0.35f && lv < -0.8f) c = scaleC(c, 0.6f);   // the vein
        }
      }
      return c;
    }
    default: {                                          // Coastal sunset
      const float hz = 16.5f;
      const float hi = constrain((hz - k.sunY) / 10.0f, 0.0f, 1.0f);     // 1 = sun high, 0 = set
      const Rgbw top = mixC(Rgbw{ 4, 0, 18, 0 }, Rgbw{ 8, 24, 80, 0 }, hi);
      const Rgbw low = mixC(Rgbw{ 150, 8, 36, 0 }, Rgbw{ 255, 90, 8, 10 }, hi);
      Rgbw c;
      if (v < hz) {
        c = mixC(top, low, powf(constrain(v / hz, 0.0f, 1.0f), 1.6f));
        const float sd = sqrtf((u - 14.5f) * (u - 14.5f) + (v - k.sunY) * (v - k.sunY));
        if (sd < 2.8f) c = mixC(Rgbw{ 255, 210, 80, 140 }, Rgbw{ 255, 110, 0, 10 }, sd / 2.8f);
        else { const float g = expf(-(sd - 2.8f) * 1.1f) * (0.4f + 0.6f * hi); c.r += 70.0f * g; c.g += 28.0f * g; }
      } else {                                           // the water: waves, and the sun's glitter on them
        const float d = (v - hz) / (24.0f - hz);
        c = mixC(Rgbw{ 0, 26, 60, 0 }, Rgbw{ 0, 8, 30, 0 }, d);
        c = mixC(c, scaleC(low, 0.35f), (1.0f - d) * 0.5f);
        const float n = perlin8(nq(u * 40.0f + k.t * 6.0f), nq(v * 110.0f - k.t * 34.0f)) / 255.0f;
        const float lane = expf(-((u - 14.5f) * (u - 14.5f)) / (2.0f + d * 14.0f));
        const float sunOn = constrain((hz + 2.5f - k.sunY) / 3.0f, 0.0f, 1.0f);
        const float glit = sstep(0.55f, 0.8f, n) * lane * sunOn;
        c.r += 255.0f * glit; c.g += 140.0f * glit; c.b += 30.0f * glit; c.w += 90.0f * glit;
        c.w += 30.0f * sstep(0.7f, 0.85f, n) * (1.0f - d);
      }
      // the beach and the palm (silhouettes)
      if (v > 21.0f - 2.5f * expf(-(u * u) / 30.0f) && u < 9.0f + (v - 21.0f) * 3.0f) c = { 0, 0, 0, 0 };
      const float tv = (v - 9.6f) / 12.0f;               // trunk from the crown (0) to the sand (1)
      if (tv > 0.0f && tv < 1.0f) {
        const float tx = 5.6f - 2.3f * tv * tv + k.sway * (1.0f - tv) * 0.6f;
        if (fabsf(u - tx) < 0.55f + 0.35f * tv) c = { 0, 0, 0, 0 };
      }
      return c;
    }
  }
}

void mode_holiday_window() {
  const int wd = SEG_W, ht = SEG_H;
  if (!SEGMENT.is2D() || wd * ht > WIN_MAX_PX || !SEGENV.allocateData(sizeof(WinState) + wd * ht * sizeof(Rgbw))) { staticFallback(); return; }
  WinState *st = reinterpret_cast<WinState *>(SEGENV.data);
  Canvas cv; cvInit(cv, SEGENV.data + sizeof(WinState));
  const float t = winClock(st, 0.25f + SEGMENT.speed / 170.0f);
  uint8_t sc = SEGMENT.custom1 >> 5;
  if (sc == HW_AUTO) sc = holidayFor(localTime);
  HolCtx k;
  k.t = t; k.den = SEGMENT.intensity / 255.0f;
  k.flick = 0.72f + 0.28f * perlin8(nq(t * 260.0f), 99) / 255.0f;
  k.bob = 0.35f * sinf(t * 2.4f) + (fmodf(t, 7.0f) > 6.2f ? 0.8f * sinf((fmodf(t, 7.0f) - 6.2f) * 3.93f) : 0.0f);   // a peck now and then
  k.sway = sinf(t * 0.8f);
  { const float ph = fract1(t / 1.1f); k.beat = 1.0f + 0.07f * expf(-ph * ph / 0.004f) + 0.045f * expf(-(ph - 0.24f) * (ph - 0.24f) / 0.004f); }
  k.sunY = 11.0f + 8.0f * (0.5f - 0.5f * cosf(t * 6.2832f / 90.0f));   // sets over 45 s, rises over 45 s
  cvPaint(cv, [&](float u, float v) { return holidayPx(sc, u, v, k); });

  const float den = k.den;
  auto hashPx = [&](int x, int y, uint32_t s) { return h01(x + y * 131, s, 4242); };
  switch (sc) {
    case HW_HALLOWEEN: {
      for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) {          // stars
        const float u = cv.du(x), v = cv.dv(y);
        if (v > 9.5f || (u - 18.6f) * (u - 18.6f) + (v - 4.6f) * (v - 4.6f) < 20.0f) continue;
        if (hashPx(x, y, 1) < 0.05f * den + 0.01f) {
          const float tw = 0.5f + 0.5f * sinf(t * (1.5f + hashPx(x, y, 2) * 2.0f) + hashPx(x, y, 3) * 6.28f);
          cvAt(cv, x, y, Rgbw{ 30, 20, 60, 140 }, tw * tw, false);
        }
      }
      const int bats = 1 + (int)(den * 3.0f);
      for (int i = 0; i < bats; i++) {                 // bats: silhouettes flapping across
        const float sp = 2.2f + h01(i, 1, 66) * 1.6f;
        const float x = fmodf(t * sp + h01(i, 2, 66) * 34.0f, 34.0f) - 5.0f;
        const float y = 2.5f + h01(i, 3, 66) * 5.5f + sinf(t * 1.3f + i * 2.0f) * 1.4f;
        const float fl = sinf(t * 13.0f + i * 1.7f);
        const Rgbw bat = { 0, 0, 0, 0 };
        cvDot(cv, x, y, bat, 1.0f, true);
        cvLine(cv, x, y, x - 2.3f, y - 1.3f * fl, bat, 0.9f);
        cvLine(cv, x, y, x + 2.3f, y - 1.3f * fl, bat, 0.9f);
      }
      break;
    }
    case HW_THANKS: {
      const int n = 2 + (int)(den * 6.0f);
      for (int i = 0; i < n; i++) {                    // leaves blowing past
        const float sp = 2.0f + h01(i, 1, 55) * 2.5f;
        const float x = fmodf(t * sp + h01(i, 2, 55) * 32.0f, 32.0f) - 4.0f;
        const float y = fmodf(t * (0.7f + h01(i, 3, 55)) + h01(i, 4, 55) * 26.0f, 26.0f) - 1.0f + sinf(t * 2.0f + i) * 0.8f;
        const Rgbw lc = unpackC(FALL[i % 6]);
        const float flip = 0.55f + 0.45f * sinf(t * 5.0f + i * 1.3f);
        cvDot(cv, x, y, scaleC(lc, flip), 1.0f, true);
      }
      break;
    }
    case HW_XMAS: {
      static const Rgbw BULBS[5] = { { 255, 0, 0, 0 }, { 255, 140, 0, 40 }, { 0, 40, 255, 0 }, { 0, 0, 0, 255 }, { 255, 0, 90, 0 } };
      const float tiers[3][3] = { { 4.0f, 10.4f, 4.4f }, { 7.4f, 15.0f, 6.6f }, { 10.8f, 19.8f, 8.8f } };
      for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) {          // twinkling bulbs on the tree
        const float u = cv.du(x), v = cv.dv(y);
        bool on = false;
        for (const auto &tr : tiers) on |= inTri(u, v, 12.0f, tr[0] + 1.2f, tr[1] - 0.4f, tr[2] - 1.0f);
        if (!on || hashPx(x, y, 7) > 0.2f + 0.12f * den) continue;
        const float tw = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * (0.8f + 2.5f * den * hashPx(x, y, 8)) + hashPx(x, y, 9) * 6.28f));
        cv.fb[x + y * cv.wd] = scaleC(BULBS[(int)(hashPx(x, y, 10) * 5)], tw);
      }
      const float sh = 0.7f + 0.3f * sinf(t * 3.0f);    // the star on top
      cvGlow(cv, 12.0f, 3.4f, Rgbw{ 255, 170, 30, 160 }, 0.9f * sh, 0.8f);
      cvLine(cv, 12.0f, 1.6f, 12.0f, 5.0f, Rgbw{ 255, 200, 60, 220 }, sh);
      cvLine(cv, 10.4f, 3.4f, 13.6f, 3.4f, Rgbw{ 255, 200, 60, 220 }, sh);
      const int flakes = 6 + (int)(den * 26.0f);
      for (int i = 0; i < flakes; i++) {               // snow
        const float sp = 1.2f + h01(i, 1, 33) * 1.6f;
        const float y = fmodf(t * sp + h01(i, 2, 33) * 27.0f, 27.0f) - 1.5f;
        const float x = h01(i, 3, 33) * 25.0f - 0.5f + sinf(t * 0.9f + i * 1.9f) * 0.8f;
        cvDot(cv, x, y, Rgbw{ 40, 50, 70, 200 }, 0.85f, false);
      }
      break;
    }
    case HW_FIREWORKS: {
      static const Rgbw FW[7] = { { 255, 0, 0, 0 }, { 0, 0, 0, 255 }, { 0, 30, 255, 0 }, { 255, 110, 0, 50 }, { 0, 220, 30, 0 }, { 160, 0, 255, 0 }, { 255, 0, 90, 0 } };
      const int lanes = 2 + (int)(den * 2.0f);
      for (int j = 0; j < lanes; j++) {
        const float P = 2.9f + j * 0.85f, tt = t + j * 1.37f;
        const uint32_t cyc = (uint32_t)(tt / P);
        const float p = tt - cyc * P;
        const float lx = 4.0f + h01(cyc, j, 91) * 16.0f, peak = 4.5f + h01(cyc, j, 92) * 5.5f;
        const uint8_t style = (uint8_t)(h01(cyc, j, 93) * 4);
        const Rgbw col = style == 2 ? Rgbw{ 255, 120, 10, 60 } : FW[(int)(h01(cyc, j, 94) * 7)];
        const float rise = 0.85f;
        if (p < rise) {                                // the rocket going up, with a short trail
          const float q = p / rise, e = 1.0f - (1.0f - q) * (1.0f - q);
          const float y = 23.0f - (23.0f - peak) * e, x = lx + (q - 0.5f) * 0.6f;
          cvGlow(cv, x, y, Rgbw{ 255, 150, 60, 80 }, 0.9f, 0.5f);
          for (int s = 1; s <= 3; s++) cvDot(cv, x, y + s * 0.9f, Rgbw{ 160, 60, 10, 10 }, 0.7f / s);
          continue;
        }
        const float dur = style == 2 ? 2.0f : 1.6f;
        const float q = (p - rise) / dur;
        if (q >= 1.0f) continue;
        if (q < 0.07f) cvGlow(cv, lx, peak, Rgbw{ 255, 230, 190, 255 }, 1.0f - q / 0.07f, 1.6f);   // the flash
        const int np = style == 1 ? 16 : 14;
        const float rmax = style == 1 ? 6.5f : (style == 2 ? 6.0f : 7.0f);
        const float grav = style == 2 ? 6.0f : 2.4f;
        const float rot = h01(cyc, j, 95) * 6.28f;
        for (int i = 0; i < np; i++) {
          const float a = rot + i * 6.2832f / np;
          const float sp = (style == 0 ? 0.75f + 0.25f * h01(cyc * 31 + i, j, 96) : 1.0f);
          for (int tr = 0; tr < 3; tr++) {               // the spark and two trail points behind it
            const float qq = max(0.0f, q - tr * 0.035f);
            const float rr = rmax * sp * (1.0f - powf(1.0f - qq, 2.4f));
            const float x = lx + cosf(a) * rr, y = peak + sinf(a) * rr * 0.92f + grav * qq * qq;
            float f = powf(1.0f - q, style == 2 ? 1.1f : 1.7f) * (tr == 0 ? 1.0f : 0.35f / tr);
            if (style == 3 && q > 0.45f) f *= h01(i, (uint32_t)(t * 20.0f), cyc) > 0.5f ? 1.6f : 0.0f;   // crackle
            const Rgbw cc = (style == 3 && q > 0.45f) ? Rgbw{ 60, 50, 40, 255 } : col;
            cvDot(cv, x, y, cc, f, false);
          }
        }
      }
      break;
    }
    case HW_VALENTINE:
    case HW_PADDY: {
      const bool gold = sc == HW_PADDY;
      for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) {          // sparkles
        const uint32_t slot = (uint32_t)(t * 3.0f + hashPx(x, y, 21) * 50.0f);
        if (h01(x + y * 131, slot, 22) < 0.010f + 0.025f * den) {
          const float f = 1.0f - fract1(t * 3.0f + hashPx(x, y, 21) * 50.0f);
          cvAt(cv, x, y, gold ? Rgbw{ 255, 140, 0, 60 } : Rgbw{ 0, 0, 0, 200 }, f, false);
        }
      }
      if (gold) {
        const int n = 3 + (int)(den * 8.0f);
        for (int i = 0; i < n; i++) {                  // gold glints falling
          const float y = fmodf(t * (1.6f + h01(i, 1, 12)) + h01(i, 2, 12) * 27.0f, 27.0f) - 1.5f;
          const float x = h01(i, 3, 12) * 24.0f;
          cvDot(cv, x, y, Rgbw{ 255, 150, 0, 40 }, 0.6f + 0.4f * sinf(t * 7.0f + i), false);
        }
      }
      break;
    }
    case HW_COASTAL:
    default: {
      const float hi = constrain((16.5f - k.sunY) / 10.0f, 0.0f, 1.0f);
      if (hi < 0.5f) for (int y = 0; y < cv.ht; y++) for (int x = 0; x < cv.wd; x++) {   // stars come out after sunset
        const float v = cv.dv(y);
        if (v > 9.0f || hashPx(x, y, 31) > 0.05f * den + 0.01f) continue;
        cvAt(cv, x, y, Rgbw{ 0, 0, 20, 110 }, (0.5f - hi) * 2.0f * (0.5f + 0.5f * sinf(t * 2.0f + hashPx(x, y, 32) * 6.28f)), false);
      }
      const Rgbw dark = { 0, 0, 0, 0 };
      const float cx = 5.6f + k.sway * 0.6f, cy = 9.6f;   // palm fronds swaying from the crown
      for (int f = 0; f < 6; f++) {
        const float a = -2.9f + f * 0.95f + k.sway * 0.08f;
        float px = cx, py = cy;
        for (int s = 1; s <= 5; s++) {
          const float nx = cx + cosf(a) * s * 1.05f, ny = cy + sinf(a) * s * 1.05f + 0.09f * s * s;
          cvLine(cv, px, py, nx, ny, dark, 1.0f);
          px = nx; py = ny;
        }
      }
      for (int b = 0; b < 2; b++) {                    // a pair of gulls gliding by
        const float x = fmodf(t * 1.1f + b * 2.2f, 34.0f) - 5.0f, y = 5.5f + b * 1.3f + sinf(t * 0.7f + b) * 0.6f;
        const float fl = 0.5f + 0.5f * sinf(t * 4.0f + b);
        cvLine(cv, x - 1.0f, y - 0.4f * fl, x, y, dark, 0.8f);
        cvLine(cv, x, y, x + 1.0f, y - 0.4f * fl, dark, 0.8f);
      }
      break;
    }
  }
  cvFlush(cv);
}
const char _data_holiday_window[] PROGMEM =
  "Holiday Window@Speed,Sparkle,Scene;;;2;sx=128,ix=128,c1=0";

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
    add(&mode_ucf_knights,       _data_ucf_knights);      // 238 (sound reactive)
    add(&mode_autumn_harvest,    _data_autumn_harvest);   // 239
    add(&mode_star_wars,         _data_star_wars);        // 240
    add(&mode_window_clock,      _data_window_clock);     // 241 (2D: the window matrix)
    add(&mode_holiday_window,    _data_holiday_window);   // 242 (2D: the window matrix)
  }

  void loop() override {}

  // Info page: shows which effect ids were assigned, to check presets against
  void addToJsonInfo(JsonObject &root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray arr = user.createNestedArray(F("Coastal FX"));
    if (firstId == 255) { arr.add(F("not loaded")); return; }
    char buf[40];
    snprintf_P(buf, sizeof(buf), PSTR("ids %u-%u, strips in step"), firstId, lastId);
    arr.add(buf);
  }
};

static CoastalFxUsermod coastal_fx;
REGISTER_USERMOD(coastal_fx);
