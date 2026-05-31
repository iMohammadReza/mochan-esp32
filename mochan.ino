#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

/* ================= OLED ================= */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_SDA 8
#define OLED_SCL 9

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
RoboEyes<Adafruit_SSD1306> roboEyes(display);

/* ================= MOTOR PIN ================= */
#define LF   1
#define LB   0
#define RF   2
#define RB   3
#define STBY 10

enum MotorDir {
  DIR_STOP,
  DIR_FWD,
  DIR_BACK,
  DIR_LEFT,
  DIR_RIGHT,
  DIR_WHEEL_LB,
  DIR_WHEEL_RB,
  DIR_WHEEL_LF,
  DIR_WHEEL_RF,
  MOTOR_DIR_COUNT
};

enum RandomMode {
  RANDOM_OFF,
  RANDOM_SOFT,
  RANDOM_NORMAL
};

struct SoundEffect {
  const int* notes;
  const int* durations;
  int len;
};

struct MelodyState {
  const int* notes;
  const int* durations;
  int len;
  int idx;
  unsigned long noteStart;
  bool playing;
};

/* Lookup: for each direction, pin states { LF, LB, RF, RB }. Unifies motorWifi and MOTOR. */
static const uint8_t MOTOR_LOOKUP[][4] = {
  { 0, 0, 0, 0 },  /* DIR_STOP */
  { 1, 0, 0, 1 },  /* DIR_FWD (was motorWifi 1) */
  { 0, 1, 1, 0 },  /* DIR_BACK (was motorWifi 2) */
  { 0, 1, 0, 1 },  /* DIR_LEFT (was motorWifi 3) */
  { 1, 0, 1, 0 },  /* DIR_RIGHT (was motorWifi 4) */
  { 0, 1, 0, 0 },  /* DIR_WHEEL_LB (was MOTOR 5) */
  { 0, 0, 0, 1 },  /* DIR_WHEEL_RB (was MOTOR 6) */
  { 1, 0, 0, 0 },  /* DIR_WHEEL_LF (was MOTOR 7) */
  { 0, 0, 1, 0 },  /* DIR_WHEEL_RF (was MOTOR 8) */
};

void setMotors(MotorDir dir) {
  if ((unsigned)dir >= MOTOR_DIR_COUNT) dir = DIR_STOP;
  digitalWrite(STBY, HIGH);
  digitalWrite(LF, MOTOR_LOOKUP[dir][0] ? HIGH : LOW);
  digitalWrite(LB, MOTOR_LOOKUP[dir][1] ? HIGH : LOW);
  digitalWrite(RF, MOTOR_LOOKUP[dir][2] ? HIGH : LOW);
  digitalWrite(RB, MOTOR_LOOKUP[dir][3] ? HIGH : LOW);
}

void pulseMotor(MotorDir dir, int onMs, int offMs, int count) {
  for (int i = 0; i < count; i++) {
    setMotors(dir);
    delay(onMs);
    setMotors(DIR_STOP);
    delay(offMs);
  }
}

/* ================= BUZZER ================= */
#define BUZZER_PIN 5
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ================= CLOCK BUTTON ================= */
/* WARNING: GPIOs 0,1,2,3 are motor pins and 5,8,9,10 are taken. Use a free GPIO.
   Wire a button between CLOCK_BTN_PIN and GND. The internal pull-up is enabled. */
#define CLOCK_BTN_PIN 4
#define CLOCK_BTN_PRESSED LOW
int lastClockBtnState = -1;    /* -1 = not yet read, so first edge is ignored for debounce */
unsigned long lastClockBtnMillis = 0;
#define CLOCK_BTN_DEBOUNCE_MS 250

/* ================= WIFI ================= */
WebServer server(80);
DNSServer dnsServer;

/* ================= STATE ================= */
bool manualActive = false;

/* ================= RANDOM MODE ================= */
RandomMode randomMode = RANDOM_NORMAL;

/* ================= EXPRESSION TOGGLES ================= */
bool sweatOn = false;
bool curiousOn = false;

/* ================= MUTE ================= */
bool mute = false;

/* ================= CLOCK MODE ================= */
struct ClockState {
  bool active;
  int hour;
  int minute;
  int second;
  unsigned long lastTick;
  unsigned long lastDraw;
};
ClockState clockState = { false, 12, 0, 0, 0, 0 };

void drawClock() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  char buf[16];
  display.setTextSize(3);
  snprintf(buf, sizeof(buf), "%02d:%02d", clockState.hour, clockState.minute);
  int charW = 18;
  int strW = 5 * charW;
  display.setCursor((SCREEN_WIDTH - strW) / 2, (SCREEN_HEIGHT - 24) / 2 - 4);
  display.print(buf);
  display.setTextSize(1);
  snprintf(buf, sizeof(buf), "%02d", clockState.second);
  display.setCursor((SCREEN_WIDTH - 12) / 2, SCREEN_HEIGHT - 12);
  display.print(buf);
  display.display();
}

