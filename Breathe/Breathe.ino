/*
 *  Breathe — a coherent-breathing timer for the M5Stack Core2
 *  Pattern: 5.5 s inhale / 5.5 s exhale (~5.5 breaths per minute)
 *
 *  Home      : pick session length (1 / 3 / 5 / 10 min or Open-ended),
 *              toggle Sound and Haptics (saved to flash), tap Begin.
 *  Countdown : 3-2-1 settle-in with soft ticks. Tap to cancel.
 *  Session   : glowing orb grows and shrinks with eased motion, a smooth
 *              progress ring sweeps each phase, breath counter and time
 *              remaining up top, session progress bar along the bottom.
 *              Soft directional tones (higher = inhale, lower = exhale)
 *              and a short haptic tap mean it works with eyes closed.
 *              Tap anywhere (or a touch button) to pause.
 *              Timed sessions finish at the end of a full breath — the
 *              app never cuts you off mid-exhale.
 *  Summary   : breaths completed + duration, gentle three-note chime.
 */

#include <M5Unified.h>
#include <Preferences.h>
#include <math.h>

// ---------------- palette (RGB565) ----------------
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
const uint16_t COL_BG     = rgb565(  6,  13,  22);  // deep navy
const uint16_t COL_INHALE = rgb565( 52, 211, 168);  // mint teal
const uint16_t COL_EXHALE = rgb565( 96, 132, 250);  // periwinkle
const uint16_t COL_TEXT   = rgb565(236, 242, 248);
const uint16_t COL_DIM    = rgb565(118, 134, 152);
const uint16_t COL_FAINT  = rgb565( 30,  44,  60);
const uint16_t COL_PANEL  = rgb565( 13,  24,  38);
const uint16_t COL_DARKTX = rgb565(  4,  28,  22);  // text on accent fills

uint16_t lerpCol(uint16_t a, uint16_t b, float t) {
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  int ar = a >> 11, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = b >> 11, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  return (uint16_t)(((int)(ar + (br - ar) * t) << 11) |
                    ((int)(ag + (bg - ag) * t) << 5)  |
                     (int)(ab + (bb - ab) * t));
}

// ---------------- breathing pattern ----------------
struct Phase { const char* name; float secs; bool grow; };
const Phase PHASES[] = { {"Inhale", 5.5f, true}, {"Exhale", 5.5f, false} };
const int N_PHASES = sizeof(PHASES) / sizeof(PHASES[0]);

// ---------------- session lengths ----------------
const int   DUR_MIN[]   = { 1, 3, 5, 10, 0 };       // 0 = open-ended
const char* DUR_LABEL[] = { "1", "3", "5", "10", "Open" };
const int   N_DUR  = 5;
const int   CHIP_W[N_DUR] = { 44, 44, 44, 44, 70 };

// ---------------- app state ----------------
enum Mode { M_HOME, M_COUNTDOWN, M_RUN, M_PAUSE, M_DONE };
Mode mode = M_HOME;

int      durIdx   = 2;        // default 5 min
bool     soundOn  = true;
bool     hapticOn = true;

int      phaseIdx    = 0;
uint32_t phaseStart  = 0;
uint32_t sessionStart = 0;
uint32_t sessionMs   = 0;     // 0 = open-ended
uint32_t pausedAt    = 0;
uint32_t countdownStart = 0;
int      cdLast      = -1;
int      breathCount = 0;
uint16_t curCol = COL_INHALE, prevCol = COL_EXHALE;

int      doneBreaths = 0;
uint32_t doneMs      = 0;

Preferences prefs;
M5Canvas canvas(&M5.Display);

// ---------------- layout ----------------
const int CX = 160, CY = 130;          // orb centre during a session
const int R_MIN = 40, R_MAX = 86, R_RING = 100;
const uint8_t BRIGHT_UI = 140, BRIGHT_RUN = 90;