void updateClockSeconds() {
  unsigned long now = millis();
  if (now - clockState.lastTick >= 1000) {
    clockState.lastTick = now;
    clockState.second++;
    if (clockState.second >= 60) { clockState.second = 0; clockState.minute++; }
    if (clockState.minute >= 60) { clockState.minute = 0; clockState.hour++; }
    if (clockState.hour >= 24) clockState.hour = 0;
  }
}

void enterClockMode() {
  buzzerSilence();
  setMotors(DIR_STOP);
  clockState.lastTick = millis();
  clockState.lastDraw = millis();
  drawClock();
}

void exitClockMode() {
  setMotors(DIR_STOP);
}

/* ================= BUZZER / MELODY STATE ================= */
MelodyState melodyState = { nullptr, nullptr, 0, 0, 0, false };

void playSound(const SoundEffect& se) {
  playMelody(se.notes, se.durations, se.len);
}

void buzzerSilence() {
  ledcWriteTone(BUZZER_PIN, 0);
  melodyState.playing = false;
  melodyState.notes = nullptr;
  melodyState.durations = nullptr;
}

void playMelody(const int* notes, const int* durations, int len) {
  if (len <= 0 || mute || clockState.active) return;
  melodyState.notes = notes;
  melodyState.durations = durations;
  melodyState.len = len;
  melodyState.idx = 0;
  melodyState.noteStart = millis();
  melodyState.playing = true;
  int f = notes[0];
  ledcWriteTone(BUZZER_PIN, f > 0 ? (unsigned int)f : 0);
}

void updateMelody() {
  if (!melodyState.playing || melodyState.notes == nullptr || melodyState.durations == nullptr) return;
  unsigned long elapsed = millis() - melodyState.noteStart;
  int dur = melodyState.durations[melodyState.idx];
  if (elapsed >= (unsigned long)dur) {
    melodyState.idx++;
    if (melodyState.idx >= melodyState.len) {
      ledcWriteTone(BUZZER_PIN, 0);
      melodyState.playing = false;
      return;
    }
    int f = melodyState.notes[melodyState.idx];
    ledcWriteTone(BUZZER_PIN, f > 0 ? (unsigned int)f : 0);
    melodyState.noteStart = millis();
  }
}

/* Sound effects as short melodies. playMelody() already guards mute/clockState.active. */
static const int SE_CHIRP_N[] = { 880, 1100 };
static const int SE_CHIRP_D[] = { 80, 80 };
static const SoundEffect SE_CHIRP = { SE_CHIRP_N, SE_CHIRP_D, (int)ARRAY_LEN(SE_CHIRP_N) };

static const int SE_SAD_N[] = { 392 };
static const int SE_SAD_D[] = { 200 };
static const SoundEffect SE_SAD = { SE_SAD_N, SE_SAD_D, (int)ARRAY_LEN(SE_SAD_N) };

static const int SE_ALERT_N[] = { 660 };
static const int SE_ALERT_D[] = { 100 };
static const SoundEffect SE_ALERT = { SE_ALERT_N, SE_ALERT_D, (int)ARRAY_LEN(SE_ALERT_N) };

static const int SE_PURR_N[] = { 200, 0, 150, 0, 200, 0, 150, 0, 200, 0, 150 };
static const int SE_PURR_D[] = { 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30 };
static const SoundEffect SE_PURR = { SE_PURR_N, SE_PURR_D, (int)ARRAY_LEN(SE_PURR_N) };

static const int SE_GIGGLE_N[] = { 523, 0, 659, 0, 784 };
static const int SE_GIGGLE_D[] = { 60, 40, 60, 40, 80 };
static const SoundEffect SE_GIGGLE = { SE_GIGGLE_N, SE_GIGGLE_D, (int)ARRAY_LEN(SE_GIGGLE_N) };

static const int SE_ACTION_N[] = { 600 };
static const int SE_ACTION_D[] = { 40 };
static const SoundEffect SE_ACTION = { SE_ACTION_N, SE_ACTION_D, (int)ARRAY_LEN(SE_ACTION_N) };

static const int SE_MOOD_DEFAULT_N[] = { 440 };
static const int SE_MOOD_DEFAULT_D[] = { 60 };
static const SoundEffect SE_MOOD_DEFAULT = { SE_MOOD_DEFAULT_N, SE_MOOD_DEFAULT_D, (int)ARRAY_LEN(SE_MOOD_DEFAULT_N) };
static const int SE_MOOD_TIRED_N[] = { 200 };
static const int SE_MOOD_TIRED_D[] = { 300 };
static const SoundEffect SE_MOOD_TIRED = { SE_MOOD_TIRED_N, SE_MOOD_TIRED_D, (int)ARRAY_LEN(SE_MOOD_TIRED_N) };
static const int SE_MOOD_ANGRY_N[] = { 400, 0, 400 };
static const int SE_MOOD_ANGRY_D[] = { 80, 60, 80 };
static const SoundEffect SE_MOOD_ANGRY = { SE_MOOD_ANGRY_N, SE_MOOD_ANGRY_D, (int)ARRAY_LEN(SE_MOOD_ANGRY_N) };

void soundChirp() { playSound(SE_CHIRP); }
void soundSad() { playSound(SE_SAD); }
void soundAlert() { playSound(SE_ALERT); }
void soundPurr() { playSound(SE_PURR); }
void soundGiggle() { playSound(SE_GIGGLE); }
void soundActionBeep() { playSound(SE_ACTION); }
void soundMoodDefault() { playSound(SE_MOOD_DEFAULT); }
void soundMoodTired() { playSound(SE_MOOD_TIRED); }
void soundMoodAngry() { playSound(SE_MOOD_ANGRY); }
void soundMoodHappy() { soundChirp(); }

/* Melodies (freq, duration ms). 0 = rest. */
static const int MELODY_STARTUP[] = { 262, 330, 392, 523, 0 };
static const int MELODY_STARTUP_DUR[] = { 80, 80, 80, 200, 50 };

static const int MELODY_MARIO_COIN[] = { 988, 1319, 0 };
static const int MELODY_MARIO_COIN_DUR[] = { 100, 200, 50 };

static const int MELODY_NOKIA[] = { 659, 587, 370, 392, 523, 494, 392, 523, 587, 784, 659, 698, 587, 0 };
static const int MELODY_NOKIA_DUR[] = { 150, 150, 150, 150, 150, 150, 300, 150, 150, 300, 150, 150, 300, 50 };

static const int MELODY_STARWARS[] = { 392, 392, 392, 311, 523, 392, 311, 523, 392, 0 };
static const int MELODY_STARWARS_DUR[] = { 350, 350, 350, 250, 100, 350, 250, 100, 700, 50 };

/* ================= DANCE STATE ================= */
struct DanceSequence {
  const byte* cmds;
  const unsigned int* durations;
  int len;
};

static const byte DANCE_WIGGLE_CMD[] = { 3, 0, 4, 0, 3, 0, 4, 0, 3, 0, 4, 0 };
static const unsigned int DANCE_WIGGLE_DUR[] = { 180, 80, 180, 80, 180, 80, 180, 80, 180, 80, 180, 80 };

static const byte DANCE_SPIN_CMD[] = { 3, 0, 3, 0, 3, 0, 3, 0, 3, 0, 3, 0, 3, 0, 3, 0 };
static const unsigned int DANCE_SPIN_DUR[] = { 120, 40, 120, 40, 120, 40, 120, 40, 120, 40, 120, 40, 120, 40, 120, 40 };

static const byte DANCE_HAPPY_CMD[] = { 1, 0, 2, 0, 1, 0, 2, 0, 1, 0, 2, 0 };
static const unsigned int DANCE_HAPPY_DUR[] = { 200, 100, 200, 100, 200, 100, 200, 100, 200, 100, 200, 100 };

static const DanceSequence DANCE_SEQUENCES[] = {
  { DANCE_WIGGLE_CMD, DANCE_WIGGLE_DUR, (int)ARRAY_LEN(DANCE_WIGGLE_CMD) },
  { DANCE_SPIN_CMD,   DANCE_SPIN_DUR,   (int)ARRAY_LEN(DANCE_SPIN_CMD) },
  { DANCE_HAPPY_CMD,  DANCE_HAPPY_DUR,  (int)ARRAY_LEN(DANCE_HAPPY_CMD) },
};
#define DANCE_TYPE_COUNT (int)ARRAY_LEN(DANCE_SEQUENCES)

bool danceActive = false;
int danceType = 0;           /* 0=wiggle, 1=spin, 2=happy */
int danceStepIndex = 0;
unsigned long danceStepStart = 0;

/* ================= PERSONALITY STATE ================= */
bool personalityOn = false;
#define PERSONALITY_PERIOD_MS (5 * 60 * 1000)  /* 5 minutes */
unsigned long lastPersonalitySound = 0;
int lastPersonalityPhase = -1;