struct Box {
  int x, y, w, h;
  bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
const Box BOX_BEGIN  {  84, 158, 152, 46 };
const Box BOX_POWER  {   0,   0,  60,  48 };   // top-left corner
const Box BOX_SOUND  {   0, 192,  70,  48 };   // bottom-left corner
const Box BOX_HAPTIC { 250, 192,  70,  48 };   // bottom-right corner
const Box BOX_RESUME {  76, 128,  98, 42 };
const Box BOX_END    { 186, 128,  60, 42 };

// Chip boxes are a plain array (filled in setup) rather than a function
// returning Box: the Arduino IDE hoists auto-generated prototypes above
// the struct definition, which breaks compilation.
Box CHIP_BOX[N_DUR];

void initChipBoxes() {
  int x = 21;
  for (int i = 0; i < N_DUR; ++i) {
    CHIP_BOX[i] = Box{ x, 98, CHIP_W[i], 40 };
    x += CHIP_W[i] + 8;
  }
}

// ---------------- non-blocking sound & haptics ----------------
struct ToneEv { uint32_t at; float freq; uint32_t dur; };
ToneEv toneQ[4];
int    toneQn = 0;

void queueTone(uint32_t delayMs, float freq, uint32_t dur) {
  if (toneQn < 4) toneQ[toneQn++] = { millis() + delayMs, freq, dur };
}
void serviceTones() {
  for (int i = 0; i < toneQn; ) {
    if ((int32_t)(millis() - toneQ[i].at) >= 0) {
      if (soundOn) M5.Speaker.tone(toneQ[i].freq, toneQ[i].dur);
      toneQ[i] = toneQ[--toneQn];
    } else ++i;
  }
}

uint32_t vibeOff = 0;
void buzz(uint32_t ms, uint8_t power = 130) {
  if (!hapticOn) return;
  M5.Power.setVibration(power);
  vibeOff = millis() + ms;
}
void serviceVibe() {
  if (vibeOff && (int32_t)(millis() - vibeOff) >= 0) {
    M5.Power.setVibration(0);
    vibeOff = 0;
  }
}

// ---------------- helpers ----------------
float easeInOutSine(float t) { return 0.5f * (1.0f - cosf(PI * t)); }

String fmtTime(uint32_t ms) {
  uint32_t s = (ms + 500) / 1000;
  return String(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + String(s % 60);
}

void savePrefs() {
  prefs.putInt("dur", durIdx);
  prefs.putBool("snd", soundOn);
  prefs.putBool("hap", hapticOn);
}

// Layered halo + lit core makes the orb feel luminous instead of flat.
void drawOrb(int ox, int oy, float r, uint16_t col) {
  for (int i = 5; i >= 1; --i) {
    uint16_t glow = lerpCol(COL_BG, col, 0.07f + 0.05f * (5 - i));
    canvas.fillCircle(ox, oy, (int)(r + i * 4), glow);
  }
  canvas.fillCircle(ox, oy, (int)r, col);
  canvas.fillCircle(ox - (int)(r * 0.10f), oy - (int)(r * 0.14f),
                    (int)(r * 0.60f), lerpCol(col, COL_TEXT, 0.16f));
  canvas.fillCircle(ox - (int)(r * 0.16f), oy - (int)(r * 0.22f),
                    (int)(r * 0.32f), lerpCol(col, COL_TEXT, 0.30f));
}

// ---------------- corner icons (drawn with primitives) ----------------
// Arc spans that cross 0 degrees are split in two, since wrap-around
// support varies between LovyanGFX versions.

void drawPowerIcon(int x, int y) {
  canvas.fillArc(x, y, 11, 9, 305, 360, COL_DIM);   // ring, gap at top
  canvas.fillArc(x, y, 11, 9, 0, 235, COL_DIM);
  canvas.fillRect(x - 1, y - 13, 3, 9, COL_DIM);    // stem through the gap
}

void drawSoundIcon(int x, int y, bool on) {
  uint16_t col = on ? lerpCol(COL_INHALE, COL_DIM, 0.45f) : COL_FAINT;
  canvas.fillRect(x - 11, y - 4, 5, 9, col);                       // driver box
  canvas.fillTriangle(x - 8, y, x - 1, y - 8, x - 1, y + 8, col);  // cone
  if (on) {
    canvas.fillArc(x + 2, y, 6, 5, 305, 360, col);   // inner wave
    canvas.fillArc(x + 2, y, 6, 5, 0, 55, col);
    canvas.fillArc(x + 2, y, 11, 10, 305, 360, col); // outer wave
    canvas.fillArc(x + 2, y, 11, 10, 0, 55, col);
  } else {
    canvas.drawLine(x - 12, y + 11, x + 12, y - 11, COL_DIM);
    canvas.drawLine(x - 11, y + 11, x + 13, y - 11, COL_DIM);
  }
}

void drawHapticIcon(int x, int y, bool on) {
  uint16_t col = on ? lerpCol(COL_INHALE, COL_DIM, 0.45f) : COL_FAINT;
  canvas.drawRoundRect(x - 5, y - 9, 11, 19, 3, col);  // phone body, 2px stroke
  canvas.drawRoundRect(x - 4, y - 8, 9, 17, 2, col);
  if (on) {
    canvas.fillArc(x, y, 13, 12, 125, 235, col);       // left wave
    canvas.fillArc(x, y, 13, 12, 305, 360, col);       // right wave
    canvas.fillArc(x, y, 13, 12, 0, 55, col);
  } else {
    canvas.drawLine(x - 14, y + 11, x + 12, y - 11, COL_DIM);
    canvas.drawLine(x - 13, y + 11, x + 13, y - 11, COL_DIM);
  }
}

// ---------------- screens ----------------
void drawHome() {
  canvas.fillScreen(COL_BG);

  canvas.setTextDatum(top_right);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_DIM);
  canvas.drawString(String(M5.Power.getBatteryLevel()) + "%", 310, 8);

  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString("Breathe", 160, 46);

  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("coherent breathing - 5.5s in / 5.5s out", 160, 78);

  for (int i = 0; i < N_DUR; ++i) {
    const Box& b = CHIP_BOX[i];
    bool sel = (i == durIdx);
    if (sel) canvas.fillRoundRect(b.x, b.y, b.w, b.h, 10, COL_INHALE);
    else     canvas.drawRoundRect(b.x, b.y, b.w, b.h, 10, COL_FAINT);
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextColor(sel ? COL_DARKTX : COL_DIM);
    canvas.drawString(DUR_LABEL[i], b.x + b.w / 2, b.y + b.h / 2 - 1);
  }
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_FAINT);
  canvas.drawString("minutes", 160, 148);