void updatePersonality() {
  if (!personalityOn) return;
  unsigned long t = millis() % PERSONALITY_PERIOD_MS;
  int phase = (int)(t / (PERSONALITY_PERIOD_MS / 4));  /* 0..3 */
  if (phase > 3) phase = 3;

  if (phase != lastPersonalityPhase) {
    lastPersonalityPhase = phase;
    if (phase == 0) {
      randomMode = RANDOM_NORMAL;
      roboEyes.setMood(HAPPY);
      roboEyes.setCuriosity(OFF);
    } else if (phase == 1) {
      randomMode = RANDOM_NORMAL;
      roboEyes.setMood(DEFAULT);
      roboEyes.setCuriosity(ON);
    } else if (phase == 2) {
      randomMode = RANDOM_SOFT;
      roboEyes.setMood(DEFAULT);
      roboEyes.setCuriosity(OFF);
    } else {
      randomMode = (t < PERSONALITY_PERIOD_MS * 95 / 100) ? RANDOM_SOFT : RANDOM_OFF;
      roboEyes.setMood(TIRED);
      roboEyes.setCuriosity(OFF);
    }
  }

  /* Occasional sounds per phase */
  if (millis() - lastPersonalitySound < 15000) return;
  if (phase == 0 && random(20) == 0) {
    soundChirp();
    lastPersonalitySound = millis();
  } else if (phase == 1 && random(25) == 0) {
    roboEyes.blink(random(2) == 0, random(2) == 1);
    soundGiggle();
    lastPersonalitySound = millis();
  } else if (phase == 3 && random(30) == 0) {
    static const int YAWN_N[] = { 150 };
    static const int YAWN_D[] = { 800 };
    playMelody(YAWN_N, YAWN_D, (int)ARRAY_LEN(YAWN_N));
    lastPersonalitySound = millis();
  }
}

static void startDanceIntro(int type) {
  if (type == 0) {
    roboEyes.setMood(HAPPY);
    soundGiggle();
  } else if (type == 1) {
    roboEyes.anim_confused();
    playMelody(MELODY_MARIO_COIN, MELODY_MARIO_COIN_DUR, (int)ARRAY_LEN(MELODY_MARIO_COIN));
  } else {
    roboEyes.anim_laugh();
    soundChirp();
  }
}

void startDance(int type) {
  if (danceActive || clockState.active) return;
  if (type < 0 || type >= DANCE_TYPE_COUNT) return;
  const DanceSequence& seq = DANCE_SEQUENCES[type];
  danceType = type;
  danceStepIndex = 0;
  danceStepStart = millis();
  danceActive = true;
  manualActive = true;
  startDanceIntro(type);
  setMotors((MotorDir)seq.cmds[0]);
}

void updateDance() {
  if (!danceActive) return;
  const DanceSequence& seq = DANCE_SEQUENCES[danceType];
  unsigned long elapsed = millis() - danceStepStart;
  if (elapsed >= (unsigned long)seq.durations[danceStepIndex]) {
    danceStepIndex++;
    if (danceStepIndex >= seq.len) {
      danceActive = false;
      manualActive = false;
      setMotors(DIR_STOP);
      roboEyes.setMood(DEFAULT);
      return;
    }
    danceStepStart = millis();
    setMotors((MotorDir)seq.cmds[danceStepIndex]);
  }
}

/* ================= DICE ROLL GAME ================= */
struct DiceState {
  int result;        /* 0 = not rolled, 1-6 = last result */
  bool animating;
  unsigned long animStart;
};
DiceState diceState = { 0, false, 0 };

static const int DICE_ROLL_N[] = { 400, 500, 600, 700 };
static const int DICE_ROLL_D[] = { 80, 80, 80, 120 };

void diceRoll() {
  if (clockState.active || diceState.animating) return;
  diceState.result = 1 + (random(6));
  diceState.animating = true;
  diceState.animStart = millis();
  playMelody(DICE_ROLL_N, DICE_ROLL_D, (int)ARRAY_LEN(DICE_ROLL_N));
}

void drawDiceFace(int value) {
  display.clearDisplay();
  display.setTextSize(4);
  display.setTextColor(SSD1306_WHITE);
  char buf[2];
  buf[0] = '0' + value;
  buf[1] = '\0';
  int w = 24;
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - 32) / 2);
  display.print(buf);
  display.display();
}

void updateDice() {
  if (!diceState.animating) return;
  unsigned long elapsed = millis() - diceState.animStart;
  if (elapsed < 1500) {
    int cycle = (elapsed / 100) % 6;
    drawDiceFace(cycle + 1);
  } else if (elapsed < 2500) {
    drawDiceFace(diceState.result);
  } else {
    diceState.animating = false;
    soundChirp();
  }
}

/* ================= WEB UI ================= */
void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover, user-scalable=yes, maximum-scale=5, minimum-scale=1">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<meta name="format-detection" content="telephone=no">
<style>
html{ -webkit-text-size-adjust:100%; text-size-adjust:100%; }
body{
  margin:0;
  min-height:100vh;
  min-height:-webkit-fill-available;
  padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left);
  box-sizing:border-box;
  background:#0a0a0a;
  color:#00ff41;
  font-family:"SF Mono",Consolas,Monaco,"Courier New",monospace;
  display:flex;align-items:center;justify-content:center;
  -webkit-tap-highlight-color:transparent;
}
.panel{
  margin: 16px;
  padding:16px;
  padding-left:max(16px, env(safe-area-inset-left));
  padding-right:max(16px, env(safe-area-inset-right));
  border-radius:0;
  background:#111;
  border:1px solid #00ff41;
  box-shadow:0 0 20px rgba(0,255,65,0.2);
  box-sizing:border-box;
}
h2{text-align:center;margin:0 0 12px;letter-spacing:2px;font-size:1rem;}
.grid{
  display:grid;
  grid-template-columns:1fr 1fr 1fr;
  grid-template-rows:52px 52px 52px;
  gap:8px;
}
button{
  border:1px solid #00ff41;
  border-radius:0;
  font-size:14px;
  font-weight:bold;
  font-family:inherit;
  background:#0a0a0a;
  color:#00ff41;
  min-height:44px;
  touch-action:manipulation;
}
button:active{background:#003300;}
.stop{border-color:#ff4444;color:#ff4444;}
.stop:active{background:#330000;}
.empty{background:transparent;border:none;}

.mode{
  margin-top:10px;
  display:flex;
  flex-wrap:wrap;
  gap:6px;
}
.mode button{
  flex:1;
  min-width:52px;
  font-size:11px;
  opacity:0.85;
}
.mode button.active{
  opacity:1;
  background:#003300;
  box-shadow:0 0 8px rgba(0,255,65,0.5);
}
.mood-btns{display:flex;flex-wrap:wrap;gap:6px;}
.mood-btns button{flex:1;min-width:52px;font-size:11px;}
.mood-toggles{display:flex;flex-wrap:wrap;gap:8px;margin-top:6px;align-items:center;}
.mood-toggles .section-inline{font-size:10px;opacity:0.8;margin-right:4px;}
.mood-toggles button{
  min-width:72px;
  font-size:11px;
  border-style:dashed;
  border-width:1px;
}
.mood-toggles button.active{border-style:solid;box-shadow:0 0 6px rgba(0,255,65,0.4);}
.mood-expr-row{flex-wrap:wrap;}

.section{font-size:10px;margin-top:8px;margin-bottom:4px;opacity:0.9;}
.footer{margin-top:12px;text-align:center;font-size:10px;opacity:0.7;}
.game-panel{display:none;margin-top:10px;padding:10px;border-radius:0;background:#0a0a0a;border:1px solid #00ff41;}
.game-panel.show{display:block;}
.game-score{margin-bottom:8px;font-size:13px;}
.game-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;}
</style>
</head>
<body>

<div class="panel">
<h2>MOCHAN ROBOT</h2>

<div class="grid">
  <div class="empty"></div>
  <button onclick="fetch('/f')">UP</button>
  <div class="empty"></div>
  <button onclick="fetch('/l')">LEFT</button>
  <button class="stop" onclick="fetch('/s')">STOP</button>
  <button onclick="fetch('/r')">RIGHT</button>
  <div class="empty"></div>
  <button onclick="fetch('/b')">DOWN</button>
  <div class="empty"></div>
</div>

<div class="section">Mode</div>
<div class="mode">
  <button id="btn_sleep" onclick="setMode('off')">SLEEP</button>
  <button id="btn_wiggle" onclick="setMode('soft')">WIGGLE</button>
  <button id="btn_curious" class="active" onclick="setMode('normal')">CURIOUS</button>
</div>

<div class="section">Mood &amp; expressions</div>
<div class="mood-btns">
  <button id="btn_mood_default" class="active" onclick="setMood('default')">Default</button>
  <button id="btn_mood_tired" onclick="setMood('tired')">Tired</button>
  <button id="btn_mood_angry" onclick="setMood('angry')">Angry</button>
  <button id="btn_mood_happy" onclick="setMood('happy')">Happy</button>
  <button onclick="fetch('/expr_confused')">Confused</button>
  <button onclick="fetch('/expr_laugh')">Laugh</button>
  <button onclick="fetch('/expr_winkL')">Wink L</button>
  <button onclick="fetch('/expr_winkR')">Wink R</button>
</div>
<div class="mood-toggles">
  <span class="section-inline">Toggles:</span>
  <button id="btn_sweat" onclick="toggleSweat()">Sweat</button>
  <button id="btn_curious_expr" onclick="toggleCurious()">Curious</button>
</div>

<div class="section">Sound</div>
<div class="mode">
  <button id="btn_mute" onclick="toggleMute()">MUTE: OFF</button>
</div>

<div class="section">Dances</div>
<div class="mode">
  <button onclick="fetch('/dance_wiggle')">Wiggle</button>
  <button onclick="fetch('/dance_spin')">Spin</button>
  <button onclick="fetch('/dance_happy')">Happy</button>
</div>

<div class="section">Sounds</div>
<div class="mode">
  <button onclick="fetch('/sound_chirp')">Chirp</button>
  <button onclick="fetch('/sound_mario')">Mario</button>
  <button onclick="fetch('/sound_nokia')">Nokia</button>
  <button onclick="fetch('/sound_starwars')">Star Wars</button>
</div>

<div class="section">Auto-Personality</div>
<div class="mode">
  <button id="btn_personality" onclick="togglePersonality()">OFF</button>
</div>

<div class="section">Clock mode</div>
<div class="mode">
  <button id="btn_clock" onclick="toggleClock()">CLOCK: OFF</button>
</div>
<div class="mode" style="margin-top:4px;">
  <input type="number" id="clock_h" min="0" max="23" value="12" style="width:48px;font-size:16px;background:#0a0a0a;border:1px solid #00ff41;color:#00ff41;font-family:inherit;padding:8px;box-sizing:border-box;">
  <input type="number" id="clock_m" min="0" max="59" value="0" style="width:48px;font-size:16px;background:#0a0a0a;border:1px solid #00ff41;color:#00ff41;font-family:inherit;padding:8px;box-sizing:border-box;">
  <button onclick="clockSet()">Set time</button>
</div>

<div class="section">Dice</div>
<div class="mode">
  <button onclick="diceRollUI()">Roll</button>
  <span id="dice_result" style="margin-left:8px;">--</span>
</div>

<div class="footer">Created by MamRez</div>
</div>

<script>
function setActiveInGroup(activeId, allIds){
  allIds.forEach(function(id){
    var el=document.getElementById(id);
    if(el) el.classList.toggle('active', id===activeId);
  });
}
var MODE_BTNS = ['btn_sleep','btn_wiggle','btn_curious'];
var MODE_ACTIVE = {off:'btn_sleep', soft:'btn_wiggle', normal:'btn_curious'};
var MOOD_BTNS = ['btn_mood_default','btn_mood_tired','btn_mood_angry','btn_mood_happy'];
var MOOD_ACTIVE = {default:'btn_mood_default', tired:'btn_mood_tired', angry:'btn_mood_angry', happy:'btn_mood_happy'};
function setMode(mode){
  fetch('/mode_' + mode);
  setActiveInGroup(MODE_ACTIVE[mode], MODE_BTNS);
}
function setMood(mode){
  fetch('/mood_' + mode);
  setActiveInGroup(MOOD_ACTIVE[mode], MOOD_BTNS);
}
function toggleFetch(url, btnId, onText, offText){
  fetch(url).then(r=>r.text()).then(function(t){
    var b=document.getElementById(btnId);
    var on=(t==='1');
    b.classList.toggle('active', on);
    if(onText!==undefined) b.textContent=on?onText:offText;
  });
}
function toggleSweat(){ toggleFetch('/expr_sweat', 'btn_sweat'); }
function toggleCurious(){ toggleFetch('/expr_curious', 'btn_curious_expr'); }
function togglePersonality(){ toggleFetch('/personality_toggle', 'btn_personality', 'ON', 'OFF'); }
function toggleMute(){ toggleFetch('/mute_toggle', 'btn_mute', 'MUTE: ON', 'MUTE: OFF'); }
function toggleClock(){ toggleFetch('/clock_toggle', 'btn_clock', 'CLOCK: ON', 'CLOCK: OFF'); }
function clockSet(){
  var h=document.getElementById('clock_h').value;
  var m=document.getElementById('clock_m').value;
  fetch('/clock_set?h='+h+'&m='+m);
}
function diceRollUI(){
  fetch('/dice_roll');
  document.getElementById('dice_result').textContent='...';
  pollDiceStatus();
}
function pollDiceStatus(){
  fetch('/dice_status').then(r=>r.json()).then(function(j){
    if(j.animating) setTimeout(pollDiceStatus,200);
    else document.getElementById('dice_result').textContent=j.value>0?'Last: '+j.value:'--';
  });
}
fetch('/expr_state').then(r=>r.json()).then(function(j){
  if(j.sweat) document.getElementById('btn_sweat').classList.add('active');
  if(j.curious) document.getElementById('btn_curious_expr').classList.add('active');
});
toggleFetch('/personality_status', 'btn_personality', 'ON', 'OFF');
toggleFetch('/mute_status', 'btn_mute', 'MUTE: ON', 'MUTE: OFF');
toggleFetch('/clock_status', 'btn_clock', 'CLOCK: ON', 'CLOCK: OFF');
fetch('/dice_status').then(r=>r.json()).then(function(j){
  document.getElementById('dice_result').textContent=j.value>0?'Last: '+j.value:'--';
});
(function setTimeFromDevice(){
  var d=new Date();
  var h=d.getHours();
  var m=d.getMinutes();
  var s=d.getSeconds();
  document.getElementById('clock_h').value=h;
  document.getElementById('clock_m').value=m;
  fetch('/clock_set?h='+h+'&m='+m+'&s='+s);
})();
</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", page);
}

static void handleDirection(MotorDir dir) {
  if (clockState.active) { server.send(200); return; }
  manualActive = (dir != DIR_STOP);
  setMotors(dir);
  soundActionBeep();
  server.send(200);
}

static void handleMode(RandomMode m) {
  randomMode = m;
  soundActionBeep();
  server.send(200);
}

static void handleMood(uint8_t moodId, void (*soundFn)()) {
  roboEyes.setMood(moodId);
  soundFn();
  server.send(200);
}

/* ================= SERVER ================= */
void setupServer() {
  server.on("/", handleRoot);

  server.on("/mute_toggle", [](){ mute = !mute; server.send(200, "text/plain", mute ? "1" : "0"); });
  server.on("/mute_status",  [](){ server.send(200, "text/plain", mute ? "1" : "0"); });

  server.on("/clock_toggle", [](){
    clockState.active = !clockState.active;
    if (clockState.active) enterClockMode(); else exitClockMode();
    server.send(200, "text/plain", clockState.active ? "1" : "0");
  });
  server.on("/clock_set", [](){
    if (server.hasArg("h")) clockState.hour = constrain(server.arg("h").toInt(), 0, 23);
    if (server.hasArg("m")) clockState.minute = constrain(server.arg("m").toInt(), 0, 59);
    if (server.hasArg("s")) clockState.second = constrain(server.arg("s").toInt(), 0, 59);
    clockState.lastTick = millis();
    server.send(200);
  });
  server.on("/clock_status", [](){ server.send(200, "text/plain", clockState.active ? "1" : "0"); });

  server.on("/f", [](){ handleDirection(DIR_FWD); });
  server.on("/b", [](){ handleDirection(DIR_BACK); });
  server.on("/l", [](){ handleDirection(DIR_LEFT); });
  server.on("/r", [](){ handleDirection(DIR_RIGHT); });
  server.on("/s", [](){ handleDirection(DIR_STOP); });

  server.on("/mode_off",    [](){ handleMode(RANDOM_OFF); });
  server.on("/mode_soft",   [](){ handleMode(RANDOM_SOFT); });
  server.on("/mode_normal", [](){ handleMode(RANDOM_NORMAL); });

  server.on("/mood_default", [](){ handleMood(DEFAULT, soundMoodDefault); });
  server.on("/mood_tired",   [](){ handleMood(TIRED, soundMoodTired); });
  server.on("/mood_angry",   [](){ handleMood(ANGRY, soundMoodAngry); });
  server.on("/mood_happy",   [](){ handleMood(HAPPY, soundMoodHappy); });

  server.on("/expr_confused", [](){ roboEyes.anim_confused(); soundActionBeep(); server.send(200); });
  server.on("/expr_laugh",    [](){ roboEyes.anim_laugh(); soundActionBeep(); server.send(200); });
  server.on("/expr_winkL",    [](){ roboEyes.blink(true, false); soundActionBeep(); server.send(200); });
  server.on("/expr_winkR",    [](){ roboEyes.blink(false, true); soundActionBeep(); server.send(200); });
  server.on("/expr_sweat",    [](){ sweatOn = !sweatOn; roboEyes.setSweat(sweatOn ? ON : OFF); soundActionBeep(); server.send(200, "text/plain", sweatOn ? "1" : "0"); });
  server.on("/expr_curious",  [](){ curiousOn = !curiousOn; roboEyes.setCuriosity(curiousOn ? ON : OFF); soundActionBeep(); server.send(200, "text/plain", curiousOn ? "1" : "0"); });
  server.on("/expr_state",    [](){ String j = "{\"sweat\":"; j += sweatOn?"1":"0"; j += ",\"curious\":"; j += curiousOn?"1":"0"; j += "}"; server.send(200, "application/json", j); });
  server.on("/personality_status", [](){ server.send(200, "text/plain", personalityOn ? "1" : "0"); });

  server.on("/dance_wiggle", [](){ startDance(0); soundActionBeep(); server.send(200); });
  server.on("/dance_spin",   [](){ startDance(1); soundActionBeep(); server.send(200); });
  server.on("/dance_happy",  [](){ startDance(2); soundActionBeep(); server.send(200); });

  server.on("/personality_toggle", [](){ personalityOn = !personalityOn; soundActionBeep(); server.send(200, "text/plain", personalityOn ? "1" : "0"); });

  server.on("/dice_roll", [](){ diceRoll(); soundActionBeep(); server.send(200); });
  server.on("/dice_status", [](){
    String j = "{\"value\":";
    j += diceState.result;
    j += ",\"animating\":";
    j += diceState.animating ? "1" : "0";
    j += "}";
    server.send(200, "application/json", j);
  });

  server.on("/sound_chirp", [](){ soundChirp(); server.send(200); });
  server.on("/sound_mario", [](){ playMelody(MELODY_MARIO_COIN, MELODY_MARIO_COIN_DUR, (int)ARRAY_LEN(MELODY_MARIO_COIN)); server.send(200); });
  server.on("/sound_nokia", [](){ playMelody(MELODY_NOKIA, MELODY_NOKIA_DUR, (int)ARRAY_LEN(MELODY_NOKIA)); server.send(200); });
  server.on("/sound_starwars", [](){ playMelody(MELODY_STARWARS, MELODY_STARWARS_DUR, (int)ARRAY_LEN(MELODY_STARWARS)); server.send(200); });

  server.onNotFound(handleRoot);
  server.begin();
}

/* ================= SETUP ================= */
void setup() {
  pinMode(STBY,OUTPUT); digitalWrite(STBY,LOW);
  pinMode(LF,OUTPUT); pinMode(LB,OUTPUT);
  pinMode(RF,OUTPUT); pinMode(RB,OUTPUT);

  Serial.begin(115200);
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC,0x3C);
  display.clearDisplay(); display.display();

  roboEyes.begin(SCREEN_WIDTH,SCREEN_HEIGHT,100);
  roboEyes.setAutoblinker(ON,3,2);
  roboEyes.setIdleMode(ON,2,2);
  roboEyes.setMood(DEFAULT);

  randomSeed(esp_random());

  pinMode(BUZZER_PIN, OUTPUT);
  ledcAttach(BUZZER_PIN, 440, 8);
  ledcWriteTone(BUZZER_PIN, 0);

  pinMode(CLOCK_BTN_PIN, INPUT_PULLUP);
  lastClockBtnState = -1;

  Serial.write("LETS GO!");

  WiFi.softAP("MOCHAN");
  dnsServer.start(53,"*",WiFi.softAPIP());
  setupServer();

  /* Ensure motors are stopped before enabling driver (fixes left wheel spin at boot) */
  digitalWrite(LF, LOW);
  digitalWrite(LB, LOW);
  digitalWrite(RF, LOW);
  digitalWrite(RB, LOW);
  digitalWrite(STBY, HIGH);

  if (!mute) playMelody(MELODY_STARTUP, MELODY_STARTUP_DUR, (int)ARRAY_LEN(MELODY_STARTUP));
}

/* ================= LOOP ================= */
void loop() {
  /* Built-in button: toggle clock mode (debounced).
     Reset the debounce timer on ANY state change (press or release) so that
     mechanical bounce on release cannot re-trigger the toggle. */
  int btn = digitalRead(CLOCK_BTN_PIN);
  unsigned long now = millis();
  if (lastClockBtnState == -1) lastClockBtnState = btn;
  if (btn == CLOCK_BTN_PRESSED && lastClockBtnState != CLOCK_BTN_PRESSED && (now - lastClockBtnMillis) >= (unsigned long)CLOCK_BTN_DEBOUNCE_MS) {
    lastClockBtnMillis = now;
    clockState.active = !clockState.active;
    if (clockState.active) enterClockMode(); else exitClockMode();
  }
  if (btn != lastClockBtnState) lastClockBtnMillis = now;
  lastClockBtnState = btn;

  if (clockState.active) {
    updateClockSeconds();
    if (millis() - clockState.lastDraw >= 1000) {
      drawClock();
      clockState.lastDraw = millis();
    }
    server.handleClient();
    dnsServer.processNextRequest();
    return;
  }

  updateMelody();
  updateDance();
  updateDice();
  updatePersonality();
  if (!diceState.animating) roboEyes.update();
  server.handleClient();
  dnsServer.processNextRequest();

  static unsigned long lastTick = 0;
  if (!manualActive && millis() - lastTick > 40) {
    lastTick = millis();

    if (randomMode == RANDOM_SOFT) {
      if (random(120) == 1) {
        pulseMotor((MotorDir)random(MOTOR_DIR_COUNT), random(6,18), random(40,90), 1);
      }
    }
    else if (randomMode == RANDOM_NORMAL) {
      if (random(100) == 1) {
        pulseMotor((MotorDir)random(MOTOR_DIR_COUNT), random(5,50), random(10,100), random(20));
      }
    }
  }
}