  canvas.fillRoundRect(BOX_BEGIN.x, BOX_BEGIN.y, BOX_BEGIN.w, BOX_BEGIN.h, 23, COL_INHALE);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(COL_DARKTX);
  canvas.drawString("Begin", 160, BOX_BEGIN.y + BOX_BEGIN.h / 2 - 1);

  drawPowerIcon(22, 20);
  drawSoundIcon(24, 216, soundOn);
  drawHapticIcon(296, 216, hapticOn);

  canvas.pushSprite(0, 0);
}

void drawCountdown() {
  uint32_t el = millis() - countdownStart;
  int num = 3 - (int)(el / 1000);
  if (num != cdLast) {
    cdLast = num;
    if (soundOn) M5.Speaker.tone(523, 35);
  }

  canvas.fillScreen(COL_BG);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_DIM);
  canvas.drawString("get ready", 160, 44);

  float r = 46.0f + 5.0f * sinf(el / 600.0f);
  drawOrb(CX, CY, r, lerpCol(COL_INHALE, COL_BG, 0.35f));

  canvas.setFont(&fonts::FreeSansBold24pt7b);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString(String(num), CX, CY);

  canvas.pushSprite(0, 0);
}

// One frame of the breathing scene; `el` is ms elapsed in the current phase.
void drawRunFrame(uint32_t el, bool dimmed) {
  const Phase& ph = PHASES[phaseIdx];
  uint32_t phMs = (uint32_t)(ph.secs * 1000.0f);
  float prog  = (float)el / phMs;
  float eased = easeInOutSine(prog);
  float r = ph.grow ? R_MIN + (R_MAX - R_MIN) * eased
                    : R_MAX - (R_MAX - R_MIN) * eased;

  // colour crossfades over the first 650 ms of each phase
  uint16_t col = lerpCol(prevCol, curCol, el / 650.0f);
  if (dimmed) col = lerpCol(col, COL_BG, 0.55f);

  canvas.fillScreen(COL_BG);

  drawOrb(CX, CY, r, col);

  // Ring "fills" like lungs: anchored at the bottom, it grows up both
  // sides and closes at the top on inhale, then splits at the top and
  // drains back down on exhale. Eased with the orb so motion is
  // continuous across phase changes. Arcs that would cross 0 degrees
  // are split for LovyanGFX compatibility.
  canvas.fillArc(CX, CY, R_RING + 1, R_RING - 1, 0, 360, COL_FAINT);
  float f = ph.grow ? eased : 1.0f - eased;   // fraction of ring present
  if (f > 0.004f && !dimmed) {
    uint16_t ringCol = lerpCol(col, COL_BG, 0.2f);
    float half = 180.0f * f;
    canvas.fillArc(CX, CY, R_RING + 3, R_RING - 3, 90.0f, 90.0f + half, ringCol);
    float a0 = 90.0f - half;
    if (a0 >= 0.0f) {
      canvas.fillArc(CX, CY, R_RING + 3, R_RING - 3, a0, 90.0f, ringCol);
    } else {
      canvas.fillArc(CX, CY, R_RING + 3, R_RING - 3, 0.0f, 90.0f, ringCol);
      canvas.fillArc(CX, CY, R_RING + 3, R_RING - 3, 360.0f + a0, 360.0f, ringCol);
    }
    if (f < 0.996f) {   // bright dots where the ring is growing/receding
      float rad = a0 * DEG_TO_RAD;
      canvas.fillCircle(CX + (int)(cosf(rad) * R_RING),
                        CY + (int)(sinf(rad) * R_RING), 4, col);
      rad = (90.0f + half) * DEG_TO_RAD;
      canvas.fillCircle(CX + (int)(cosf(rad) * R_RING),
                        CY + (int)(sinf(rad) * R_RING), 4, col);
    }
  }

  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString(ph.name, CX, CY);

  // time remaining (or elapsed when open-ended), small and dim
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_DIM);
  canvas.setTextDatum(top_right);
  uint32_t now = dimmed ? pausedAt : millis();
  uint32_t sel = now - sessionStart;
  canvas.drawString(fmtTime(sessionMs ? (sel >= sessionMs ? 0 : sessionMs - sel) : sel), 310, 8);
}

void drawPause() {
  drawRunFrame(pausedAt - phaseStart, true);

  canvas.fillRoundRect(60, 58, 200, 130, 14, COL_PANEL);
  canvas.drawRoundRect(60, 58, 200, 130, 14, COL_FAINT);
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString("Paused", 160, 90);

  canvas.fillRoundRect(BOX_RESUME.x, BOX_RESUME.y, BOX_RESUME.w, BOX_RESUME.h, 21, COL_INHALE);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(COL_DARKTX);
  canvas.drawString("Resume", BOX_RESUME.x + BOX_RESUME.w / 2, BOX_RESUME.y + BOX_RESUME.h / 2);

  canvas.drawRoundRect(BOX_END.x, BOX_END.y, BOX_END.w, BOX_END.h, 21, COL_DIM);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString("End", BOX_END.x + BOX_END.w / 2, BOX_END.y + BOX_END.h / 2);

  canvas.pushSprite(0, 0);
}

void drawDone() {
  canvas.fillScreen(COL_BG);
  drawOrb(160, 92, 38, COL_INHALE);
  for (int o = -1; o <= 1; ++o) {       // check mark
    canvas.drawLine(146, 92 + o, 156, 103 + o, COL_DARKTX);
    canvas.drawLine(156, 103 + o, 177, 81 + o, COL_DARKTX);
  }
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(COL_TEXT);
  canvas.drawString("Well done", 160, 158);
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.setTextColor(COL_DIM);
  canvas.drawString(String(doneBreaths) + " breaths in " + fmtTime(doneMs), 160, 186);
  canvas.setTextColor(COL_FAINT);
  canvas.drawString("tap anywhere to finish", 160, 214);
  canvas.pushSprite(0, 0);
}

// ---------------- transitions ----------------
void goHome() {
  mode = M_HOME;
  M5.Display.setBrightness(BRIGHT_UI);
}

void startCountdown() {
  mode = M_COUNTDOWN;
  countdownStart = millis();
  cdLast = -1;
  buzz(30);
}

void startSession() {
  mode = M_RUN;
  M5.Display.setBrightness(BRIGHT_RUN);
  phaseIdx = 0;
  breathCount = 0;
  sessionStart = phaseStart = millis();
  sessionMs = (uint32_t)DUR_MIN[durIdx] * 60000UL;
  prevCol = COL_EXHALE;
  curCol  = COL_INHALE;
  queueTone(0, 587, 140);
  buzz(45);
}

void pauseSession() {
  pausedAt = millis();
  mode = M_PAUSE;
  M5.Display.setBrightness(BRIGHT_UI);
  buzz(25);
}

void resumeSession() {
  uint32_t d = millis() - pausedAt;
  phaseStart   += d;
  sessionStart += d;
  mode = M_RUN;
  M5.Display.setBrightness(BRIGHT_RUN);
  buzz(25);
}

void finishSession() {
  uint32_t endRef = (mode == M_PAUSE) ? pausedAt : millis();
  doneBreaths = breathCount;
  doneMs = endRef - sessionStart;
  mode = M_DONE;
  M5.Display.setBrightness(BRIGHT_UI);
  queueTone(0, 523, 150);
  queueTone(180, 659, 150);
  queueTone(360, 784, 300);
  buzz(220);
}

void tickRun() {
  uint32_t phMs = (uint32_t)(PHASES[phaseIdx].secs * 1000.0f);
  uint32_t el = millis() - phaseStart;

  if (el >= phMs) {
    phaseStart += phMs;          // drift-free phase timing
    el -= phMs;
    if (phaseIdx == N_PHASES - 1) {
      breathCount++;
      // timed sessions end on a completed breath, never mid-phase
      if (sessionMs && millis() - sessionStart >= sessionMs) {
        finishSession();
        return;
      }
    }
    phaseIdx = (phaseIdx + 1) % N_PHASES;
    prevCol = curCol;
    curCol = PHASES[phaseIdx].grow ? COL_INHALE : COL_EXHALE;
    queueTone(0, PHASES[phaseIdx].grow ? 587 : 392, PHASES[phaseIdx].grow ? 140 : 180);
    buzz(45);
  }

  drawRunFrame(el, false);
  canvas.pushSprite(0, 0);
}

// ---------------- touch routing ----------------
void homeTouch(int x, int y) {
  for (int i = 0; i < N_DUR; ++i) {
    if (CHIP_BOX[i].hit(x, y)) {
      durIdx = i;
      savePrefs();
      if (soundOn) M5.Speaker.tone(660, 30);
      buzz(20);
      return;
    }
  }
  if (BOX_BEGIN.hit(x, y))  { startCountdown(); return; }
  if (BOX_SOUND.hit(x, y))  { soundOn = !soundOn;  savePrefs(); if (soundOn) M5.Speaker.tone(660, 40); return; }
  if (BOX_HAPTIC.hit(x, y)) { hapticOn = !hapticOn; savePrefs(); buzz(40); return; }
  if (BOX_POWER.hit(x, y)) {
    canvas.fillScreen(COL_BG);
    canvas.setTextDatum(middle_center);
    canvas.setFont(&fonts::FreeSans9pt7b);
    canvas.setTextColor(COL_DIM);
    canvas.drawString("see you", 160, 120);
    canvas.pushSprite(0, 0);
    delay(450);
    M5.Power.powerOff();
    return;
  }
}

// ---------------- arduino entry points ----------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(BRIGHT_UI);
  M5.Speaker.setVolume(100);

  initChipBoxes();

  prefs.begin("breathe", false);
  durIdx   = prefs.getInt("dur", 2);
  if (durIdx < 0 || durIdx >= N_DUR) durIdx = 2;
  soundOn  = prefs.getBool("snd", true);
  hapticOn = prefs.getBool("hap", true);

  canvas.setColorDepth(16);
  if (!canvas.createSprite(320, 240)) {   // fall back to PSRAM if heap is tight
    canvas.setPsram(true);
    canvas.createSprite(320, 240);
  }
}

void loop() {
  M5.update();
  serviceTones();
  serviceVibe();

  auto t = M5.Touch.getDetail();
  bool tap = t.wasPressed();
  int tx = t.x, ty = t.y;
  bool btn = M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnC.wasPressed();

  switch (mode) {
    case M_HOME:
      if (tap) homeTouch(tx, ty);
      drawHome();
      break;

    case M_COUNTDOWN:
      if (tap || btn) { goHome(); break; }
      if (millis() - countdownStart >= 3000) { startSession(); break; }
      drawCountdown();
      break;

    case M_RUN:
      if (tap || btn) { pauseSession(); break; }
      tickRun();
      break;

    case M_PAUSE:
      if (btn) { resumeSession(); break; }
      if (tap) {
        if      (BOX_RESUME.hit(tx, ty)) { resumeSession(); break; }
        else if (BOX_END.hit(tx, ty))    { finishSession();  break; }
      }
      drawPause();
      break;

    case M_DONE:
      if (tap || btn) { goHome(); break; }
      drawDone();
      break;
  }

  // pace frames; in practice the display push is the limiter (~30 fps)
  static uint32_t nextFrame = 0;
  uint32_t now = millis();
  if (now < nextFrame) delay(nextFrame - now);
  nextFrame = millis() + 25;
}
