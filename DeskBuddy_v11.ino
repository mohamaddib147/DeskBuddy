#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_MPU6050 mpu;
Preferences prefs;   // saves the SETTINGS, your best reaction time and the tempo (NOT the DIZZY easter egg)

#define DHTPIN 15
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Set to 1 to stream tilt / shake values to the Serial Monitor (115200)
#define DEBUG_TILT 0

// 1 = smooth colors + brightness through LEDC PWM (works on ESP32 core 2.x and 3.x)
// 0 = plain on/off pins (only 8 colors, no brightness) - use this if PWM ever gives trouble
#define RGB_PWM 1

// RGB LED polarity. Use whatever your LED actually does:
//   0 = common leg goes to GND  (common-CATHODE: pin HIGH = colour ON)   <- matches what you observed
//   1 = common leg goes to 3.3V (common-ANODE:   pin LOW  = colour ON)
// Quick check: at boot the screen says "LED should be OFF". If the LED is lit then, flip this value.
#define RGB_COMMON_ANODE 0

// LED colour balance (PWM mode only). On most cheap RGB LEDs the red die is far brighter than
// green/blue at the same current, so red is turned down. Tune with the torch on WHITE:
// reddish -> lower RGB_GAIN_R, greenish -> lower RGB_GAIN_G, bluish -> lower RGB_GAIN_B (0-255).
const uint8_t RGB_GAIN_R = 100;
const uint8_t RGB_GAIN_G = 190;
const uint8_t RGB_GAIN_B = 255;
const float RGB_GAMMA = 2.0;   // 1.0 = off. Makes mixed colours (orange, pink, purple) look right

// What the LED does when you lose a game: 0 = solid red until you restart/exit, 1 = blinking red
#define GAMEOVER_BLINK 0

struct Color { uint8_t r, g, b; };
const Color C_OFF    = {0, 0, 0};
const Color C_RED    = {255, 0, 0};
const Color C_GREEN  = {0, 255, 0};

// ---------------- Pins ----------------
// I2C bus (OLED 0x3C + GY-521 0x68 share it): SDA 21, SCL 22
const int VRX_PIN = 34;
const int VRY_PIN = 35;
const int SW_PIN = 32;
const int LED_PIN = 26;        // optional plain torch LED (leave unwired if you don't have it)
const int BUZZER_PIN = 27;
const int BTN_FIRE_PIN = 4;
const int BTN_BACK_PIN = 5;
const int BTN_SELECT_PIN = 23;
const int RGB_RED_PIN = 16;    // RGB LED legs, each through ~220 ohm (common leg: see RGB_COMMON_ANODE)
const int RGB_GREEN_PIN = 17;
const int RGB_BLUE_PIN = 19;

const char* menuItems[] = {"EMO MODE", "GAMES", "STOPWATCH", "TORCH", "TEMP",
                           "TIMER", "LEVEL", "REACTION", "MUSIC", "TOYS", "SETTINGS"};
const int MENU_COUNT = 11;
const int MENU_VISIBLE = 6;   // rows that fit on screen; the list scrolls
const int ROW_H = 10;         // 6 rows x 10px = 60px, fits the 64px screen
int menuTop = 0;              // first visible menu row

enum State { MENU, EMO, GAMES, GAME, STOPWATCH, TORCH, TEMP, SHOOTER, TIMER, LEVEL, REACTION, MUSIC, TOYS, SETTINGS };
State state = MENU;

int selected = 0;
unsigned long lastMove = 0;
unsigned long lastTempRead = 0;
bool gestureAsleep = false;      // board is face-down: screen off, sound off

// ---------------- Debounced buttons ----------------
struct Button {
  uint8_t pin;
  bool stable;              // debounced level (HIGH = released)
  bool lastRaw;
  unsigned long lastChange;
  unsigned long downTime;
  bool pressed;             // true for exactly one loop pass
  bool released;            // true for exactly one loop pass
  unsigned long held;       // ms the button was held (valid when released)
};

const unsigned long DEBOUNCE_MS = 25;
const unsigned long LONG_PRESS_MS = 500;

Button btnJoy    = {SW_PIN,         HIGH, HIGH, 0, 0, false, false, 0};
Button btnFire   = {BTN_FIRE_PIN,   HIGH, HIGH, 0, 0, false, false, 0};
Button btnBack   = {BTN_BACK_PIN,   HIGH, HIGH, 0, 0, false, false, 0};
Button btnSelect = {BTN_SELECT_PIN, HIGH, HIGH, 0, 0, false, false, 0};

bool swallowJoyRelease = false; // ignore the release of the click that opened a mode

void updateButton(Button &b) {
  b.pressed = false;
  b.released = false;
  b.held = 0;
  bool raw = digitalRead(b.pin);
  unsigned long now = millis();
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.lastChange = now;
  }
  if ((now - b.lastChange) >= DEBOUNCE_MS && raw != b.stable) {
    b.stable = raw;
    if (b.stable == LOW) {
      b.pressed = true;
      b.downTime = now;
    } else {
      b.released = true;
      b.held = now - b.downTime;
    }
  }
}

// ---------------- MPU6050 (tilt + shake) ----------------
bool mpuOK = false;
float tiltRaw = 0;                 // degrees (the left/right axis the games use)
float tiltOtherRaw = 0;            // degrees (the other axis, used by the spirit level)
float tiltZero = 0;                // captured when a game starts
unsigned long lastMpuRead = 0;
const unsigned long MPU_READ_INTERVAL = 30;
const float TILT_DEADBAND = 6.0;   // degrees around the starting pose where the joystick rules
const float TILT_MAX = 30.0;       // degrees of tilt = edge of the screen
const float TILT_SIGN = 1.0;       // set to -1.0 if tilting right moves the ship left
const bool TILT_USE_X = false;     // set true if the tilt axis is the other one on your board
float shakeThresh = 2.0;          // lower = easier to trigger a shake (changed in SETTINGS)
const unsigned long SHAKE_COOLDOWN_MS = 800;
bool shakeTriggered = false;
unsigned long lastShakeTime = 0;
float accelG = 1.0;
float accelZ = 9.8;                // m/s^2 along the board's Z axis (negative = face-down)
float prevAccelG = 1.0;
float gyroMag = 0;                 // rad/s, total rotation speed

// ---- eyes that follow the tilt (EMO and the screensaver) ----
const float GAZE_RANGE_DEG = 18.0; // tilt that moves the pupils to the edge of the eye
const bool GAZE_FLIP_X = false;    // set true if the pupils look the wrong way left/right
const bool GAZE_FLIP_Y = false;    // set true if the pupils look the wrong way up/down
float gazeX = 0, gazeY = 0;        // -1 ... 1
float gazeZeroX = 0, gazeZeroY = 0;// slow-moving "rest pose" the gaze is measured from
bool gazeInit = false;

// ---- spinning the board makes the face dizzy ----
const float SPIN_RATE = 4.5;       // rad/s (about 260 deg/s) counts as spinning
float spinMs = 0;
bool spinDizzy = false;

// ---- double tap on the board = an extra SELECT button ----
const float TAP_DELTA = 0.35;      // g jump between two readings that counts as a tap (tune me)
const unsigned long TAP_GAP_MIN = 110;
const unsigned long TAP_GAP_MAX = 500;
bool doubleTap = false;            // true for one loop pass
int tapCount = 0;
unsigned long lastTapTime = 0;

void updateMPU() {
  shakeTriggered = false;
  doubleTap = false;
  if (!mpuOK) return;
  unsigned long nowMs = millis();
  if (nowMs - lastMpuRead < MPU_READ_INTERVAL) return;
  float dtMs = (float)(nowMs - lastMpuRead);
  if (dtMs > 100.0f) dtMs = 100.0f;
  lastMpuRead = nowMs;

  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  float ax = a.acceleration.x;
  float ay = a.acceleration.y;
  float az = a.acceleration.z;

  float angY = atan2(ay, az);
  float angX = atan2(ax, az);
  float angle = TILT_USE_X ? angX : angY;
  float other = TILT_USE_X ? angY : angX;
  tiltRaw = angle * 180.0 / PI * TILT_SIGN;
  tiltOtherRaw = other * 180.0 / PI;
  accelG = sqrt(ax * ax + ay * ay + az * az) / 9.80665;
  accelZ = az;
  gyroMag = sqrtf(g.gyro.x * g.gyro.x + g.gyro.y * g.gyro.y + g.gyro.z * g.gyro.z);

  if (accelG > shakeThresh && nowMs - lastShakeTime > SHAKE_COOLDOWN_MS) {
    shakeTriggered = true;
    lastShakeTime = nowMs;
  }

  // where the eyes look: the tilt relative to a rest pose that slowly follows you (about 3 s)
  if (!gazeInit) { gazeZeroX = tiltRaw; gazeZeroY = tiltOtherRaw; gazeInit = true; }
  gazeZeroX += (tiltRaw - gazeZeroX) * 0.01f;
  gazeZeroY += (tiltOtherRaw - gazeZeroY) * 0.01f;
  float gx = constrain((tiltRaw - gazeZeroX) / GAZE_RANGE_DEG, -1.0f, 1.0f);
  float gy = constrain((tiltOtherRaw - gazeZeroY) / GAZE_RANGE_DEG, -1.0f, 1.0f);
  if (GAZE_FLIP_X) gx = -gx;
  if (GAZE_FLIP_Y) gy = -gy;
  gazeX += (gx - gazeX) * 0.3f;
  gazeY += (gy - gazeY) * 0.3f;

  // dizzy meter: fills while you spin the board, drains when you stop
  if (gyroMag > SPIN_RATE) spinMs += dtMs; else spinMs -= dtMs * 0.6f;
  spinMs = constrain(spinMs, 0.0f, 3000.0f);
  if (!spinDizzy && spinMs > 1200.0f) spinDizzy = true;
  else if (spinDizzy && spinMs < 300.0f) spinDizzy = false;

  // double tap: two sharp little bumps 0.1 - 0.5 s apart (ignored during shakes and spins)
  float jump = fabsf(accelG - prevAccelG);
  prevAccelG = accelG;
  bool calm = (nowMs - lastShakeTime > 700) && !spinDizzy;
  if (calm && jump > TAP_DELTA && jump < 1.5f && nowMs - lastTapTime > TAP_GAP_MIN) {
    if (tapCount == 1 && nowMs - lastTapTime < TAP_GAP_MAX) { doubleTap = true; tapCount = 0; }
    else tapCount = 1;
    lastTapTime = nowMs;
  }
  if (tapCount == 1 && nowMs - lastTapTime > TAP_GAP_MAX) tapCount = 0;
}

void captureTiltZero() {
  tiltZero = tiltRaw;
}

// Position for a paddle/ship of a given width: tilt overrides the joystick once
// the board is tilted past the deadband; otherwise the joystick works as before.
int steerX(int joyRawValue, int width) {
  int maxX = 128 - width;
  float rel = tiltRaw - tiltZero;
  if (mpuOK && fabs(rel) > TILT_DEADBAND) {
    float t = (fabs(rel) - TILT_DEADBAND) / (TILT_MAX - TILT_DEADBAND);
    if (t > 1.0) t = 1.0;
    if (rel < 0) t = -t;
    int pos = (int)((0.5 + 0.5 * t) * maxX);
    return constrain(pos, 0, maxX);
  }
  int pos = map(joyRawValue, 0, 4095, 0, maxX);
  return constrain(pos, 0, maxX);
}

// ---------------- Idle / screensaver state ----------------
const unsigned long IDLE_OPEN_MS = 4000;     // how long eyes stay open between blinks
const unsigned long IDLE_BLINK_MS = 150;     // how long the blink (closed) lasts
bool idleActive = false;
bool idleEyesOpen = true;
unsigned long idlePhaseStart = 0;
unsigned long lastActivityTime = 0;

// ---------------- Settings (saved to flash when you leave SETTINGS) ----------------
bool soundOn = true;
int ledIdx = 3;                                   // index into ledScales
int ssIdx = 1;                                    // index into ssTimes
int shakeIdx = 2;                                 // shake threshold = 1.5 + 0.25 * shakeIdx  (default 2.0 g)
const int SHAKE_STEPS = 11;                       // 1.5 g ... 4.0 g
const uint8_t ledScales[] = {64, 128, 191, 255};
const char* ledNames[] = {"25%", "50%", "75%", "100%"};
const unsigned long ssTimes[] = {15000, 30000, 60000, 120000, 300000, 0};   // 0 = never
const char* ssNames[] = {"15 sec", "30 sec", "1 min", "2 min", "5 min", "NEVER"};
uint8_t ledScale = 255;                           // global RGB LED brightness
const int SET_COUNT = 8;
const int SET_VISIBLE = 5;
const char* setLabels[] = {"SOUND", "LED BRIGHT", "SCREENSAVER", "SHAKE LEVEL", "TEMP OFFSET", "HUM OFFSET", "GESTURES", "RESET ALL"};
bool gesturesOn = true;                           // face-down sleep, double-tap, tilt-scroll menu
int setSel = 0;
int setTop = 0;
int tOffTenths = 0;                               // temperature calibration, tenths of a degC (-5.0 ... +5.0)
int hOff = 0;                                     // humidity calibration, % points (-15 ... +15)
unsigned long lastSetMove = 0;
unsigned long lastSetDraw = 0;
unsigned long shakeFlashUntil = 0;
float shakePeak = 0;

// ---------------- Stopwatch state ----------------
unsigned long swElapsed = 0;
unsigned long swStartTime = 0;
bool swRunning = false;
unsigned long lastSwDraw = 0;
unsigned long lastSwMove = 0;
bool swResetArmed = false;                        // FIRE is being held to reset

// ---------------- Torch state ----------------
struct NamedColor { const char* name; Color c; };
const NamedColor torchColors[] = {
  {"WHITE",   {255, 255, 255}},
  {"RED",     {255, 0, 0}},
  {"ORANGE",  {255, 100, 0}},
  {"YELLOW",  {255, 180, 0}},
  {"GREEN",   {0, 255, 0}},
  {"CYAN",    {0, 255, 255}},
  {"BLUE",    {0, 0, 255}},
  {"PURPLE",  {140, 0, 255}},
  {"PINK",    {255, 0, 120}},
  {"RAINBOW", {255, 255, 255}}   // colour is generated live, see updateRGB()
};
const int TORCH_COUNT = 10;
const int TORCH_RAINBOW = 9;
const uint8_t torchLevels[] = {30, 70, 120, 185, 255};
const int TORCH_LEVEL_COUNT = 5;
bool torchOn = false;
int torchColorIdx = 0;
int torchLevelIdx = 4;
unsigned long lastTorchMove = 0;

// ---------------- Temp mode state ----------------
const int TEMP_HIST = 100;                         // graph points (1 px each)
const unsigned long TEMP_INTERVAL_MS = 2000;       // DHT11 can't be read faster than ~0.5 Hz
const unsigned long TEMP_BUCKET_MS = 20000;        // one graph point = 20 s average -> 100 points = 33 min
const int TEMP_SMOOTH_N = 4;                       // live value = average of the last 4 readings (8 s)
const int TEMP_FAIL_LIMIT = 4;                     // this many failed reads in a row = "NO SENSOR" screen
const unsigned long TEMP_STALE_MS = 10000;         // reading older than this is flagged STALE
float tempHist[TEMP_HIST];                         // degrees C
float humHist[TEMP_HIST];                          // % RH
int histCount = 0;
float smoothT[TEMP_SMOOTH_N], smoothH[TEMP_SMOOTH_N];
int smoothCount = 0, smoothPos = 0;
float curT = NAN, curH = NAN;                      // smoothed + calibrated
float minT = NAN, maxT = NAN, minH = NAN, maxH = NAN;
double sumT = 0;
unsigned long cntT = 0;
float bucketSumT = 0, bucketSumH = 0;
int bucketN = 0;
unsigned long bucketStart = 0;
int tempFails = 0;                                 // failed reads in a row
unsigned long lastGoodMs = 0;
bool everGood = false;
bool useF = false;
int tempPage = 0;                                  // 0 = NOW, 1 = TREND graph, 2 = STATS
bool graphHum = false;                             // graph page shows humidity instead of temperature
bool tempDirty = true;                             // screen needs a redraw
unsigned long lastPageMove = 0;

// ---------------- Emo mode state ----------------
// faces 0-4 are always there; face 5 (DIZZY) is unlocked by shaking the board in EMO mode
const char* emoNames[] = {"HAPPY", "SAD", "ANGRY", "SURPRISED", "WINK", "DIZZY"};
const int EMO_BASE_COUNT = 5;
const int EMO_DIZZY = 5;
const Color emoColors[] = {
  {0, 255, 60},    // HAPPY - green
  {0, 60, 255},    // SAD - blue
  {255, 0, 0},     // ANGRY - red
  {255, 180, 0},   // SURPRISED - yellow
  {255, 0, 200}    // WINK - magenta
};
const uint8_t EMO_LED_LEVEL = 150;
int emoIndex = 0;
unsigned long lastEmoMove = 0;
bool dizzyUnlocked = false;
unsigned long emoBannerUntil = 0;   // "NEW EMO UNLOCKED" screen
unsigned long lastDizzyFrame = 0;

int emoCount() {
  return dizzyUnlocked ? EMO_BASE_COUNT + 1 : EMO_BASE_COUNT;
}

// ---------------- Dodge game state ----------------
struct Obstacle { int x; int y; bool active; };
const int MAX_OBSTACLES = 4;
Obstacle obstacles[MAX_OBSTACLES];
int playerX = 56;
const int PLAYER_W = 16;
const int PLAYER_H = 4;
const int OBST_SIZE = 6;
int gameScore = 0;
int highScore = 0;
bool gameOver = false;
bool gamePaused = false;
unsigned long lastGameUpdate = 0;
unsigned long lastObstacleSpawn = 0;
unsigned long gameSpawnInterval = 900;
float gameFallSpeed = 1.5;

// ---------------- Shooter game state ----------------
struct Bullet { int x; int y; bool active; };
struct Enemy { int x; int y; bool active; };
const int MAX_BULLETS = 3;
const int MAX_ENEMIES = 3;
Bullet bullets[MAX_BULLETS];
Enemy enemies[MAX_ENEMIES];
int shipX = 60;
const int SHIP_W = 10;
const int SHIP_H = 5;
const int BULLET_SIZE = 2;
const int ENEMY_SIZE = 7;
int shooterScore = 0;
int shooterHighScore = 0;
int shooterLives = 3;
bool shooterOver = false;
bool shooterPaused = false;
unsigned long lastShooterUpdate = 0;
unsigned long lastEnemySpawn = 0;
unsigned long enemySpawnInterval = 1000;
float enemyFallSpeed = 1.0;
unsigned long lastShotTime = 0;
const unsigned long SHOT_COOLDOWN = 300;
bool fireQueued = false;

// shared by both games: ignore restart inputs briefly after "game over"
// so button-mashing or a shake right at the end doesn't skip the score screen
unsigned long gameOverAt = 0;
const unsigned long RESTART_GUARD_MS = 1000;

// ---------------- Timer (pomodoro / countdown) state ----------------
enum TimerPhase { TP_SETUP, TP_WORK, TP_BREAK, TP_DONE };
TimerPhase timerPhase = TP_SETUP;
int timerWorkMin = 25;
int timerBreakMin = 5;            // 0 = plain countdown (no break)
int timerField = 0;               // which one the joystick edits: 0 = WORK, 1 = BREAK
int timerRounds = 0;              // finished work rounds
bool timerPaused = false;
unsigned long tmrEndMs = 0;
unsigned long timerLen = 0;
unsigned long timerRemainMs = 0;  // frozen value while paused
unsigned long lastTimerDraw = 0;
unsigned long lastTimerAdj = 0;
unsigned long timerHoldStart = 0;
int lastTickSec = -1;

// ---------------- Spirit level state ----------------
const float LEVEL_FLAT_DEG = 1.0;   // closer than this to level = "flat" (LED green)
const float LEVEL_LEAVE_DEG = 1.6;  // must tilt past this to lose "flat" (stops flicker)
const float LEVEL_FULL_DEG = 15.0;  // tilt that pushes the bubble to the edge of the vial
const bool LEVEL_FLIP_X = false;    // set true if the bubble moves the wrong way left/right
const bool LEVEL_FLIP_Y = false;    // set true if the bubble moves the wrong way up/down
float levelZeroX = 0, levelZeroY = 0;
float bubbleX = 0, bubbleY = 0;     // smoothed bubble offset in pixels
bool levelFlat = false;
unsigned long lastLevelDraw = 0;

// ---------------- Reaction tester state ----------------
enum RxPhase { RX_READY, RX_WAIT, RX_GO, RX_RESULT, RX_EARLY };
RxPhase rxPhase = RX_READY;
unsigned long rxWaitUntil = 0;
unsigned long rxGoTime = 0;
unsigned int rxLast = 0;
unsigned int rxBest = 0;            // 0 = no score yet; saved to flash
bool rxNewBest = false;
const unsigned int RX_MIN_MS = 100; // faster than this is a guess, not a reaction

// ---------------- Blink timing (EMO face) ----------------
unsigned long nextBlinkMs = 0;
unsigned long blinkUntil = 0;

// ---------------- Music state ----------------
// Joystick CLICK is the SHIFT key inside MUSIC: tap it = next tab, hold it + stick left/right = previous / next tab,
// hold it + stick up/down or FIRE or SELECT = that tab's extra options (shown at the bottom of the screen).
const int MUS_TABS = 7;
const char* musTabNames[MUS_TABS] = {"METRONOME", "PIANO", "THEREMIN", "DRUMS", "SONGS", "TUNER", "RETRO"};
int musTab = 0;
bool musShiftUsed = false;           // the click was used as SHIFT, so letting go must not change tab
unsigned long lastMusDraw = 0;
unsigned long lastMusMove = 0;
unsigned long lastMusMove2 = 0;
Color musLed = {0, 0, 0};            // what the MUSIC tabs want on the RGB LED (updateRGB just shows it)
int bpm = 100;                       // tempo shared by the metronome and the drum machine (saved)
const char* NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// ---- metronome ----
const int BEAT_OPTS[4] = {2, 3, 4, 6};
const int SUB_OPTS[3] = {1, 2, 3};                       // ticks per beat: quarters, eighths, triplets
const char* SUB_NAMES[3] = {"", " x2", " x3"};
const char* METRO_NAMES[4] = {"CLICK", "BEEP", "TICK", "LOW"};
const int METRO_HI[4] = {1500, 880, 3000, 600};          // accent pitch
const int METRO_LO[4] = {1000, 660, 2400, 400};          // normal pitch
const int METRO_MS[4] = {30, 60, 12, 40};
int metroBeatsIdx = 2;               // 4 beats per bar
int metroSubIdx = 0;
int metroSound = 0;
bool metroAccentOn = true;
int metroBeat = 0;                   // next beat to play (0 = the accented one)
int metroCur = 0;                    // beat that just played
int metroSubTick = 0;                // 0 = the next tick is a full beat
int metroSwing = 0;                  // flips on every beat (pendulum side)
bool metroRunning = false;
unsigned long metroNext = 0;
unsigned long metroFlashUntil = 0;
bool metroFlashAccent = false;
unsigned long lastBpmAdj = 0;
unsigned long bpmHoldStart = 0;
unsigned long tapTempoT[4];
int tapTempoN = 0;

// ---- piano + looper ----
const int N_SCALES = 4;
const char* SCALE_NAMES[N_SCALES] = {"PENTA", "MAJOR", "MINOR", "BLUES"};
const int8_t SCALE_DEG[N_SCALES][6] = {
  {0, 2, 4, 7, 9, 12},               // pentatonic: C D E G A C  (always sounds nice)
  {0, 2, 4, 5, 7, 9},                // major:      C D E F G A
  {0, 2, 3, 5, 7, 8},                // minor:      C D Eb F G Ab
  {0, 3, 5, 6, 7, 10}                // blues:      C Eb F Gb G Bb
};
int pianoScale = 0;
int pianoOct = 1;                    // 0 = C4, 1 = C5, 2 = C6
int pianoKey = -1;                   // key you hold right now, -1 = none
int pianoSounding = -1;              // key that is actually sounding (yours, or the loop's)
struct LoopEv { uint16_t t; int8_t key; };
const int LOOP_MAX = 80;
LoopEv loopEv[LOOP_MAX];
int loopN = 0;
int loopState = 0;                   // 0 idle, 1 recording, 2 playing the loop
int loopPos = 0;
int loopKeyPlaying = -1;
unsigned long loopRecStart = 0;
unsigned long loopPlayStart = 0;
unsigned long loopLen = 1000;

// ---- theremin ----
int thereminBase = 1;                // 0 = A2 (110 Hz), 1 = A3 (220 Hz), 2 = A4 (440 Hz); the range is 3 octaves
bool thereminSnap = false;           // snap the pitch to real notes
bool thereminOn = false;
float thereminFreq = 220;
float thereminVib = 0;               // 0 ... 1, from tilting the board forward / back
float thereminOtherZero = 0;
float thereminLastOut = 0;
unsigned long thereminLastTune = 0;
int thereminSemi = 0;                // semitones above the base A

// ---- drum machine ----
const uint16_t DRUM_PRESETS[3][3] = {
  {0x0501, 0x1010, 0x5555},          // ROCK:   kick / snare / hat  (bit n = step n of 16)
  {0x1111, 0x1010, 0x4444},          // DISCO:  four on the floor, off-beat hats
  {0x0481, 0x1010, 0x5555}           // HIPHOP: syncopated kick
};
const char* DRUM_NAMES[4] = {"ROCK", "DISCO", "HIPHOP", "EMPTY"};
uint16_t drumPat[3] = {0x0501, 0x1010, 0x5555};
int drumPreset = 0;
int drumStep = 0;                    // next step to play
int drumPlayStep = -1;               // step that just played (the playhead)
int drumCurRow = 0, drumCurCol = 0;  // edit cursor
bool drumRunning = false;
bool drumDirty = true;
unsigned long drumNext = 0;
int drumFx = 0;                      // sound being made right now: 0 none, 1 kick, 2 snare, 3 hat
unsigned long drumFxStart = 0;
unsigned long drumFxLast = 0;
int drumFxLastF = 0;
unsigned long drumFlashUntil = 0;
int drumFlashRow = 0;

// ---- built-in songs (all public domain) ----
struct SongNote { uint8_t m; uint8_t u; };   // MIDI note (0 = rest), length in sixteenths
const SongNote SONG_TWINKLE[] = {
  {60, 4}, {60, 4}, {67, 4}, {67, 4}, {69, 4}, {69, 4}, {67, 8},
  {65, 4}, {65, 4}, {64, 4}, {64, 4}, {62, 4}, {62, 4}, {60, 8},
  {67, 4}, {67, 4}, {65, 4}, {65, 4}, {64, 4}, {64, 4}, {62, 8},
  {67, 4}, {67, 4}, {65, 4}, {65, 4}, {64, 4}, {64, 4}, {62, 8},
  {60, 4}, {60, 4}, {67, 4}, {67, 4}, {69, 4}, {69, 4}, {67, 8},
  {65, 4}, {65, 4}, {64, 4}, {64, 4}, {62, 4}, {62, 4}, {60, 8}
};
const SongNote SONG_ODE[] = {
  {64, 4}, {64, 4}, {65, 4}, {67, 4}, {67, 4}, {65, 4}, {64, 4}, {62, 4},
  {60, 4}, {60, 4}, {62, 4}, {64, 4}, {64, 6}, {62, 2}, {62, 8},
  {64, 4}, {64, 4}, {65, 4}, {67, 4}, {67, 4}, {65, 4}, {64, 4}, {62, 4},
  {60, 4}, {60, 4}, {62, 4}, {64, 4}, {62, 6}, {60, 2}, {60, 8},
  {62, 4}, {62, 4}, {64, 4}, {60, 4}, {62, 4}, {64, 2}, {65, 2}, {64, 4}, {60, 4},
  {62, 4}, {64, 2}, {65, 2}, {64, 4}, {62, 4}, {60, 4}, {62, 4}, {55, 8},
  {64, 4}, {64, 4}, {65, 4}, {67, 4}, {67, 4}, {65, 4}, {64, 4}, {62, 4},
  {60, 4}, {60, 4}, {62, 4}, {64, 4}, {62, 6}, {60, 2}, {60, 8}
};
const SongNote SONG_BDAY[] = {
  {67, 3}, {67, 1}, {69, 4}, {67, 4}, {72, 4}, {71, 8},
  {67, 3}, {67, 1}, {69, 4}, {67, 4}, {74, 4}, {72, 8},
  {67, 3}, {67, 1}, {79, 4}, {76, 4}, {72, 4}, {71, 4}, {69, 8},
  {77, 3}, {77, 1}, {76, 4}, {72, 4}, {74, 4}, {72, 8}
};
const SongNote SONG_JINGLE[] = {
  {64, 4}, {64, 4}, {64, 8}, {64, 4}, {64, 4}, {64, 8},
  {64, 4}, {67, 4}, {60, 6}, {62, 2}, {64, 16},
  {65, 4}, {65, 4}, {65, 6}, {65, 2}, {65, 4}, {64, 4}, {64, 4}, {64, 2}, {64, 2},
  {64, 4}, {62, 4}, {62, 4}, {64, 4}, {62, 8}, {67, 8}
};
const SongNote SONG_ELISE[] = {
  {76, 1}, {75, 1}, {76, 1}, {75, 1}, {76, 1}, {71, 1}, {74, 1}, {72, 1},
  {69, 2}, {0, 1}, {60, 1}, {64, 1}, {69, 1},
  {71, 2}, {0, 1}, {64, 1}, {68, 1}, {71, 1},
  {72, 2}, {0, 1}, {64, 1}, {76, 1}, {75, 1},
  {76, 1}, {75, 1}, {76, 1}, {71, 1}, {74, 1}, {72, 1},
  {69, 2}, {0, 1}, {60, 1}, {64, 1}, {69, 1},
  {71, 2}, {0, 1}, {64, 1}, {72, 1}, {71, 1},
  {69, 6}
};
// Korobeiniki: the old Russian folk tune (public domain) that the falling-blocks game made famous
const SongNote SONG_KOROBEI[] = {
  {76, 4}, {71, 2}, {72, 2}, {74, 4}, {72, 2}, {71, 2},
  {69, 4}, {69, 2}, {72, 2}, {76, 4}, {74, 2}, {72, 2},
  {71, 6}, {72, 2}, {74, 4}, {76, 4},
  {72, 4}, {69, 4}, {69, 8},
  {74, 6}, {77, 2}, {81, 4}, {79, 2}, {77, 2},
  {76, 6}, {72, 2}, {76, 4}, {74, 2}, {72, 2},
  {71, 6}, {72, 2}, {74, 4}, {76, 4},
  {72, 4}, {69, 4}, {69, 8}
};
// SPEEDRUN: my own finger-twister, all sixteenths. Push the tempo up and use RAMP mode!
const SongNote SONG_SPEED[] = {
  {57, 1}, {60, 1}, {64, 1}, {69, 1}, {72, 1}, {69, 1}, {64, 1}, {60, 1}, {55, 1}, {59, 1}, {62, 1}, {67, 1}, {71, 1}, {67, 1}, {62, 1}, {59, 1},
  {53, 1}, {57, 1}, {60, 1}, {65, 1}, {69, 1}, {65, 1}, {60, 1}, {57, 1}, {52, 1}, {56, 1}, {59, 1}, {64, 1}, {68, 1}, {64, 1}, {59, 1}, {56, 1},
  {69, 1}, {72, 1}, {76, 1}, {81, 1}, {76, 1}, {72, 1}, {69, 1}, {72, 1}, {67, 1}, {71, 1}, {74, 1}, {79, 1}, {74, 1}, {71, 1}, {67, 1}, {71, 1},
  {65, 1}, {69, 1}, {72, 1}, {77, 1}, {72, 1}, {69, 1}, {65, 1}, {69, 1}, {64, 1}, {68, 1}, {71, 1}, {76, 1}, {80, 1}, {76, 1}, {71, 1}, {68, 1},
  {81, 1}, {80, 1}, {79, 1}, {78, 1}, {77, 1}, {76, 1}, {75, 1}, {74, 1}, {73, 1}, {72, 1}, {71, 1}, {70, 1}, {69, 1}, {68, 1}, {67, 1}, {66, 1},
  {57, 1}, {60, 1}, {64, 1}, {69, 1}, {72, 1}, {76, 1}, {81, 1}, {84, 1}, {81, 2}, {0, 2}, {69, 8}
};
// MY SONG: type your own tune here (a game theme you love, anything you have the sheet music for).
//   {note, length}   note = MIDI number: C4 = 60, C#4 = 61, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71, C5 = 72 ... up to 84; 0 = rest
//   length is counted in sixteenth notes: 1 = sixteenth, 2 = eighth, 4 = quarter, 8 = half, 16 = whole.
const SongNote SONG_MINE[] = {
  {60, 4}, {62, 4}, {64, 4}, {65, 4}, {67, 8}, {0, 4}, {67, 4}, {65, 4}, {64, 4}, {62, 4}, {60, 8}
};
struct SongInfo { const char* name; const SongNote* notes; uint8_t n; uint8_t bpm; };
const int SONG_COUNT = 8;
const SongInfo SONGS[SONG_COUNT] = {
  {"TWINKLE",  SONG_TWINKLE, sizeof(SONG_TWINKLE) / sizeof(SongNote), 100},
  {"ODE JOY",  SONG_ODE,     sizeof(SONG_ODE) / sizeof(SongNote),     110},
  {"BIRTHDAY", SONG_BDAY,    sizeof(SONG_BDAY) / sizeof(SongNote),    90},
  {"JINGLE",   SONG_JINGLE,  sizeof(SONG_JINGLE) / sizeof(SongNote),  120},
  {"ELISE",    SONG_ELISE,   sizeof(SONG_ELISE) / sizeof(SongNote),   150},
  {"KOROBEI",  SONG_KOROBEI, sizeof(SONG_KOROBEI) / sizeof(SongNote), 150},
  {"SPEEDRUN", SONG_SPEED,   sizeof(SONG_SPEED) / sizeof(SongNote),   150},
  {"MY SONG",  SONG_MINE,    sizeof(SONG_MINE) / sizeof(SongNote),    120}
};
int songIdx = 0;
int songPos = 0;                     // next note to play
int songBpm = 100;
int songCurMidi = -1;
bool songPlaying = false;
bool songGapPending = false;
int songMode = 0;                    // 0 play once, 1 loop, 2 ramp (a bit faster every time round)
unsigned long songNext = 0;
unsigned long songGapAt = 0;

// ---- tuner / pitch pipe ----
const int TUNER_N = 11;
const char* TUNER_NAMES[TUNER_N] = {"PITCH PIPE A4", "UKULELE G", "UKULELE C", "UKULELE E", "UKULELE A",
                                    "GUITAR E (+8va)", "GUITAR A (+8va)", "GUITAR D (+8va)",
                                    "GUITAR G", "GUITAR B", "GUITAR e"};
const int TUNER_MIDI[TUNER_N] = {69, 67, 60, 64, 69, 52, 57, 62, 55, 59, 64};
int tunerPreset = 0;
int tunerMidi = 69;
bool tunerLatch = false;             // SELECT: keep sounding without holding FIRE
bool tunerSounding = false;
int tunerSoundMidi = -1;

// ---- retro sound bank (8-bit style effects made of tone steps) ----
const int SFX_COUNT = 8;
const int SFX_MAX = 40;
const char* SFX_NAMES[SFX_COUNT] = {"COIN", "JUMP", "POWER UP", "BONUS", "LASER", "BOOM", "FANFARE", "YOU LOSE"};
struct SfxStep { uint16_t f; uint8_t ms; };   // f = 0 is a rest
SfxStep sfxBuf[SFX_MAX];
int sfxN = 0;
int sfxPos = 0;
int sfxSel = 0;
int sfxHue = 0;
bool sfxPlaying = false;
unsigned long sfxNext = 0;

// ---------------- Toys state ----------------
const char* toyTabNames[] = {"8-BALL", "DICE", "COIN"};
int toyTab = 0;
bool toyBusy = false;
bool toyDirty = true;
unsigned long toyStart = 0;
const unsigned long TOY_ANIM_MS = 1400;
unsigned long lastToyMove = 0, lastToyDraw = 0, lastToyTick = 0;
unsigned long toyFlashUntil = 0;
Color toyFlashColor = {0, 0, 0};
const char* BALL_TEXT[] = {"YES!", "DEFINITELY", "COUNT ON IT", "LOOKS GOOD", "SIGNS SAY YES", "WITHOUT A DOUBT",
                           "ASK AGAIN LATER", "HAZY, TRY AGAIN", "CAN'T SAY NOW", "MAYBE...",
                           "NO WAY", "VERY DOUBTFUL", "DON'T BET ON IT", "NOT LIKELY", "MY ANSWER IS NO", "FORGET IT"};
const uint8_t BALL_KIND[] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2};   // 0 yes, 1 maybe, 2 no
const int BALL_COUNT = 16;
int ballAnswer = -1;                 // -1 = nothing asked yet
int ballPending = 0;
int diceCount = 2;
int diceVal[3] = {1, 1, 1};
int diceTmp[3] = {1, 1, 1};
int coinResult = -1;                 // 0 heads, 1 tails, -1 = not flipped yet
int coinFlips = 0;                   // half-turns in the animation (even = heads)
int coinHeads = 0, coinTails = 0;

// ---------------- Gesture state ----------------
const float MENU_TILT_DEG = 18.0;    // tilt past this (forward / back) to scroll the menu
const float MENU_TILT_SIGN = 1.0;    // set to -1.0 if tilting forward scrolls UP
float menuTiltZero = 0;
bool menuTiltInit = false;
unsigned long lastTiltMove = 0;
unsigned long faceDownSince = 0;
unsigned long gestureCooldown = 0;

// ---------------- Buzzer helpers ----------------
// every sound goes through here, so SOUND: OFF in SETTINGS mutes everything
void sfx(int freq, int durMs) {
  if (soundOn && !gestureAsleep) tone(BUZZER_PIN, freq, durMs);
}

void holdTone(int freq) {          // keeps sounding until stopTone()
  if (soundOn && !gestureAsleep) tone(BUZZER_PIN, freq);
}

void stopTone() {
  noTone(BUZZER_PIN);
}

void beep(int freq, int durMs) {
  sfx(freq, durMs);
}

void doubleBeep() {
  sfx(700, 40);
  delay(60);
  sfx(700, 40);
}

// plays a short tune (blocks while it plays - fine for an alarm)
void playMelody(const int* notes, const int* lens, int count) {
  for (int i = 0; i < count; i++) {
    sfx(notes[i], lens[i]);
    delay(lens[i] + 40);
  }
}

// ---------------- RGB LED driver ----------------
#if RGB_PWM && (ESP_ARDUINO_VERSION_MAJOR < 3)
const int RGB_CH_R = 8;   // LEDC channels 8-10, kept clear of the one tone() uses
const int RGB_CH_G = 9;
const int RGB_CH_B = 10;
#endif

int lastR = -1, lastG = -1, lastB = -1;   // -1 = "unknown", forces the next write

// gamma-correct a 0-255 colour value, scale it by the channel gain and apply the LED polarity
uint8_t calib(uint8_t v, uint8_t gain) {
  float x = powf(v / 255.0f, RGB_GAMMA);
  uint8_t c = (uint8_t)(x * gain + 0.5f);
  return RGB_COMMON_ANODE ? (uint8_t)(255 - c) : c;   // anode: duty 255 = OFF; cathode: duty 0 = OFF
}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  if (r == lastR && g == lastG && b == lastB) return;
  lastR = r; lastG = g; lastB = b;
#if RGB_PWM
  r = (uint8_t)((uint16_t)r * ledScale / 255);   // global LED brightness from SETTINGS
  g = (uint8_t)((uint16_t)g * ledScale / 255);
  b = (uint8_t)((uint16_t)b * ledScale / 255);
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(RGB_RED_PIN, calib(r, RGB_GAIN_R));
  ledcWrite(RGB_GREEN_PIN, calib(g, RGB_GAIN_G));
  ledcWrite(RGB_BLUE_PIN, calib(b, RGB_GAIN_B));
  #else
  ledcWrite(RGB_CH_R, calib(r, RGB_GAIN_R));
  ledcWrite(RGB_CH_G, calib(g, RGB_GAIN_G));
  ledcWrite(RGB_CH_B, calib(b, RGB_GAIN_B));
  #endif
#else
  digitalWrite(RGB_RED_PIN, ((r > 60) == (bool)RGB_COMMON_ANODE) ? LOW : HIGH);
  digitalWrite(RGB_GREEN_PIN, ((g > 60) == (bool)RGB_COMMON_ANODE) ? LOW : HIGH);
  digitalWrite(RGB_BLUE_PIN, ((b > 60) == (bool)RGB_COMMON_ANODE) ? LOW : HIGH);
#endif
}

void setRGB(Color c) {
  setRGB(c.r, c.g, c.b);
}

void rgbInit() {
#if RGB_PWM
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(RGB_RED_PIN, 5000, 8);
  ledcAttach(RGB_GREEN_PIN, 5000, 8);
  ledcAttach(RGB_BLUE_PIN, 5000, 8);
  #else
  ledcSetup(RGB_CH_R, 5000, 8);
  ledcAttachPin(RGB_RED_PIN, RGB_CH_R);
  ledcSetup(RGB_CH_G, 5000, 8);
  ledcAttachPin(RGB_GREEN_PIN, RGB_CH_G);
  ledcSetup(RGB_CH_B, 5000, 8);
  ledcAttachPin(RGB_BLUE_PIN, RGB_CH_B);
  #endif
#else
  pinMode(RGB_RED_PIN, OUTPUT);
  pinMode(RGB_GREEN_PIN, OUTPUT);
  pinMode(RGB_BLUE_PIN, OUTPUT);
#endif
  lastR = lastG = lastB = -1;
  setRGB(0, 0, 0);
}

void rgbRefresh() {            // forget the cached value so the next setRGB() rewrites the pins
  lastR = lastG = lastB = -1;
}

Color scaleColor(Color c, uint8_t level) {
  Color o;
  o.r = (uint8_t)((c.r * level) / 255);
  o.g = (uint8_t)((c.g * level) / 255);
  o.b = (uint8_t)((c.b * level) / 255);
  return o;
}

Color hueColor(int hue) {   // hue 0..359 -> fully saturated colour
  int h = hue % 360;
  int region = h / 60;
  int rem = (h % 60) * 255 / 60;
  Color c = {0, 0, 0};
  switch (region) {
    case 0: c = {255, (uint8_t)rem, 0}; break;
    case 1: c = {(uint8_t)(255 - rem), 255, 0}; break;
    case 2: c = {0, 255, (uint8_t)rem}; break;
    case 3: c = {0, (uint8_t)(255 - rem), 255}; break;
    case 4: c = {(uint8_t)rem, 0, 255}; break;
    default: c = {255, 0, (uint8_t)(255 - rem)}; break;
  }
  return c;
}

void updateRGB() {
  // 1) you lost a game: LED goes red and stays red until you restart or leave
  bool lost = (state == GAME && gameOver) || (state == SHOOTER && shooterOver);
  if (lost) {
#if GAMEOVER_BLINK
    setRGB(((millis() % 400) < 200) ? C_RED : C_OFF);
#else
    setRGB(C_RED);
#endif
    return;
  }

  // 2) the LED is OFF by default; only these modes light it
  //    EMO: colour follows the face | TORCH: your colour | STOPWATCH: green running / red stopped
  if (state == EMO) {
    if (emoBannerUntil != 0 || emoIndex == EMO_DIZZY || spinDizzy) {
      setRGB(scaleColor(hueColor((millis() / 4) % 360), EMO_LED_LEVEL));   // rainbow
    } else {
      setRGB(scaleColor(emoColors[emoIndex], EMO_LED_LEVEL));
    }
  } else if (state == TORCH) {
    if (torchOn) {
      Color c = (torchColorIdx == TORCH_RAINBOW) ? hueColor((millis() / 15) % 360)
                                                  : torchColors[torchColorIdx].c;
      setRGB(scaleColor(c, torchLevels[torchLevelIdx]));
    } else {
      setRGB(C_OFF);
    }
  } else if (state == STOPWATCH) {
    if (swRunning) setRGB(scaleColor(C_GREEN, 200));        // started = green
    else if (swElapsed > 0) setRGB(scaleColor(C_RED, 200)); // stopped = red
    else setRGB(C_OFF);                                     // fresh / reset = off
  } else if (state == TIMER) {
    // green while working, red during the break, blinking while paused / when the time is up
    bool blinkOff = ((millis() / 400) % 2) == 1;
    if (timerPhase == TP_WORK) {
      if (timerPaused && blinkOff) setRGB(C_OFF); else setRGB(scaleColor(C_GREEN, 200));
    } else if (timerPhase == TP_BREAK) {
      if (timerPaused && blinkOff) setRGB(C_OFF); else setRGB(scaleColor(C_RED, 200));
    } else if (timerPhase == TP_DONE) {
      if (blinkOff) setRGB(C_OFF); else setRGB(C_RED);
    } else {
      setRGB(C_OFF);
    }
  } else if (state == LEVEL) {
    if (levelFlat && mpuOK) setRGB(scaleColor(C_GREEN, 200)); else setRGB(C_OFF);
  } else if (state == REACTION) {
    if (rxPhase == RX_GO) setRGB(255, 255, 255);                         // the "GO" flash
    else if (rxPhase == RX_EARLY) setRGB(C_RED);                         // jumped the gun
    else if (rxPhase == RX_RESULT && rxNewBest) setRGB(scaleColor(C_GREEN, 200));
    else setRGB(C_OFF);
  } else if (state == MUSIC) {
    setRGB(musLed);                                                      // each MUSIC tab decides its own colour
  } else if (state == TOYS) {
    if (millis() < toyFlashUntil) setRGB(toyFlashColor); else setRGB(C_OFF);
  } else if (state == SETTINGS) {
    if (setSel == 1) setRGB(255, 255, 255); else setRGB(C_OFF);          // white preview while you pick brightness
  } else {
    setRGB(C_OFF);
  }
}

// ---------------- Drawing ----------------
void drawMenu() {
  if (selected < menuTop) menuTop = selected;                       // keep the selection on screen
  if (selected >= menuTop + MENU_VISIBLE) menuTop = selected - MENU_VISIBLE + 1;
  display.clearDisplay();
  for (int r = 0; r < MENU_VISIBLE; r++) {
    int i = menuTop + r;
    if (i >= MENU_COUNT) break;
    int y = r * ROW_H;
    if (i == selected) {
      display.fillRect(0, y, 123, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y + 1);
    display.setTextSize(1);
    display.println(menuItems[i]);
  }
  if (MENU_COUNT > MENU_VISIBLE) {                                  // scroll bar on the right edge
    int trackH = MENU_VISIBLE * ROW_H;
    int thumbH = trackH * MENU_VISIBLE / MENU_COUNT;
    int thumbY = (trackH - thumbH) * menuTop / (MENU_COUNT - MENU_VISIBLE);
    display.drawFastVLine(126, 0, trackH, SSD1306_WHITE);
    display.fillRect(125, thumbY, 3, thumbH, SSD1306_WHITE);
  }
  display.display();
}

// ----- TEMP -----
float toUnit(float c) {
  return useF ? (c * 9.0f / 5.0f + 32.0f) : c;
}

char unitChar() {
  return useF ? 'F' : 'C';
}

// dew point (Magnus formula): the temperature at which the air would be saturated
float dewPointC(float t, float h) {
  if (h < 1.0f) h = 1.0f;
  float g = logf(h / 100.0f) + (17.62f * t) / (243.12f + t);
  return 243.12f * g / (17.62f - g);
}

// "feels like": heat index (NWS formula) when it is warm, otherwise just the air temperature
float feelsLikeC(float t, float h) {
  if (t < 26.7f) return t;
  float tf = t * 9.0f / 5.0f + 32.0f;
  float hi = -42.379f + 2.04901523f * tf + 10.14333127f * h - 0.22475541f * tf * h
             - 0.00683783f * tf * tf - 0.05481717f * h * h + 0.00122874f * tf * tf * h
             + 0.00085282f * tf * h * h - 0.00000199f * tf * tf * h * h;
  float hc = (hi - 32.0f) * 5.0f / 9.0f;
  return hc > t ? hc : t;
}

const char* comfortWord(float t, float h, float dp) {
  if (t < 16) return "COLD";
  if (t < 20) return "COOL";
  if (t <= 26) {
    if (dp > 18) return "MUGGY";
    if (h < 30) return "DRY";
    return "COMFY";
  }
  if (t <= 30) return (dp > 18) ? "STICKY" : "WARM";
  return "HOT";
}

// 1 = rising, -1 = falling, 0 = steady, 2 = not enough data yet.  Compares ~5 minutes of 20 s averages.
int tempTrend() {
  if (histCount < 9) return 2;
  int back = (histCount > 15) ? 15 : histCount - 1;
  float d = tempHist[histCount - 1] - tempHist[histCount - 1 - back];
  if (d > 0.5f) return 1;
  if (d < -0.5f) return -1;
  return 0;
}

void resetTempStats() {
  histCount = 0;
  smoothCount = 0;
  smoothPos = 0;
  curT = NAN;
  curH = NAN;
  minT = NAN; maxT = NAN; minH = NAN; maxH = NAN;
  sumT = 0;
  cntT = 0;
  bucketSumT = 0; bucketSumH = 0; bucketN = 0;
  tempDirty = true;
}

void pushHistory(float t, float h) {
  if (histCount < TEMP_HIST) {
    tempHist[histCount] = t;
    humHist[histCount] = h;
    histCount++;
  } else {
    memmove(tempHist, tempHist + 1, (TEMP_HIST - 1) * sizeof(float));
    memmove(humHist, humHist + 1, (TEMP_HIST - 1) * sizeof(float));
    tempHist[TEMP_HIST - 1] = t;
    humHist[TEMP_HIST - 1] = h;
  }
}

void sampleTemp() {
  lastTempRead = millis();
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    if (tempFails < 100) tempFails++;
    tempDirty = true;
    return;
  }
  tempFails = 0;
  everGood = true;
  lastGoodMs = millis();

  t += tOffTenths / 10.0f;                         // calibration offsets from SETTINGS
  h = fminf(100.0f, fmaxf(0.0f, h + hOff));

  smoothT[smoothPos] = t;                          // short moving average hides sensor jitter
  smoothH[smoothPos] = h;
  smoothPos = (smoothPos + 1) % TEMP_SMOOTH_N;
  if (smoothCount < TEMP_SMOOTH_N) smoothCount++;
  float st = 0, sh = 0;
  for (int i = 0; i < smoothCount; i++) { st += smoothT[i]; sh += smoothH[i]; }
  curT = st / smoothCount;
  curH = sh / smoothCount;

  if (isnan(minT) || curT < minT) minT = curT;
  if (isnan(maxT) || curT > maxT) maxT = curT;
  if (isnan(minH) || curH < minH) minH = curH;
  if (isnan(maxH) || curH > maxH) maxH = curH;
  sumT += t;
  cntT++;

  bucketSumT += t;                                 // one graph point per 20 s (first one immediately)
  bucketSumH += h;
  bucketN++;
  if (histCount == 0 || millis() - bucketStart >= TEMP_BUCKET_MS) {
    pushHistory(bucketSumT / bucketN, bucketSumH / bucketN);
    bucketSumT = 0; bucketSumH = 0; bucketN = 0;
    bucketStart = millis();
  }
  tempDirty = true;
}

// logs in the background in every mode except the games / reaction tester (which need steady timing)
void tempBackground() {
  if (state == GAME || state == SHOOTER || state == REACTION || state == MUSIC) return;
  if (lastTempRead == 0 || millis() - lastTempRead >= TEMP_INTERVAL_MS) sampleTemp();
}

void drawHeader(const char* left, const char* right) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(left);
  display.setCursor(128 - (int)strlen(right) * 6, 0);
  display.print(right);
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
}

void drawTempHints() {
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("SEL:");
  display.print(useF ? "C" : "F");
  const char* r = "hold FIRE:reset";
  display.setCursor(128 - (int)strlen(r) * 6, 56);
  display.print(r);
}

// page 1: big readings, trend, feels-like, dew point
void drawTempNow() {
  float dp = dewPointC(curT, curH);
  float feels = feelsLikeC(curT, curH);
  bool stale = (millis() - lastGoodMs) > TEMP_STALE_MS;
  drawHeader("NOW 1/3", stale ? "STALE" : comfortWord(curT, curH, dp));

  char buf[16];
  char u[2] = {unitChar(), 0};

  snprintf(buf, sizeof(buf), "%d", (int)roundf(toUnit(curT)));       // temperature
  display.setTextSize(3);
  display.setCursor(0, 13);
  display.print(buf);
  int w = (int)strlen(buf) * 18;
  display.setTextSize(2);
  display.setCursor(w + 2, 13);
  display.print(u);

  snprintf(buf, sizeof(buf), "%d", (int)roundf(curH));               // humidity
  display.setTextSize(3);
  display.setCursor(72, 13);
  display.print(buf);
  w = (int)strlen(buf) * 18;
  if (72 + w + 12 <= 128) {
    display.setTextSize(2);
    display.setCursor(72 + w + 2, 13);
    display.print("%");
  }

  int tr = tempTrend();                                              // trend arrow + word
  if (tr == 1) display.fillTriangle(3, 39, 0, 45, 6, 45, SSD1306_WHITE);
  else if (tr == -1) display.fillTriangle(0, 39, 6, 39, 3, 45, SSD1306_WHITE);
  else if (tr == 0) display.fillRect(0, 41, 7, 3, SSD1306_WHITE);
  else display.drawFastHLine(0, 42, 7, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 38);
  display.print(tr == 1 ? "RISING" : (tr == -1 ? "FALLING" : (tr == 0 ? "STEADY" : "measuring")));

  snprintf(buf, sizeof(buf), "feels %d%c", (int)roundf(toUnit(feels)), unitChar());
  display.setCursor(128 - (int)strlen(buf) * 6, 38);
  display.print(buf);

  snprintf(buf, sizeof(buf), "lo%d hi%d", (int)roundf(toUnit(minT)), (int)roundf(toUnit(maxT)));
  display.setCursor(0, 47);
  display.print(buf);
  snprintf(buf, sizeof(buf), "dew %d%c", (int)roundf(toUnit(dp)), unitChar());
  display.setCursor(128 - (int)strlen(buf) * 6, 47);
  display.print(buf);

  drawTempHints();
}

// page 2: labelled history graph (temperature or humidity)
void drawTempGraph() {
  const bool H = graphHum;
  drawHeader(H ? "HUMID 2/3" : "TEMP 2/3", "FIRE:T/H");

  const int PX0 = 26, PX1 = 125, PY0 = 12, PY1 = 52;
  int n = histCount;
  if (n < 2) {
    display.setCursor(10, 28);
    display.print("collecting data...");
    return;
  }

  const float* src = H ? humHist : tempHist;
  float lo = src[0], hi = src[0];
  for (int i = 1; i < n; i++) {
    if (src[i] < lo) lo = src[i];
    if (src[i] > hi) hi = src[i];
  }
  float minSpan = H ? 10.0f : 4.0f;                 // keep sensor noise from looking dramatic
  if (hi - lo < minSpan) {
    float mid = (hi + lo) / 2.0f;
    lo = mid - minSpan / 2.0f;
    hi = mid + minSpan / 2.0f;
  }

  display.drawFastVLine(PX0 - 2, PY0, PY1 - PY0 + 1, SSD1306_WHITE);             // axes
  display.drawFastHLine(PX0 - 2, PY1, PX1 - PX0 + 3, SSD1306_WHITE);
  for (int x = PX0; x <= PX1; x += 4) display.drawPixel(x, (PY0 + PY1) / 2, SSD1306_WHITE);

  char lb[10];
  if (H) snprintf(lb, sizeof(lb), "%d%%", (int)roundf(hi));
  else snprintf(lb, sizeof(lb), "%d%c", (int)roundf(toUnit(hi)), unitChar());
  display.setCursor(0, PY0 - 1);
  display.print(lb);
  snprintf(lb, sizeof(lb), "%d", (int)roundf(H ? lo : toUnit(lo)));
  display.setCursor(0, PY1 - 7);
  display.print(lb);

  int px = 0, py = 0;
  for (int i = 0; i < n; i++) {                      // newest sample on the right
    int x = PX1 - (n - 1 - i);
    int y = PY1 - 2 - (int)((src[i] - lo) / (hi - lo) * (PY1 - PY0 - 4));
    if (i > 0) display.drawLine(px, py, x, y, SSD1306_WHITE);
    px = x;
    py = y;
  }
  display.fillCircle(px, py, 2, SSD1306_WHITE);

  int spanSec = (n - 1) * (int)(TEMP_BUCKET_MS / 1000);
  char xb[10];
  if (spanSec < 120) snprintf(xb, sizeof(xb), "-%ds", spanSec);
  else snprintf(xb, sizeof(xb), "-%dm", spanSec / 60);
  display.setCursor(PX0 - 2, 56);
  display.print(xb);
  display.setCursor(PX1 - 17, 56);
  display.print("now");
}

void statRow(int i, const char* label, const char* value) {
  int y = 12 + i * 9;
  display.setCursor(0, y);
  display.print(label);
  display.setCursor(128 - (int)strlen(value) * 6, y);
  display.print(value);
}

// page 3: derived values and ranges since power-up (or the last reset)
void drawTempStats() {
  float dp = dewPointC(curT, curH);
  float feels = feelsLikeC(curT, curH);
  float avg = cntT ? (float)(sumT / cntT) : curT;
  char hb[16];
  snprintf(hb, sizeof(hb), "n=%lu", cntT);
  drawHeader("STATS 3/3", hb);

  char v[20];
  snprintf(v, sizeof(v), "%d%c", (int)roundf(toUnit(dp)), unitChar());
  statRow(0, "DEW POINT", v);
  snprintf(v, sizeof(v), "%d%c", (int)roundf(toUnit(feels)), unitChar());
  statRow(1, "FEELS LIKE", v);
  snprintf(v, sizeof(v), "%.1f%c", toUnit(avg), unitChar());
  statRow(2, "AVERAGE", v);
  snprintf(v, sizeof(v), "%d-%d%c", (int)roundf(toUnit(minT)), (int)roundf(toUnit(maxT)), unitChar());
  statRow(3, "TEMP RANGE", v);
  snprintf(v, sizeof(v), "%d-%d%%", (int)roundf(minH), (int)roundf(maxH));
  statRow(4, "HUM RANGE", v);

  drawTempHints();
}

void drawTemp() {
  tempDirty = false;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  bool lost = (tempFails >= TEMP_FAIL_LIMIT);
  if (lost || !everGood || isnan(curT) || isnan(curH)) {
    drawHeader("TEMP", lost ? "ERROR" : "...");
    if (lost) {
      display.setTextSize(2);
      display.setCursor(10, 14);
      display.print("NO SENSOR");
      display.setTextSize(1);
      display.setCursor(0, 36);
      display.print("DHT11: no response");
      display.setCursor(0, 46);
      display.print("check DATA wire (D15)");
      display.setCursor(0, 56);
      display.print("and 3.3V / GND");
    } else {
      display.setCursor(16, 28);
      display.print("Reading sensor...");
    }
    display.display();
    return;
  }

  if (tempPage == 0) drawTempNow();
  else if (tempPage == 1) drawTempGraph();
  else drawTempStats();
  display.display();
}

// ----- TORCH -----
void drawTorch() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("TORCH");
  display.setCursor(104, 0);
  display.print(torchOn ? "ON" : "OFF");

  // colour name, with arrows
  const char* name = torchColors[torchColorIdx].name;
  int nameW = (int)strlen(name) * 12;
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print("<");
  display.setCursor(116, 14);
  display.print(">");
  display.setCursor((128 - nameW) / 2, 14);
  display.print(name);

  // brightness bar: 5 blocks
  for (int i = 0; i < TORCH_LEVEL_COUNT; i++) {
    int x = 24 + i * 16;
    if (i <= torchLevelIdx) display.fillRect(x, 36, 12, 6, SSD1306_WHITE);
    else display.drawRect(x, 36, 12, 6, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(0, 47);
  display.print("L/R color U/D light");
  display.setCursor(0, 56);
  display.print("SEL on/off BACK menu");
  display.display();
}

void setTorch(bool on) {
  torchOn = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
  drawTorch();
}

// ----- STOPWATCH -----
unsigned long swCurrentElapsed() {
  if (swRunning) return swElapsed + (millis() - swStartTime);
  return swElapsed;
}

void drawStopwatch() {
  unsigned long ms = swCurrentElapsed();
  unsigned long totalSec = ms / 1000;
  unsigned int minutes = totalSec / 60;
  unsigned int seconds = totalSec % 60;
  unsigned int hundredths = (ms % 1000) / 10;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("STOPWATCH");
  display.setCursor(78, 0);
  display.println("BACK:menu");

  display.setTextSize(2);
  display.setCursor(0, 20);
  char buf[12];
  sprintf(buf, "%02u:%02u.%02u", minutes, seconds, hundredths);
  display.println(buf);

  display.setTextSize(1);
  display.setCursor(0, 44);
  display.println(swRunning ? "SEL: pause" : "SEL: start");
  display.setCursor(0, 54);
  display.println("hold FIRE: reset");

  display.display();
}

void swToggle() {
  if (swRunning) {
    swElapsed += millis() - swStartTime;
    swRunning = false;
    beep(900, 50);
  } else {
    swStartTime = millis();
    swRunning = true;
    beep(1500, 50);
  }
  drawStopwatch();
  lastSwDraw = millis();
}

void swReset() {
  swRunning = false;
  swElapsed = 0;
  doubleBeep();
  drawStopwatch();
  lastSwDraw = millis();
}

// ----- EMO -----
void drawRobotEye(int x, int y, int w, int h, int r, int topCutSide, int cutAmt) {
  display.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);
  if (topCutSide == 1 && cutAmt > 0) {
    display.fillTriangle(x, y, x + cutAmt, y, x, y + cutAmt, SSD1306_BLACK);
  } else if (topCutSide == 2 && cutAmt > 0) {
    display.fillTriangle(x + w - cutAmt, y, x + w, y, x + w, y + cutAmt, SSD1306_BLACK);
  }
}

void updateBlink() {
  unsigned long now = millis();
  if (now >= nextBlinkMs) {
    blinkUntil = now + 130;
    nextBlinkMs = now + random(2500, 6500);
  }
}

// pupil + eyelid on top of an eye that was just drawn; they follow the tilt (gazeX / gazeY)
void drawEyeDetail(int x, int y, int w, int h, bool blinking) {
  if (blinking) {                                   // lids close to a thin slit
    int s = (h * 45) / 100;
    display.fillRect(x, y, w, s, SSD1306_BLACK);
    display.fillRect(x, y + h - s, w, s, SSD1306_BLACK);
    return;
  }
  int lid = (gazeY > 0) ? (int)(gazeY * h * 0.35f) : 0;   // looking down = the lid droops
  if (lid > 0) display.fillRect(x, y, w, lid, SSD1306_BLACK);
  const int pr = 5;
  int mx = w / 2 - pr - 3;
  int my = h / 2 - pr - 2;
  if (my < 1) my = 1;
  int px = x + w / 2 + (int)roundf(gazeX * mx);
  int py = y + h / 2 + (int)roundf(gazeY * my);
  display.fillCircle(px, py, pr, SSD1306_BLACK);
  display.fillCircle(px - 2, py - 2, 1, SSD1306_WHITE);    // little glint
}

// both eyes for an expression: 0 happy, 1 sad, 2 angry, 3 surprised, 4 wink, anything else = neutral
void drawEyesExpr(int idx, int topY) {
  int eyeW = 38, eyeH = 30, gap = 10;
  int totalW = eyeW * 2 + gap;
  int leftX = (128 - totalW) / 2;
  int rightX = leftX + eyeW + gap;
  int r = 10;
  int le[4], ri[4];                  // x, y, w, h of each eye (for the pupils)
  bool detailL = true, detailR = true;

  switch (idx) {
    case 0:   // HAPPY
      le[0] = leftX;  le[1] = topY; le[2] = eyeW; le[3] = eyeH;
      ri[0] = rightX; ri[1] = topY; ri[2] = eyeW; ri[3] = eyeH;
      drawRobotEye(le[0], le[1], le[2], le[3], r, 2, 14);
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], r, 1, 14);
      break;
    case 1:   // SAD
      le[0] = leftX;  le[1] = topY + 6; le[2] = eyeW; le[3] = eyeH - 6;
      ri[0] = rightX; ri[1] = topY + 6; ri[2] = eyeW; ri[3] = eyeH - 6;
      drawRobotEye(le[0], le[1], le[2], le[3], r, 1, 14);
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], r, 2, 14);
      break;
    case 2:   // ANGRY
      le[0] = leftX + 4;  le[1] = topY + 4; le[2] = eyeW - 8; le[3] = eyeH - 10;
      ri[0] = rightX - 4; ri[1] = topY + 4; ri[2] = eyeW - 8; ri[3] = eyeH - 10;
      drawRobotEye(le[0], le[1], le[2], le[3], 2, 2, 18);
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], 2, 1, 18);
      break;
    case 3:   // SURPRISED
      le[0] = leftX - 4;  le[1] = topY - 4; le[2] = eyeW + 8; le[3] = eyeH + 8;
      ri[0] = rightX - 4; ri[1] = topY - 4; ri[2] = eyeW + 8; ri[3] = eyeH + 8;
      drawRobotEye(le[0], le[1], le[2], le[3], 14, 0, 0);
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], 14, 0, 0);
      break;
    case 4:   // WINK
      le[0] = leftX;  le[1] = topY; le[2] = eyeW; le[3] = eyeH;
      ri[0] = rightX; ri[1] = topY; ri[2] = eyeW; ri[3] = eyeH;
      display.fillRoundRect(leftX, topY + eyeH / 2 - 3, eyeW, 6, 3, SSD1306_WHITE);
      detailL = false;
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], r, 1, 14);
      break;
    default:  // NEUTRAL
      le[0] = leftX;  le[1] = topY + 2; le[2] = eyeW; le[3] = eyeH - 4;
      ri[0] = rightX; ri[1] = topY + 2; ri[2] = eyeW; ri[3] = eyeH - 4;
      drawRobotEye(le[0], le[1], le[2], le[3], r, 0, 0);
      drawRobotEye(ri[0], ri[1], ri[2], ri[3], r, 0, 0);
      break;
  }

  bool bl = (millis() < blinkUntil);
  if (detailL) drawEyeDetail(le[0], le[1], le[2], le[3], bl);
  if (detailR) drawEyeDetail(ri[0], ri[1], ri[2], ri[3], bl);
}

void drawEmoFace(int idx) {
  display.clearDisplay();
  drawEyesExpr(idx, 10);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print(emoNames[idx]);

  display.display();
}

void drawDizzy();

void drawEmo() {
  if (emoIndex == EMO_DIZZY || spinDizzy) drawDizzy();   // the spiral face: unlocked by shaking, or just spin the board
  else drawEmoFace(emoIndex);
}

// spiral eye: a circle with a rotating spiral inside it
void drawSpiral(int cx, int cy, int maxR, float phase) {
  const int steps = 36;
  const float turns = 2.5;
  int px = cx, py = cy;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    float ang = phase + t * turns * 2.0 * PI;
    float r = maxR * t;
    int x = cx + (int)(cos(ang) * r);
    int y = cy + (int)(sin(ang) * r);
    display.drawLine(px, py, x, y, SSD1306_WHITE);
    px = x;
    py = y;
  }
  display.drawCircle(cx, cy, maxR, SSD1306_WHITE);
}

void drawDizzy() {
  display.clearDisplay();
  float t = millis() / 1000.0;
  int sway = (int)(sin(t * 6.0) * 5.0);   // the whole face wobbles
  float phase = t * 7.0;                  // spirals spin
  drawSpiral(34 + sway, 28, 19, phase);
  drawSpiral(94 + sway, 28, 19, phase);

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print("DIZZY...");
  display.setCursor(104, 56);
  display.print("@_@");
  display.display();
}

void drawUnlockBanner() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(13, 4);
  display.print("NEW EMO UNLOCKED!");
  display.setTextSize(2);
  display.setCursor(34, 24);
  display.print("DIZZY");
  display.setTextSize(1);
  display.setCursor(7, 50);
  display.print("find it in EMO list");
  display.display();
}

void unlockDizzy() {
  dizzyUnlocked = true;
  emoBannerUntil = millis() + 2500;
  drawUnlockBanner();
  // short fanfare (blocks ~0.4 s, fine for a one-off)
  sfx(880, 90);
  delay(110);
  sfx(1175, 90);
  delay(110);
  sfx(1568, 180);
  delay(200);
}

void drawIdle(bool eyesOpen) {
  display.clearDisplay();

  int eyeW = 38, gap = 10;
  int totalW = eyeW * 2 + gap;
  int leftX = (128 - totalW) / 2;
  int rightX = leftX + eyeW + gap;
  int topY = 16;
  int openH = 26;

  if (eyesOpen) {
    drawRobotEye(leftX, topY, eyeW, openH, 10, 2, 12);
    drawRobotEye(rightX, topY, eyeW, openH, 10, 1, 12);
    drawEyeDetail(leftX, topY, eyeW, openH, false);      // pupils follow the tilt
    drawEyeDetail(rightX, topY, eyeW, openH, false);
  } else {
    int closedY = topY + openH / 2 - 2;
    display.fillRoundRect(leftX, closedY, eyeW, 4, 2, SSD1306_WHITE);
    display.fillRoundRect(rightX, closedY, eyeW, 4, 2, SSD1306_WHITE);
  }

  display.display();
}

void bootAnimation() {
  int eyeW = 38, gap = 10;
  int totalW = eyeW * 2 + gap;
  int leftX = (128 - totalW) / 2;
  int rightX = leftX + eyeW + gap;
  int centerY = 32;
  int maxH = 30;

  // eyes grow from a thin closed line to full open height
  for (int h = 4; h <= maxH; h += 4) {
    display.clearDisplay();
    int y = centerY - h / 2;
    display.fillRoundRect(leftX, y, eyeW, h, min(10, h / 2), SSD1306_WHITE);
    display.fillRoundRect(rightX, y, eyeW, h, min(10, h / 2), SSD1306_WHITE);
    display.display();
    delay(60);
  }

  beep(1200, 80);
  delay(150);

  // settle into the happy-eye tilt once fully open
  display.clearDisplay();
  int topY = centerY - maxH / 2;
  drawRobotEye(leftX, topY, eyeW, maxH, 10, 2, 14);
  drawRobotEye(rightX, topY, eyeW, maxH, 10, 1, 14);
  display.display();
  delay(400);

  // title card + a quick self-check (handy while wiring)
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(6, 20);
  display.println("DESKBUDDY");
  display.setTextSize(1);
  display.setCursor(6, 46);
  display.print(mpuOK ? "motion OK" : "motion --");
  display.display();
  beep(1800, 100);

  // RGB LED wiring check: the screen names the colour it is trying to show
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(6, 14);
  display.print("LED test:");
  display.setTextSize(2);
  display.setCursor(6, 30);
  display.print("OFF");
  display.display();
  setRGB(0, 0, 0);          // the LED must be completely dark here - if it's lit, flip RGB_COMMON_ANODE
  delay(900);
  const char* testNames[] = {"RED", "GREEN", "BLUE"};
  for (int i = 0; i < 3; i++) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(6, 14);
    display.print("LED test:");
    display.setTextSize(2);
    display.setCursor(6, 30);
    display.print(testNames[i]);
    display.display();
    setRGB(i == 0 ? 255 : 0, i == 1 ? 255 : 0, i == 2 ? 255 : 0);
    delay(450);
  }
  setRGB(0, 0, 0);
  delay(200);
}

// ---------------- Dodge game ----------------
void initGame() {
  for (int i = 0; i < MAX_OBSTACLES; i++) obstacles[i].active = false;
  playerX = 64 - PLAYER_W / 2;
  gameScore = 0;
  gameOver = false;
  gamePaused = false;
  lastGameUpdate = millis();
  lastObstacleSpawn = millis();
  gameSpawnInterval = 900;
  gameFallSpeed = 1.5;
  captureTiltZero();
}

void updateGame() {
  int v = analogRead(VRY_PIN);   // VRY is the physical left/right axis on this joystick
  playerX = steerX(v, PLAYER_W);

  if (millis() - lastObstacleSpawn > gameSpawnInterval) {
    for (int i = 0; i < MAX_OBSTACLES; i++) {
      if (!obstacles[i].active) {
        obstacles[i].active = true;
        obstacles[i].x = random(0, 128 - OBST_SIZE);
        obstacles[i].y = 0;
        break;
      }
    }
    lastObstacleSpawn = millis();
  }

  for (int i = 0; i < MAX_OBSTACLES; i++) {
    if (!obstacles[i].active) continue;
    obstacles[i].y += (int)gameFallSpeed;

    if (obstacles[i].y + OBST_SIZE >= 60 &&
        obstacles[i].x + OBST_SIZE > playerX &&
        obstacles[i].x < playerX + PLAYER_W) {
      gameOver = true;
      gameOverAt = millis();
      if (gameScore > highScore) highScore = gameScore;
      beep(200, 300);
      return;   // updateRGB() turns the LED red while the game-over screen is up
    }

    if (obstacles[i].y > 64) {
      obstacles[i].active = false;
      gameScore++;
      beep(2000, 20);
      if (gameSpawnInterval > 350) gameSpawnInterval -= 15;
      gameFallSpeed += 0.03;
    }
  }
}

void drawGame() {
  display.clearDisplay();

  if (gameOver) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(34, 4);
    display.println("GAME OVER");
    display.setCursor(34, 18);
    display.print("Score: ");
    display.println(gameScore);
    display.setCursor(34, 28);
    display.print("Best: ");
    display.println(highScore);
    display.setCursor(4, 42);
    display.println("SEL/FIRE: play again");
    display.setCursor(4, 52);
    display.println("BACK/click: games");
    display.display();
    return;
  }

  if (gamePaused) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(40, 8);
    display.println("PAUSED");
    display.setCursor(10, 22);
    display.print("Score: ");
    display.println(gameScore);
    display.setCursor(10, 38);
    display.println("SEL/tap: resume");
    display.setCursor(10, 48);
    display.println("BACK/hold: exit");
    display.display();
    return;
  }

  display.fillRect(playerX, 60, PLAYER_W, PLAYER_H, SSD1306_WHITE);

  for (int i = 0; i < MAX_OBSTACLES; i++) {
    if (obstacles[i].active) {
      display.fillRect(obstacles[i].x, obstacles[i].y, OBST_SIZE, OBST_SIZE, SSD1306_WHITE);
    }
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("HI ");
  display.print(highScore);

  display.setCursor(96, 0);
  display.print(gameScore);

  display.display();
}

// ---------------- Shooter game ----------------
void initShooter() {
  for (int i = 0; i < MAX_BULLETS; i++) bullets[i].active = false;
  for (int i = 0; i < MAX_ENEMIES; i++) enemies[i].active = false;
  shipX = 64 - SHIP_W / 2;
  shooterScore = 0;
  shooterLives = 3;
  shooterOver = false;
  shooterPaused = false;
  lastShooterUpdate = millis();
  lastEnemySpawn = millis();
  enemySpawnInterval = 1000;
  enemyFallSpeed = 1.0;
  lastShotTime = 0;
  fireQueued = false;
  captureTiltZero();
}

void updateShooter() {
  // ship follows tilt (or the joystick left/right axis, VRY)
  int v = analogRead(VRY_PIN);
  shipX = steerX(v, SHIP_W);

  // fire: FIRE button (tap or hold), or push the joystick toward "up" (VRX), rate-limited
  int vx = analogRead(VRX_PIN);
  bool wantFire = (vx < 1200) || fireQueued || (btnFire.stable == LOW);
  fireQueued = false;

  if (wantFire && millis() - lastShotTime > SHOT_COOLDOWN) {
    for (int i = 0; i < MAX_BULLETS; i++) {
      if (!bullets[i].active) {
        bullets[i].active = true;
        bullets[i].x = shipX + SHIP_W / 2 - BULLET_SIZE / 2;
        bullets[i].y = 58;
        lastShotTime = millis();
        beep(2500, 15);
        break;
      }
    }
  }

  for (int i = 0; i < MAX_BULLETS; i++) {
    if (!bullets[i].active) continue;
    bullets[i].y -= 4;
    if (bullets[i].y < 0) bullets[i].active = false;
  }

  if (millis() - lastEnemySpawn > enemySpawnInterval) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
      if (!enemies[i].active) {
        enemies[i].active = true;
        enemies[i].x = random(0, 128 - ENEMY_SIZE);
        enemies[i].y = 0;
        break;
      }
    }
    lastEnemySpawn = millis();
  }

  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (!enemies[i].active) continue;
    enemies[i].y += (int)enemyFallSpeed;

    for (int b = 0; b < MAX_BULLETS; b++) {
      if (!bullets[b].active) continue;
      if (bullets[b].x < enemies[i].x + ENEMY_SIZE &&
          bullets[b].x + BULLET_SIZE > enemies[i].x &&
          bullets[b].y < enemies[i].y + ENEMY_SIZE &&
          bullets[b].y + BULLET_SIZE > enemies[i].y) {
        enemies[i].active = false;
        bullets[b].active = false;
        shooterScore++;
        beep(1800, 30);
        if (enemySpawnInterval > 400) enemySpawnInterval -= 20;
        enemyFallSpeed += 0.05;
        break;
      }
    }

    if (!enemies[i].active) continue; // destroyed this tick

    if (enemies[i].y + ENEMY_SIZE >= 58) {
      enemies[i].active = false;
      shooterLives--;
      beep(300, 150);
      if (shooterLives <= 0) {
        shooterOver = true;
        gameOverAt = millis();
        if (shooterScore > shooterHighScore) shooterHighScore = shooterScore;
        beep(150, 400);
        return;   // updateRGB() turns the LED red while the game-over screen is up
      }
    }
  }
}

void drawShooter() {
  display.clearDisplay();

  if (shooterOver) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(34, 4);
    display.println("GAME OVER");
    display.setCursor(34, 18);
    display.print("Score: ");
    display.println(shooterScore);
    display.setCursor(34, 28);
    display.print("Best: ");
    display.println(shooterHighScore);
    display.setCursor(4, 42);
    display.println("SEL/FIRE: play again");
    display.setCursor(4, 52);
    display.println("BACK/click: games");
    display.display();
    return;
  }

  if (shooterPaused) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(40, 8);
    display.println("PAUSED");
    display.setCursor(10, 22);
    display.print("Score: ");
    display.println(shooterScore);
    display.setCursor(10, 38);
    display.println("SEL/tap: resume");
    display.setCursor(10, 48);
    display.println("BACK/hold: exit");
    display.display();
    return;
  }

  display.fillTriangle(shipX, 58 + SHIP_H, shipX + SHIP_W, 58 + SHIP_H, shipX + SHIP_W / 2, 58, SSD1306_WHITE);

  for (int i = 0; i < MAX_BULLETS; i++) {
    if (bullets[i].active) display.fillRect(bullets[i].x, bullets[i].y, BULLET_SIZE, 4, SSD1306_WHITE);
  }

  for (int i = 0; i < MAX_ENEMIES; i++) {
    if (enemies[i].active) display.fillRect(enemies[i].x, enemies[i].y, ENEMY_SIZE, ENEMY_SIZE, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("HI ");
  display.print(shooterHighScore);

  display.setCursor(70, 0);
  display.print("L");
  display.print(shooterLives);

  display.setCursor(100, 0);
  display.print(shooterScore);

  display.display();
}

// ---------------- Games menu (DODGE + SHOOTER) ----------------
int gamesSel = 0;
unsigned long lastGamesMove = 0;

void drawGames() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("GAMES");
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
  const char* names[2] = {"DODGE", "SHOOTER"};
  int best[2] = {highScore, shooterHighScore};
  for (int i = 0; i < 2; i++) {
    int y = 13 + i * 19;
    bool sel = (i == gamesSel);
    if (sel) display.fillRect(0, y, 128, 17, SSD1306_WHITE);
    display.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(4, y + 1);
    display.print(names[i]);
    char b[12];
    snprintf(b, sizeof(b), "HI %d", best[i]);
    display.setTextSize(1);
    display.setCursor(124 - (int)strlen(b) * 6, y + 5);
    display.print(b);
  }
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("U/D pick SEL:play");
  display.display();
}

void initGames() {
  lastGamesMove = millis();
  drawGames();
}

void handleGames() {
  if (btnSelect.pressed || btnFire.pressed || btnJoy.pressed) {
    if (btnJoy.pressed) swallowJoyRelease = true;   // the release of this click must not pause the game
    beep(1800, 50);
    if (gamesSel == 0) { state = GAME; initGame(); drawGame(); }
    else { state = SHOOTER; initShooter(); drawShooter(); }
    return;
  }
  int vx = analogRead(VRX_PIN);
  if ((vx < 1200 || vx > 2900) && millis() - lastGamesMove > 250) {
    gamesSel = 1 - gamesSel;
    lastGamesMove = millis();
    beep(1000, 12);
    drawGames();
  }
}

void goToMenu();   // defined in the Navigation section below
void goToGames();  // defined in the Navigation section below

// ---------------- Settings ----------------
void applySettings() {
  ledScale = ledScales[ledIdx];
  shakeThresh = 1.5f + 0.25f * shakeIdx;
  rgbRefresh();
}

void loadSettings() {
  soundOn = prefs.getBool("snd", true);
  ledIdx = constrain((int)prefs.getUChar("led", 3), 0, 3);
  ssIdx = constrain((int)prefs.getUChar("ss", 1), 0, 5);
  shakeIdx = constrain((int)prefs.getUChar("shk", 2), 0, SHAKE_STEPS - 1);
  tOffTenths = constrain((int)prefs.getUChar("toff", 50) - 50, -50, 50);
  hOff = constrain((int)prefs.getUChar("hoff", 15) - 15, -15, 15);
  useF = prefs.getBool("usef", false);
  gesturesOn = prefs.getBool("gest", true);
  rxBest = prefs.getUShort("rbest", 0);
  bpm = constrain((int)prefs.getUShort("bpm", 100), 40, 220);
  applySettings();
}

void saveSettings() {
  prefs.putBool("snd", soundOn);
  prefs.putUChar("led", (uint8_t)ledIdx);
  prefs.putUChar("ss", (uint8_t)ssIdx);
  prefs.putUChar("shk", (uint8_t)shakeIdx);
  prefs.putUChar("toff", (uint8_t)(tOffTenths + 50));
  prefs.putUChar("hoff", (uint8_t)(hOff + 15));
  prefs.putBool("gest", gesturesOn);
}

void resetSettings() {
  soundOn = true;
  ledIdx = 3;
  ssIdx = 1;
  shakeIdx = 2;
  tOffTenths = 0;
  hOff = 0;
  useF = false;
  prefs.putBool("usef", false);
  gesturesOn = true;
  rxBest = 0;
  prefs.putUShort("rbest", 0);
  bpm = 100;
  prefs.putUShort("bpm", 100);
  resetTempStats();
  applySettings();
  saveSettings();
}

void adjustSetting(int dir, bool wrap) {
  switch (setSel) {
    case 0:
      soundOn = !soundOn;
      break;
    case 1:
      ledIdx = wrap ? (ledIdx + 1) % 4 : constrain(ledIdx + dir, 0, 3);
      break;
    case 2:
      ssIdx = wrap ? (ssIdx + 1) % 6 : constrain(ssIdx + dir, 0, 5);
      break;
    case 3:
      shakeIdx = wrap ? (shakeIdx + 1) % SHAKE_STEPS : constrain(shakeIdx + dir, 0, SHAKE_STEPS - 1);
      shakePeak = 0;
      break;
    case 4: {
      int v = tOffTenths + (wrap ? 5 : dir * 5);
      if (wrap && v > 50) v = -50;
      tOffTenths = constrain(v, -50, 50);
      resetTempStats();
      break;
    }
    case 5: {
      int v = hOff + (wrap ? 1 : dir);
      if (wrap && v > 15) v = -15;
      hOff = constrain(v, -15, 15);
      resetTempStats();
      break;
    }
    case 6:
      gesturesOn = !gesturesOn;
      break;
    default:
      return;
  }
  applySettings();
  beep(1200, 20);   // silent when SOUND is off
}

void keepSetVisible() {
  if (setSel < setTop) setTop = setSel;
  if (setSel >= setTop + SET_VISIBLE) setTop = setSel - SET_VISIBLE + 1;
}

void drawSettings() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("SETTINGS");

  char tb[24];
  bool haveTb = false;
  if (setSel == 3) {   // live shake meter: shake the board and watch "pk" to pick a threshold
    if (!mpuOK) snprintf(tb, sizeof(tb), "no MPU");
    else if (millis() < shakeFlashUntil) snprintf(tb, sizeof(tb), "SHAKE!");
    else snprintf(tb, sizeof(tb), "g%.1f pk%.1f", accelG, shakePeak);
    haveTb = true;
  } else if (setSel == 4 || setSel == 5) {   // live preview of the calibrated reading
    float rt = dht.readTemperature();
    float rh = dht.readHumidity();
    if (isnan(rt) || isnan(rh)) snprintf(tb, sizeof(tb), "no sensor");
    else if (setSel == 4) snprintf(tb, sizeof(tb), "%.0f>%.1fC", rt, rt + tOffTenths / 10.0f);
    else snprintf(tb, sizeof(tb), "%.0f>%.0f%%", rh, fminf(100.0f, fmaxf(0.0f, rh + hOff)));
    haveTb = true;
  }
  if (haveTb) {
    display.setCursor(128 - (int)strlen(tb) * 6, 0);
    display.print(tb);
  }

  for (int k = 0; k < SET_VISIBLE; k++) {
    int i = setTop + k;
    if (i >= SET_COUNT) break;
    int ry = 11 + k * 10;
    bool sel = (i == setSel);
    if (sel) {
      display.fillRect(0, ry, 125, 10, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(2, ry + 1);
    display.print(setLabels[i]);

    char vb[12];
    switch (i) {
      case 0: snprintf(vb, sizeof(vb), "%s", soundOn ? "ON" : "OFF"); break;
      case 1: snprintf(vb, sizeof(vb), "%s", ledNames[ledIdx]); break;
      case 2: snprintf(vb, sizeof(vb), "%s", ssNames[ssIdx]); break;
      case 3: snprintf(vb, sizeof(vb), "%.2fg", 1.5f + 0.25f * shakeIdx); break;
      case 4: snprintf(vb, sizeof(vb), "%+.1fC", tOffTenths / 10.0f); break;
      case 5: snprintf(vb, sizeof(vb), "%+d%%", hOff); break;
      case 6: snprintf(vb, sizeof(vb), "%s", gesturesOn ? "ON" : "OFF"); break;
      default: snprintf(vb, sizeof(vb), "SEL"); break;
    }
    display.setCursor(123 - (int)strlen(vb) * 6, ry + 1);
    display.print(vb);
  }

  // scrollbar
  display.setTextColor(SSD1306_WHITE);
  display.drawFastVLine(126, 11, 50, SSD1306_WHITE);
  int th = 50 * SET_VISIBLE / SET_COUNT;
  int ty = 11 + (50 - th) * setTop / (SET_COUNT - SET_VISIBLE);
  display.fillRect(126, ty, 2, th, SSD1306_WHITE);
  display.display();
}

void initSettings() {
  setSel = 0;
  setTop = 0;
  shakePeak = 0;
  lastSetMove = millis();
  drawSettings();
}

void handleSettings() {
  if (btnJoy.pressed) { goToMenu(); return; }

  bool dirty = false;
  const int RESET_ROW = SET_COUNT - 1;
  int vx = analogRead(VRX_PIN);   // up / down  -> pick a row
  int vy = analogRead(VRY_PIN);   // left / right -> change the value

  if (millis() - lastSetMove > 230) {
    if (vx < 1200) {
      setSel = (setSel + SET_COUNT - 1) % SET_COUNT;
      keepSetVisible();
      shakePeak = 0; lastSetMove = millis(); beep(1000, 12); dirty = true;
    } else if (vx > 2900) {
      setSel = (setSel + 1) % SET_COUNT;
      keepSetVisible();
      shakePeak = 0; lastSetMove = millis(); beep(1000, 12); dirty = true;
    } else if (vy < 1200 && setSel != RESET_ROW) {
      adjustSetting(-1, false); lastSetMove = millis(); dirty = true;
    } else if (vy > 2900 && setSel != RESET_ROW) {
      adjustSetting(1, false); lastSetMove = millis(); dirty = true;
    }
  }

  if (btnSelect.pressed || btnFire.pressed) {
    if (setSel == RESET_ROW) {
      resetSettings();
      doubleBeep();
    } else {
      adjustSetting(1, true);
    }
    dirty = true;
  }

  if (setSel == 3) {
    if (accelG > shakePeak) shakePeak = accelG;
    if (shakeTriggered) shakeFlashUntil = millis() + 500;
    if (millis() - lastSetDraw > 80) dirty = true;
  } else if (setSel == 4 || setSel == 5) {
    if (millis() - lastSetDraw > 1000) dirty = true;
  }

  if (dirty) {
    drawSettings();
    lastSetDraw = millis();
  }
}

// ---------------- Timer (pomodoro / countdown) ----------------
const int MEL_WORK[] = {523, 659, 784, 1047};     // work finished: happy rising tune
const int MEL_WORK_LEN[] = {120, 120, 120, 380};
const int MEL_BREAK[] = {784, 659, 523};          // break finished: gentle falling tune
const int MEL_BREAK_LEN[] = {140, 140, 320};

unsigned long timerRemaining() {
  if (timerPaused) return timerRemainMs;
  return (tmrEndMs > millis()) ? (tmrEndMs - millis()) : 0;
}

void startTimerPhase(int p) {   // int, not TimerPhase: Arduino auto-prototypes are placed above the enum
  timerPhase = (TimerPhase)p;
  timerLen = (unsigned long)(p == TP_WORK ? timerWorkMin : timerBreakMin) * 60000UL;
  tmrEndMs = millis() + timerLen;
  timerPaused = false;
  lastTickSec = -1;
}

void drawTimer() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (timerPhase == TP_SETUP) {
    display.setCursor(0, 0);
    display.print("TIMER");
    for (int i = 0; i < 2; i++) {
      int y = 11 + i * 18;
      bool sel = (i == timerField);
      if (sel) {
        display.fillRect(0, y, 128, 17, SSD1306_WHITE);
        display.setTextColor(SSD1306_BLACK);
      } else {
        display.setTextColor(SSD1306_WHITE);
      }
      display.setTextSize(2);
      display.setCursor(4, y + 1);
      display.print(i == 0 ? "WORK" : "BREAK");
      char vb[8];
      int m = (i == 0) ? timerWorkMin : timerBreakMin;
      if (m == 0) snprintf(vb, sizeof(vb), "OFF");
      else snprintf(vb, sizeof(vb), "%dm", m);
      display.setCursor(124 - (int)strlen(vb) * 12, y + 1);
      display.print(vb);
    }
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.print("U/D mins  FIRE: field");
    display.setCursor(0, 56);
    display.print("SEL:start BACK:menu");
    display.display();
    return;
  }

  if (timerPhase == TP_DONE) {
    display.setCursor(0, 0);
    display.print("TIMER");
    display.setTextSize(2);
    display.setCursor(4, 16);
    display.print("TIME'S UP!");
    display.setTextSize(1);
    display.setCursor(0, 40);
    display.print("rounds done: ");
    display.print(timerRounds);
    display.setCursor(0, 56);
    display.print("SEL: new timer");
    display.display();
    return;
  }

  unsigned long rem = timerRemaining();
  unsigned long sec = (rem + 999) / 1000;      // round up, so it starts at 25:00 and ends at 00:01 -> 00:00
  char buf[12];

  display.setCursor(0, 0);
  display.print(timerPhase == TP_WORK ? "WORK" : "BREAK");
  if (timerPaused) {
    display.setCursor(46, 0);
    display.print("PAUSED");
  }
  snprintf(buf, sizeof(buf), "#%d", timerRounds + (timerPhase == TP_WORK ? 1 : 0));
  display.setCursor(128 - (int)strlen(buf) * 6, 0);
  display.print(buf);

  snprintf(buf, sizeof(buf), "%02lu:%02lu", sec / 60, sec % 60);
  display.setTextSize(3);
  display.setCursor(19, 14);
  display.print(buf);

  display.drawRect(2, 44, 124, 8, SSD1306_WHITE);
  int fill = timerLen ? (int)((timerLen - rem) * 120UL / timerLen) : 0;
  display.fillRect(4, 46, fill, 4, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(timerPaused ? "SEL:resume FIRE:stop" : "SEL:pause FIRE:stop");
  display.display();
}

void initTimer() {
  timerPhase = TP_SETUP;
  timerPaused = false;
  timerHoldStart = 0;
  drawTimer();
}

void handleTimer() {
  if (btnJoy.pressed) { goToMenu(); return; }

  // ----- setup: joystick up/down sets the minutes, FIRE swaps WORK <-> BREAK, SELECT starts -----
  if (timerPhase == TP_SETUP) {
    int vx = analogRead(VRX_PIN);
    int dir = 0;
    if (vx < 1200) dir = 1;           // stick up = more minutes
    else if (vx > 2900) dir = -1;     // stick down = fewer

    if (dir == 0) {
      timerHoldStart = 0;
    } else {
      if (timerHoldStart == 0) timerHoldStart = millis();
      bool fast = (millis() - timerHoldStart > 1500);   // hold it and it speeds up (steps of 5)
      if (millis() - lastTimerAdj > (fast ? 90UL : 220UL)) {
        lastTimerAdj = millis();
        int step = fast ? 5 : 1;
        if (timerField == 0) timerWorkMin = constrain(timerWorkMin + dir * step, 1, 99);
        else timerBreakMin = constrain(timerBreakMin + dir * step, 0, 30);
        beep(1000, 10);
        drawTimer();
      }
    }
    if (btnFire.pressed) {
      timerField = 1 - timerField;
      beep(1100, 15);
      drawTimer();
    }
    if (btnSelect.pressed) {
      startTimerPhase(TP_WORK);
      beep(1800, 60);
      lastTimerDraw = millis();
      drawTimer();
    }
    return;
  }

  // ----- time is up (countdown with no break) -----
  if (timerPhase == TP_DONE) {
    if (btnSelect.pressed || btnFire.pressed) {
      timerPhase = TP_SETUP;
      beep(1000, 30);
      drawTimer();
    }
    return;
  }

  // ----- running -----
  if (btnFire.pressed) {              // stop and go back to the setup screen
    timerPhase = TP_SETUP;
    timerPaused = false;
    doubleBeep();
    drawTimer();
    return;
  }
  if (btnSelect.pressed) {            // pause / resume
    if (timerPaused) {
      tmrEndMs = millis() + timerRemainMs;
      timerPaused = false;
      beep(1200, 40);
    } else {
      timerRemainMs = timerRemaining();
      timerPaused = true;
      beep(600, 40);
    }
    drawTimer();
  }

  if (!timerPaused) {
    unsigned long rem = timerRemaining();
    int sec = (int)((rem + 999) / 1000);
    if (sec >= 1 && sec <= 3 && sec != lastTickSec) {   // tick-tick-tick for the last 3 seconds
      lastTickSec = sec;
      beep(1500, 40);
    }
    if (rem == 0) {
      if (timerPhase == TP_WORK) {
        timerRounds++;
        playMelody(MEL_WORK, MEL_WORK_LEN, 4);
        if (timerBreakMin > 0) startTimerPhase(TP_BREAK);
        else timerPhase = TP_DONE;
      } else {
        playMelody(MEL_BREAK, MEL_BREAK_LEN, 3);
        startTimerPhase(TP_WORK);                       // pomodoro keeps cycling
      }
      lastTimerDraw = millis();
      drawTimer();
      return;
    }
  }

  if (millis() - lastTimerDraw > 200) {
    lastTimerDraw = millis();
    drawTimer();
  }
}

// ---------------- Spirit level ----------------
void fmtAngle(char* buf, size_t n, char axis, float a) {
  if (fabsf(a) < 10.0f) snprintf(buf, n, "%c%+.1f", axis, a);
  else snprintf(buf, n, "%c%+.0f", axis, a);
}

void initLevel() {
  levelFlat = false;
  bubbleX = 0;
  bubbleY = 0;
  lastLevelDraw = 0;
}

void updateLevel() {
  if (millis() - lastLevelDraw < 50) return;
  lastLevelDraw = millis();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("LEVEL");

  if (!mpuOK) {
    display.setCursor(0, 24);
    display.print("MPU6050 not found");
    display.setCursor(0, 36);
    display.print("check SDA/SCL wiring");
    display.display();
    return;
  }

  float relX = tiltRaw - levelZeroX;
  float relY = tiltOtherRaw - levelZeroY;
  if (LEVEL_FLIP_X) relX = -relX;
  if (LEVEL_FLIP_Y) relY = -relY;
  float off = sqrtf(relX * relX + relY * relY);

  bool wasFlat = levelFlat;                       // "flat" with a little hysteresis
  if (levelFlat) {
    if (off > LEVEL_LEAVE_DEG) levelFlat = false;
  } else if (off < LEVEL_FLAT_DEG) {
    levelFlat = true;
  }
  if (levelFlat && !wasFlat) beep(1800, 40);

  const int cx = 64, cy = 36, VIAL_R = 24, BUB_R = 4;   // (not R / BR: BR is a macro in the ESP32 headers)
  const float maxOff = VIAL_R - BUB_R - 1;
  float pxPerDeg = maxOff / LEVEL_FULL_DEG;
  float tx = relX * pxPerDeg;
  float ty = relY * pxPerDeg;
  float mag = sqrtf(tx * tx + ty * ty);
  if (mag > maxOff) {
    tx *= maxOff / mag;
    ty *= maxOff / mag;
  }
  bubbleX += (tx - bubbleX) * 0.4f;               // smooth, so the bubble glides
  bubbleY += (ty - bubbleY) * 0.4f;

  // the vial: outer ring, centre target, tick marks
  display.drawCircle(cx, cy, VIAL_R, SSD1306_WHITE);
  display.drawCircle(cx, cy, BUB_R + 2, SSD1306_WHITE);
  display.drawFastHLine(cx - VIAL_R, cy, 5, SSD1306_WHITE);
  display.drawFastHLine(cx + VIAL_R - 4, cy, 5, SSD1306_WHITE);
  display.drawFastVLine(cx, cy - VIAL_R, 5, SSD1306_WHITE);
  display.drawFastVLine(cx, cy + VIAL_R - 4, 5, SSD1306_WHITE);

  int bx = cx + (int)roundf(bubbleX);
  int by = cy + (int)roundf(bubbleY);
  if (levelFlat) display.fillCircle(bx, by, BUB_R, SSD1306_WHITE);
  else display.drawCircle(bx, by, BUB_R, SSD1306_WHITE);

  // degree readouts either side of the vial
  char buf[10];
  fmtAngle(buf, sizeof(buf), 'X', relX);
  display.setCursor(0, 32);
  display.print(buf);
  fmtAngle(buf, sizeof(buf), 'Y', relY);
  display.setCursor(128 - (int)strlen(buf) * 6, 32);
  display.print(buf);

  if (levelFlat) {
    display.setCursor(104, 0);
    display.print("FLAT");
  }
  display.setCursor(0, 56);
  display.print("SEL:zero");
  display.setCursor(80, 56);
  display.print("FIRE:clr");
  display.display();
}

void handleLevel() {
  if (btnJoy.pressed) { goToMenu(); return; }
  if (btnSelect.pressed) {            // call the current pose "level"
    levelZeroX = tiltRaw;
    levelZeroY = tiltOtherRaw;
    beep(1500, 50);
  }
  if (btnFire.pressed) {              // back to the sensor's own zero
    levelZeroX = 0;
    levelZeroY = 0;
    beep(900, 50);
  }
  updateLevel();
}

// ---------------- Reaction tester ----------------
const char* rxRating(unsigned int ms) {
  if (ms < 200) return "LIGHTNING!";
  if (ms < 250) return "FAST";
  if (ms < 300) return "GOOD";
  if (ms < 400) return "OK";
  return "SLEEPY...";
}

void drawReaction() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  char buf[16];

  switch (rxPhase) {
    case RX_READY:
      display.setCursor(0, 0);
      display.print("REACTION");
      display.setCursor(0, 16);
      display.print("Wait for the flash,");
      display.setCursor(0, 26);
      display.print("then hit FIRE fast!");
      display.setCursor(0, 40);
      if (rxBest) {
        display.print("Best: ");
        display.print(rxBest);
        display.print(" ms");
      } else {
        display.print("No best score yet");
      }
      display.setCursor(0, 56);
      display.print("FIRE: start");
      break;

    case RX_WAIT:
      display.setTextSize(2);
      display.setCursor(22, 20);
      display.print("WAIT...");
      display.setTextSize(1);
      display.setCursor(16, 50);
      display.print("don't press yet!");
      break;

    case RX_GO:
      display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
      display.setTextSize(3);
      display.setCursor(37, 20);
      display.print("GO!");
      break;

    case RX_RESULT:
      display.setCursor(0, 0);
      display.print("REACTION");
      if (rxNewBest) {
        display.setCursor(74, 0);
        display.print("NEW BEST!");
      }
      snprintf(buf, sizeof(buf), "%u", rxLast);
      display.setTextSize(3);
      display.setCursor(4, 12);
      display.print(buf);
      display.setTextSize(2);
      display.setCursor(4 + (int)strlen(buf) * 18 + 4, 22);
      display.print("ms");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print(rxRating(rxLast));
      display.setCursor(0, 48);
      display.print("Best: ");
      display.print(rxBest);
      display.print(" ms");
      display.setCursor(0, 56);
      display.print("FIRE: again");
      break;

    case RX_EARLY:
      display.setTextSize(2);
      display.setCursor(4, 14);
      display.print("TOO EARLY!");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print("You jumped the gun");
      display.setCursor(0, 56);
      display.print("FIRE: try again");
      break;
  }
  display.display();
}

void startReactionRound() {
  rxPhase = RX_WAIT;
  rxWaitUntil = millis() + random(1500, 5000);   // 1.5 - 5 s, so you can't guess the timing
  beep(1000, 15);
  drawReaction();
}

void rxGo() {
  rxPhase = RX_GO;
  setRGB(255, 255, 255);          // LED first, it is instant; the screen takes a few ms to draw
  rxGoTime = millis();
  drawReaction();
}

void rxEarly() {
  rxPhase = RX_EARLY;
  setRGB(C_RED);
  drawReaction();
  sfx(180, 220);                  // the rude buzz
  delay(240);
  sfx(110, 450);
  delay(480);
}

void rxPress() {
  // use the moment the button really went down (before the 25 ms debounce wait)
  unsigned long t = btnFire.lastChange;
  if (t <= rxGoTime || (t - rxGoTime) < RX_MIN_MS) {   // faster than humanly possible = a guess
    rxEarly();
    return;
  }
  unsigned long ms = t - rxGoTime;
  if (ms > 9999) ms = 9999;
  rxLast = (unsigned int)ms;
  rxNewBest = (rxBest == 0 || rxLast < rxBest);
  if (rxNewBest) {
    rxBest = rxLast;
    prefs.putUShort("rbest", (uint16_t)rxBest);
  }
  rxPhase = RX_RESULT;
  drawReaction();
  if (rxNewBest) {
    sfx(880, 90); delay(110);
    sfx(1175, 90); delay(110);
    sfx(1568, 180); delay(200);
  } else {
    beep(1500, 60);
  }
}

void handleReaction() {
  if (btnJoy.pressed) { goToMenu(); return; }

  switch (rxPhase) {
    case RX_WAIT:
      if (btnFire.pressed) rxEarly();
      else if (millis() >= rxWaitUntil) rxGo();
      break;
    case RX_GO:
      if (btnFire.pressed) rxPress();
      break;
    default:   // READY, RESULT, EARLY
      if (btnFire.pressed || btnSelect.pressed) startReactionRound();
      break;
  }
}

// ---------------- Music ----------------
// Seven tabs: METRONOME, PIANO (+looper), THEREMIN, DRUMS, SONGS, TUNER, RETRO (8-bit sound effects).
// Joystick click = SHIFT: tap = next tab | hold + stick L/R = previous / next tab | hold + U/D, FIRE, SELECT = options.

const char* tempoWord(int b) {
  if (b < 60) return "LARGO";
  if (b < 76) return "ADAGIO";
  if (b < 108) return "ANDANTE";
  if (b < 120) return "MODERATO";
  if (b < 156) return "ALLEGRO";
  if (b < 176) return "VIVACE";
  return "PRESTO";
}

float midiFreq(int m) {
  return 440.0f * powf(2.0f, (m - 69) / 12.0f);
}

void saveTempo() {
  prefs.putUShort("bpm", (uint16_t)bpm);
}

// stick direction with one winner (no diagonals): 0 none, 1 up, 2 down, 3 left, 4 right
int stickDir() {
  int vx = analogRead(VRX_PIN);
  int vy = analogRead(VRY_PIN);
  int dx = abs(vx - 2048);
  int dy = abs(vy - 2048);
  if (dx < 850 && dy < 850) return 0;
  if (dx >= dy) return (vx < 2048) ? 1 : 2;
  return (vy < 2048) ? 3 : 4;
}

void musHeader() {
  char r[16];
  snprintf(r, sizeof(r), "%s%d/%d", soundOn ? "" : "MUTED ", musTab + 1, MUS_TABS);
  drawHeader(musTabNames[musTab], r);
}

void musShiftHint(const char* s) {     // replaces the bottom line while SHIFT is held
  display.fillRect(0, 54, 128, 10, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print(s);
}

void musicStopAll() {
  stopTone();
  metroRunning = false;
  pianoKey = -1;
  pianoSounding = -1;
  loopState = 0;
  loopKeyPlaying = -1;
  thereminOn = false;
  drumRunning = false;
  drumFx = 0;
  drumPlayStep = -1;
  songPlaying = false;
  songGapPending = false;
  songCurMidi = -1;
  tunerLatch = false;
  tunerSounding = false;
  tunerSoundMidi = -1;
  sfxPlaying = false;
  musLed = C_OFF;
  lastMusDraw = 0;
}

void musicSwitchTab(int t) {
  musicStopAll();
  musTab = t;
  thereminOtherZero = tiltOtherRaw;
  drumDirty = true;
  beep(1300, 30);
}

// ===== 1. METRONOME =====
void musicMetro(unsigned long now) {
  if (!metroRunning || (long)(now - metroNext) < 0) return;
  int subs = SUB_OPTS[metroSubIdx];
  unsigned long iv = 60000UL / (unsigned long)bpm / (unsigned long)subs;
  if (metroSubTick == 0) {                                        // a full beat
    bool accent = (metroBeat == 0) && metroAccentOn;
    sfx(accent ? METRO_HI[metroSound] : METRO_LO[metroSound], METRO_MS[metroSound]);
    metroFlashUntil = now + 80;
    metroFlashAccent = accent;
    metroCur = metroBeat;
    metroSwing ^= 1;
  } else {                                                        // a soft in-between tick
    sfx(METRO_LO[metroSound] + 200, 8);
  }
  metroSubTick++;
  if (metroSubTick >= subs) {
    metroSubTick = 0;
    metroBeat = (metroBeat + 1) % BEAT_OPTS[metroBeatsIdx];
  }
  metroNext += iv;
  if ((long)(now - metroNext) > (long)iv) metroNext = now + iv;   // catch up after a stall
  lastMusDraw = 0;                                                // redraw right after the click
}

void drawMetro(bool shift) {
  display.clearDisplay();
  musHeader();
  char b[16];
  snprintf(b, sizeof(b), "%d", bpm);
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 13);
  display.print(b);
  display.setTextSize(1);
  display.setCursor(2, 39);
  display.print(tempoWord(bpm));
  display.setCursor(56, 39);
  display.print(METRO_NAMES[metroSound]);
  if (!metroAccentOn) display.print(" flat");

  // pendulum: it is at one end exactly on every beat
  const int px0 = 103, py0 = 12;
  const float plen = 28.0f;
  float pos = 0;
  if (metroRunning) {
    unsigned long now = millis();
    int subs = SUB_OPTS[metroSubIdx];
    unsigned long ivMain = 60000UL / (unsigned long)bpm;
    unsigned long ivSub = ivMain / (unsigned long)subs;
    unsigned long rem = (metroNext > now) ? (metroNext - now) : 0;
    rem += (unsigned long)((subs - metroSubTick) % subs) * ivSub;
    if (rem > ivMain) rem = ivMain;
    float phase = 1.0f - (float)rem / (float)ivMain;
    float side = metroSwing ? -1.0f : 1.0f;
    pos = side * (1.0f - 2.0f * phase);
  }
  float ang = pos * 0.6f;
  int bx = px0 + (int)(sinf(ang) * plen);
  int by = py0 + (int)(cosf(ang) * plen);
  display.drawLine(px0, py0, bx, by, SSD1306_WHITE);
  display.fillCircle(bx, by, 3, SSD1306_WHITE);
  display.fillCircle(px0, py0, 1, SSD1306_WHITE);

  int nb = BEAT_OPTS[metroBeatsIdx];
  for (int i = 0; i < nb; i++) {
    int cx = 6 + i * 11, cy = 51;
    int rr = (i == 0) ? 4 : 3;
    if (metroRunning && i == metroCur) display.fillCircle(cx, cy, rr, SSD1306_WHITE);
    else display.drawCircle(cx, cy, rr, SSD1306_WHITE);
  }
  snprintf(b, sizeof(b), "%dB%s", nb, SUB_NAMES[metroSubIdx]);
  display.setCursor(80, 47);
  display.print(b);
  if (shift) musShiftHint("U/D:sound F:sub S:acc");
  else {
    display.setCursor(0, 56);
    display.print(metroRunning ? "FIRE:stop SEL:tap" : "FIRE:go SEL:tap");
  }
  display.display();
}

void musicMetroLoop(unsigned long now, bool shift, int dir) {
  if (shift) {
    bpmHoldStart = 0;
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 280) {     // click sound
      lastMusMove2 = now;
      metroSound = (metroSound + (dir == 1 ? 1 : 3)) % 4;
      sfx(METRO_HI[metroSound], METRO_MS[metroSound]);
      lastMusDraw = 0;
    }
    if (btnFire.pressed) {                                        // quarters / eighths / triplets
      metroSubIdx = (metroSubIdx + 1) % 3;
      metroSubTick = 0;
      beep(1200, 20);
      lastMusDraw = 0;
    }
    if (btnSelect.pressed) {                                      // accent on beat 1 or not
      metroAccentOn = !metroAccentOn;
      beep(metroAccentOn ? 1400 : 700, 30);
      lastMusDraw = 0;
    }
  } else {
    int d = (dir == 1) ? 1 : ((dir == 2) ? -1 : 0);               // stick up / down = BPM, hold to go faster
    if (d == 0) {
      bpmHoldStart = 0;
    } else {
      if (bpmHoldStart == 0) bpmHoldStart = now;
      bool fast = (now - bpmHoldStart > 1200);
      if (now - lastBpmAdj > (fast ? 60UL : 200UL)) {
        lastBpmAdj = now;
        bpm = constrain(bpm + d * (fast ? 5 : 1), 40, 220);
        lastMusDraw = 0;
      }
    }

    if ((dir == 3 || dir == 4) && now - lastMusMove > 300) {      // stick left / right = beats per bar
      lastMusMove = now;
      metroBeatsIdx = (metroBeatsIdx + (dir == 4 ? 1 : 3)) % 4;
      metroBeat = 0;
      metroCur = 0;
      metroSubTick = 0;
      beep(1000, 12);
      lastMusDraw = 0;
    }

    if (btnFire.pressed) {                                        // start / stop
      metroRunning = !metroRunning;
      if (metroRunning) { metroBeat = 0; metroCur = 0; metroSubTick = 0; metroSwing = 0; metroNext = now; }
      lastMusDraw = 0;
    }

    if (btnSelect.pressed) {                                      // tap tempo: tap the rhythm you want
      if (tapTempoN > 0 && now - tapTempoT[tapTempoN - 1] > 2000) tapTempoN = 0;
      if (tapTempoN == 4) {
        for (int i = 0; i < 3; i++) tapTempoT[i] = tapTempoT[i + 1];
        tapTempoN = 3;
      }
      tapTempoT[tapTempoN++] = now;
      if (tapTempoN >= 2) {
        unsigned long span = tapTempoT[tapTempoN - 1] - tapTempoT[0];
        if (span > 0) bpm = constrain((int)(60000UL * (unsigned long)(tapTempoN - 1) / span), 40, 220);
      }
      beep(1200, 15);
      lastMusDraw = 0;
    }
  }

  musicMetro(now);

  if (now < metroFlashUntil) musLed = metroFlashAccent ? scaleColor(C_GREEN, 220) : scaleColor(Color{255, 255, 255}, 70);
  else musLed = C_OFF;

  bool beatSoon = metroRunning && ((long)(metroNext - now) < 35);   // never start a slow screen update just before a click
  if (!beatSoon && now - lastMusDraw > (metroRunning ? 45UL : 150UL)) {
    lastMusDraw = now;
    drawMetro(shift);
  }
}

// ===== 2. PIANO + LOOPER =====
void loopFinishRec(unsigned long now) {
  if (loopN > 0 && loopEv[loopN - 1].key != -1 && loopN < LOOP_MAX) {   // make sure the last note ends
    unsigned long t = now - loopRecStart;
    if (t > 65000UL) t = 65000UL;
    loopEv[loopN].t = (uint16_t)t;
    loopEv[loopN].key = -1;
    loopN++;
  }
  if (loopN == 0) {                                                      // nothing was played
    loopState = 0;
    beep(400, 80);
    return;
  }
  loopLen = now - loopRecStart;
  if (loopLen < 400) loopLen = 400;
  if (loopLen > 65000UL) loopLen = 65000UL;
  loopState = 2;
  loopPlayStart = now;
  loopPos = 0;
  loopKeyPlaying = -1;
  beep(1300, 60);
}

void loopCycle(unsigned long now) {          // idle -> recording -> looping -> idle
  if (loopState == 0) {
    loopN = 0;
    loopState = 1;
    loopRecStart = now;
    beep(1700, 60);
  } else if (loopState == 1) {
    loopFinishRec(now);
  } else {
    loopState = 0;
    loopKeyPlaying = -1;
    beep(700, 60);
  }
}

void drawPiano(bool shift) {
  display.clearDisplay();
  musHeader();
  const char keyLab[6] = {'L', 'U', 'D', 'R', 'F', 'S'};
  for (int i = 0; i < 6; i++) {
    int x = 4 + i * 20;
    int midi = 60 + 12 * pianoOct + SCALE_DEG[pianoScale][i];
    const char* nm = NOTE_NAMES[midi % 12];
    bool sharp = (strlen(nm) > 1);
    bool on = (i == pianoSounding);
    if (on) display.fillRect(x, 12, 19, 26, SSD1306_WHITE);
    else display.drawRect(x, 12, 19, 26, SSD1306_WHITE);
    display.setTextSize(sharp ? 1 : 2);
    display.setTextColor(on ? SSD1306_BLACK : SSD1306_WHITE);
    int tw = (int)strlen(nm) * (sharp ? 6 : 12);
    display.setCursor(x + (19 - tw) / 2, sharp ? 21 : 18);
    display.print(nm);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x + 7, 41);
    display.print(keyLab[i]);
  }
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (shift) {
    musShiftHint("U/D:oct F:scale S:rec");
  } else {
    char b[20];
    snprintf(b, sizeof(b), "%s oct%d", SCALE_NAMES[pianoScale], 4 + pianoOct);
    display.setCursor(0, 54);
    display.print(b);
    if (loopState == 1) {
      if ((millis() / 300) % 2 == 0) { display.fillCircle(104, 57, 3, SSD1306_WHITE); }
      display.setCursor(110, 54);
      display.print("REC");
    } else if (loopState == 2) {
      display.setCursor(98, 54);
      display.print("LOOP");
    }
  }
  if (loopState == 2 && loopLen > 0) {                                  // loop progress, a thin line at the very bottom
    int w = (int)(((millis() - loopPlayStart) % loopLen) * 126UL / loopLen);
    display.drawFastHLine(0, 63, w, SSD1306_WHITE);
  }
  display.display();
}

void musicPianoLoop(unsigned long now, bool shift, int dir) {
  if (shift) {
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 300) {           // octave
      lastMusMove2 = now;
      pianoOct = constrain(pianoOct + (dir == 1 ? 1 : -1), 0, 2);
      beep(900 + 300 * pianoOct, 30);
      lastMusDraw = 0;
    }
    if (btnFire.pressed) {                                              // scale
      pianoScale = (pianoScale + 1) % N_SCALES;
      beep(1200, 25);
      lastMusDraw = 0;
    }
    if (btnSelect.pressed) {                                            // record / loop / stop
      loopCycle(now);
      lastMusDraw = 0;
    }
  }

  int key = -1;                                                         // the 6 keys: stick L U D R, FIRE, SELECT
  if (!shift) {
    if (btnSelect.stable == LOW) key = 5;
    else if (btnFire.stable == LOW) key = 4;
    else if (dir == 4) key = 3;
    else if (dir == 2) key = 2;
    else if (dir == 1) key = 1;
    else if (dir == 3) key = 0;
  }

  if (loopState == 1) {                                                 // recording: remember every key change
    if (key != pianoKey) {
      unsigned long t = now - loopRecStart;
      if (t > 65000UL) t = 65000UL;
      loopEv[loopN].t = (uint16_t)t;
      loopEv[loopN].key = (int8_t)key;
      loopN++;
    }
    if (loopN >= LOOP_MAX - 2 || now - loopRecStart > 60000UL) loopFinishRec(now);
  }
  pianoKey = key;

  if (loopState == 2) {                                                 // the loop keeps time even while you play over it
    unsigned long t = now - loopPlayStart;
    while (t >= loopLen) { loopPlayStart += loopLen; t -= loopLen; loopPos = 0; loopKeyPlaying = -1; }
    while (loopPos < loopN && loopEv[loopPos].t <= t) { loopKeyPlaying = loopEv[loopPos].key; loopPos++; }
  }

  int soundKey = (pianoKey >= 0) ? pianoKey : ((loopState == 2) ? loopKeyPlaying : -1);
  if (soundKey != pianoSounding) {
    pianoSounding = soundKey;
    if (soundKey >= 0) holdTone((int)(midiFreq(60 + 12 * pianoOct + SCALE_DEG[pianoScale][soundKey]) + 0.5f));
    else stopTone();
    lastMusDraw = 0;
  }
  if (soundKey >= 0) musLed = scaleColor(hueColor(soundKey * 55), 200);
  else musLed = C_OFF;

  if (now - lastMusDraw > (loopState != 0 ? 100UL : 250UL)) {
    lastMusDraw = now;
    drawPiano(shift);
  }
}

// ===== 3. THEREMIN =====
float thereminBaseHz() {
  return 110.0f * (float)(1 << thereminBase);
}

void drawTheremin(bool shift) {
  display.clearDisplay();
  musHeader();
  if (!mpuOK) {
    display.setCursor(0, 24);
    display.print("MPU6050 not found");
    display.display();
    return;
  }
  float base = thereminBaseHz();
  float t = log2f(thereminFreq / base) / 3.0f;              // 0 ... 1 across three octaves
  t = constrain(t, 0.0f, 1.0f);
  display.drawRect(2, 13, 124, 10, SSD1306_WHITE);
  int mx = 4 + (int)(t * 118.0f);
  display.fillRect(mx - 1, 14, 4, 8, SSD1306_WHITE);
  if (!thereminOn) display.fillRect(mx, 16, 2, 4, SSD1306_BLACK);   // hollow marker = silent

  char b[16];
  snprintf(b, sizeof(b), "%dHz", (int)thereminFreq);
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(2, 27);
  display.print(b);
  int n = ((thereminSemi % 12) + 12) % 12;                  // pitch class counted from A
  static const char* const thNote[12] = {"A", "A#", "B", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#"};
  display.setCursor(92, 27);
  display.print(thNote[n]);

  display.setTextSize(1);
  display.setCursor(0, 46);
  display.print("VIB");
  display.drawRect(20, 46, 38, 7, SSD1306_WHITE);
  display.fillRect(22, 48, (int)(thereminVib * 34.0f), 3, SSD1306_WHITE);
  snprintf(b, sizeof(b), "A%d-A%d", 2 + thereminBase, 5 + thereminBase);
  display.setCursor(64, 46);
  display.print(b);
  if (thereminSnap) {
    display.setCursor(104, 46);
    display.print("SNAP");
  }
  if (shift) musShiftHint("U/D:range  L/R:tab");
  else {
    display.setCursor(0, 56);
    display.print("tilt+FIRE SEL:snap");
  }
  display.display();
}

void musicThereminLoop(unsigned long now, bool shift, int dir) {
  if (shift && (dir == 1 || dir == 2) && now - lastMusMove2 > 300) {   // which three octaves
    lastMusMove2 = now;
    thereminBase = constrain(thereminBase + (dir == 1 ? 1 : -1), 0, 2);
    beep(900 + 200 * thereminBase, 30);
    lastMusDraw = 0;
  }
  if (!shift && btnSelect.pressed) {
    thereminSnap = !thereminSnap;
    beep(1300, 25);
    lastMusDraw = 0;
  }

  float base = thereminBaseHz();
  if (mpuOK) {
    float t = constrain((tiltRaw + 40.0f) / 80.0f, 0.0f, 1.0f);       // -40 ... +40 degrees = 3 octaves
    float f = base * powf(2.0f, 3.0f * t);
    thereminSemi = (int)roundf(12.0f * log2f(f / base));
    if (thereminSnap) f = base * powf(2.0f, thereminSemi / 12.0f);
    thereminFreq = f;
    // tilt the board forward / back (from the pose you entered in) for vibrato
    thereminVib = constrain((fabsf(tiltOtherRaw - thereminOtherZero) - 8.0f) / 22.0f, 0.0f, 1.0f);
  }

  bool gate = (btnFire.stable == LOW) && !shift && mpuOK;
  if (gate) {
    float f = thereminFreq * (1.0f + 0.035f * thereminVib * sinf(now * 0.0377f));   // about 6 wobbles a second
    if (!thereminOn || (now - thereminLastTune >= 8 && fabsf(f - thereminLastOut) > 2.0f)) {
      holdTone((int)f);
      thereminLastOut = f;
      thereminLastTune = now;
      thereminOn = true;
    }
  } else if (thereminOn) {
    stopTone();
    thereminOn = false;
  }

  if (thereminOn) musLed = scaleColor(hueColor((int)fmaxf(0.0f, log2f(thereminFreq / base) * 110.0f)), 200);
  else musLed = C_OFF;

  if (now - lastMusDraw > 60) {
    lastMusDraw = now;
    drawTheremin(shift);
  }
}

// ===== 4. DRUM MACHINE =====
void loadDrumPreset() {
  for (int r = 0; r < 3; r++) drumPat[r] = (drumPreset < 3) ? DRUM_PRESETS[drumPreset][r] : 0;
  drumDirty = true;
}

void drumTrigger(int fx, unsigned long now) {   // 1 kick, 2 snare, 3 hat - made of tones, no blocking
  drumFx = fx;
  drumFxStart = now;
  drumFxLast = now;
  drumFlashRow = fx - 1;
  drumFlashUntil = now + 70;
  if (fx == 1) { drumFxLastF = 320; holdTone(320); }
  else if (fx == 2) { holdTone((int)random(800, 3500)); }
  else { holdTone(6500); }
}

void drumFxUpdate(unsigned long now) {
  if (drumFx == 0) return;
  unsigned long t = now - drumFxStart;
  if (drumFx == 1) {                                        // kick: a quick downward sweep
    if (t >= 120) { stopTone(); drumFx = 0; return; }
    int f = 320 - (int)(t * 210UL / 120UL);
    if (abs(f - drumFxLastF) >= 8) { holdTone(f); drumFxLastF = f; }
  } else if (drumFx == 2) {                                 // snare: a burst of random pitches = noise
    if (t >= 85) { stopTone(); drumFx = 0; return; }
    if (now - drumFxLast >= 5) { holdTone((int)random(800, 3500)); drumFxLast = now; }
  } else {                                                  // hat: a very short high tick
    if (t >= 16) { stopTone(); drumFx = 0; return; }
  }
}

void drawDrums(bool shift) {
  display.clearDisplay();
  musHeader();
  const char lab[3] = {'K', 'S', 'H'};
  for (int r = 0; r < 3; r++) {
    int y = 12 + r * 11;
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, y);
    display.print(lab[r]);
    for (int c = 0; c < 16; c++) {
      int x = 8 + c * 7 + (c / 4);                           // a tiny gap between beats
      bool on = ((drumPat[r] >> c) & 1) != 0;
      if (on) display.fillRect(x, y, 6, 8, SSD1306_WHITE);
      else display.drawRect(x, y, 6, 8, SSD1306_WHITE);
      if (r == drumCurRow && c == drumCurCol) {              // cursor: a notch in a lit cell, a dot in an empty one
        display.fillRect(x + 2, y + 2, 2, 4, on ? SSD1306_BLACK : SSD1306_WHITE);
      }
    }
  }
  if (drumRunning && drumPlayStep >= 0) {                    // playhead
    int x = 8 + drumPlayStep * 7 + (drumPlayStep / 4);
    display.fillRect(x, 44, 6, 2, SSD1306_WHITE);
  }
  char b[24];
  snprintf(b, sizeof(b), "%s %dBPM", DRUM_NAMES[drumPreset], bpm);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 48);
  display.print(b);
  display.setCursor(128 - 4 * 6, 48);
  display.print(drumRunning ? "PLAY" : "STOP");
  if (shift) musShiftHint("U/D:bpm F:beat S:clr");
  else {
    display.setCursor(0, 56);
    display.print("FIRE:set SEL:play");
  }
  display.display();
}

void musicDrumsLoop(unsigned long now, bool shift, int dir) {
  if (shift) {
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 110) {          // tempo
      lastMusMove2 = now;
      bpm = constrain(bpm + (dir == 1 ? 2 : -2), 40, 220);
      drumDirty = true;
    }
    if (btnFire.pressed) {                                             // next preset beat
      drumPreset = (drumPreset + 1) % 4;
      loadDrumPreset();
      beep(1200, 25);
    }
    if (btnSelect.pressed) {                                           // wipe the pattern
      for (int r = 0; r < 3; r++) drumPat[r] = 0;
      drumPreset = 3;
      beep(600, 40);
      drumDirty = true;
    }
  } else {
    if ((dir == 3 || dir == 4) && now - lastMusMove > 130) {           // cursor left / right
      lastMusMove = now;
      drumCurCol = (drumCurCol + (dir == 4 ? 1 : 15)) % 16;
      drumDirty = true;
    }
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 200) {          // cursor up / down
      lastMusMove2 = now;
      drumCurRow = (drumCurRow + (dir == 2 ? 1 : 2)) % 3;
      drumDirty = true;
    }
    if (btnFire.pressed) {                                             // switch the cell under the cursor
      drumPat[drumCurRow] ^= (uint16_t)(1u << drumCurCol);
      if ((drumPat[drumCurRow] >> drumCurCol) & 1) drumTrigger(drumCurRow + 1, now);   // let you hear it
      drumDirty = true;
    }
    if (btnSelect.pressed) {                                           // play / stop
      drumRunning = !drumRunning;
      if (drumRunning) { drumStep = 0; drumNext = now; }
      else { drumPlayStep = -1; stopTone(); drumFx = 0; }
      drumDirty = true;
    }
  }

  if (drumRunning && (long)(now - drumNext) >= 0) {                    // next 16th note
    unsigned long iv = 60000UL / (unsigned long)bpm / 4UL;
    if ((drumPat[0] >> drumStep) & 1) drumTrigger(1, now);             // one buzzer: kick beats snare beats hat
    else if ((drumPat[1] >> drumStep) & 1) drumTrigger(2, now);
    else if ((drumPat[2] >> drumStep) & 1) drumTrigger(3, now);
    drumPlayStep = drumStep;
    drumStep = (drumStep + 1) % 16;
    drumNext += iv;
    if ((long)(now - drumNext) > (long)iv) drumNext = now + iv;
    drumDirty = true;
  }
  drumFxUpdate(now);

  if (now < drumFlashUntil) {
    if (drumFlashRow == 0) musLed = scaleColor(C_RED, 230);
    else if (drumFlashRow == 1) musLed = scaleColor(Color{255, 180, 0}, 230);
    else musLed = scaleColor(Color{255, 255, 255}, 60);
  } else {
    musLed = C_OFF;
  }

  bool stepSoon = drumRunning && ((long)(drumNext - now) < 30);        // keep the screen refresh away from the beat
  bool fxBusy = (drumFx != 0) && (now - lastMusDraw < 200);
  if (!stepSoon && !fxBusy && (drumDirty || now - lastMusDraw > 250)) {
    drumDirty = false;
    lastMusDraw = now;
    drawDrums(shift);
  }
}

// ===== 5. SONGS =====
void songStop() {
  songPlaying = false;
  songGapPending = false;
  songCurMidi = -1;
  stopTone();
}

void songStart(unsigned long now) {
  if (songPos >= SONGS[songIdx].n) songPos = 0;
  songPlaying = true;
  songGapPending = false;
  songNext = now;
}

void songTick(unsigned long now) {
  if (!songPlaying) return;
  if (songGapPending && (long)(now - songGapAt) >= 0) {               // short gap so repeated notes are heard separately
    stopTone();
    songGapPending = false;
  }
  if ((long)(now - songNext) < 0) return;
  const SongInfo &s = SONGS[songIdx];
  if (songPos >= s.n) {                                                // finished
    if (songMode == 0) {
      songStop();
      songPos = 0;
      beep(1500, 60);
      lastMusDraw = 0;
      return;
    }
    songPos = 0;                                                       // LOOP: again | RAMP: again, 10% faster
    if (songMode == 2) songBpm = constrain(songBpm + songBpm / 10 + 1, 40, 360);
    lastMusDraw = 0;
  }
  unsigned long unit = 60000UL / (unsigned long)songBpm / 4UL;        // one sixteenth
  const SongNote &n = s.notes[songPos];
  unsigned long dur = unit * (unsigned long)n.u;
  if (n.m == 0) {
    stopTone();
    songCurMidi = -1;
    songGapPending = false;
  } else {
    holdTone((int)(midiFreq(n.m) + 0.5f));
    songCurMidi = n.m;
    songGapAt = now + dur * 85UL / 100UL;
    songGapPending = true;
  }
  songNext += dur;
  if ((long)(now - songNext) > (long)dur) songNext = now + dur;
  songPos++;
  if (dur >= 150) lastMusDraw = 0;                                    // very short notes: no redraw per note
}

void drawSongs(bool shift) {
  display.clearDisplay();
  musHeader();
  const SongInfo &s = SONGS[songIdx];
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  int nw = (int)strlen(s.name) * 12;
  display.setCursor((128 - nw) / 2, 13);
  display.print(s.name);
  display.setCursor(0, 13);
  display.print("<");
  display.setCursor(116, 13);
  display.print(">");

  display.drawRect(2, 32, 124, 6, SSD1306_WHITE);                       // progress through the song
  int fill = (int)((long)songPos * 120L / (long)s.n);
  display.fillRect(4, 34, fill, 2, SSD1306_WHITE);

  char b[16];
  display.setTextSize(2);
  display.setCursor(4, 41);
  if (songPlaying && songCurMidi >= 0) {
    snprintf(b, sizeof(b), "%s%d", NOTE_NAMES[songCurMidi % 12], songCurMidi / 12 - 1);
    display.print(b);
  } else if (songPlaying) {
    display.print("...");
  } else if (songPos > 0) {
    display.setTextSize(1);
    display.setCursor(4, 45);
    display.print("PAUSED");
  } else {
    display.setTextSize(1);
    display.setCursor(4, 45);
    display.print("READY");
  }
  display.setTextSize(1);
  snprintf(b, sizeof(b), "%dBPM %d/%d", songBpm, songIdx + 1, SONG_COUNT);
  display.setCursor(128 - (int)strlen(b) * 6, 45);
  display.print(b);

  if (songMode > 0) {
    display.setCursor(42, 45);
    display.print(songMode == 1 ? "LOOP" : "RAMP");
  }
  if (shift) musShiftHint("U/D:bpm F:loop/ramp");
  else {
    display.setCursor(0, 56);
    display.print("FIRE:play SEL:restart");
  }
  display.display();
}

void musicSongsLoop(unsigned long now, bool shift, int dir) {
  if (shift) {
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 150) {           // tempo
      lastMusMove2 = now;
      songBpm = constrain(songBpm + (dir == 1 ? 1 : -1) * (songBpm >= 160 ? 10 : 4), 40, 360);
      lastMusDraw = 0;
    }
    if (btnFire.pressed) {                                              // play once -> loop -> ramp (faster each time round)
      songMode = (songMode + 1) % 3;
      beep(800 + 300 * songMode, 30);
      lastMusDraw = 0;
    }
  } else {
    if ((dir == 3 || dir == 4) && now - lastMusMove > 300) {            // pick a song
      lastMusMove = now;
      songStop();
      songIdx = (songIdx + (dir == 4 ? 1 : SONG_COUNT - 1)) % SONG_COUNT;
      songPos = 0;
      songBpm = SONGS[songIdx].bpm;
      beep(1000, 15);
      lastMusDraw = 0;
    }
    if (btnFire.pressed) {                                              // play / pause
      if (songPlaying) songStop(); else songStart(now);
      lastMusDraw = 0;
    }
    if (btnSelect.pressed) {                                            // from the top
      songStop();
      songPos = 0;
      songStart(now);
      lastMusDraw = 0;
    }
  }

  songTick(now);

  if (songPlaying && songGapPending && songCurMidi >= 0) musLed = scaleColor(hueColor((songCurMidi % 12) * 30), 200);
  else musLed = C_OFF;

  bool songSoon = songPlaying && ((long)(songNext - now) < 32);        // keep the slow screen update away from the notes
  if (!songSoon && now - lastMusDraw > 150) {
    lastMusDraw = now;
    drawSongs(shift);
  }
}

// ===== 6. TUNER / PITCH PIPE =====
void drawTuner(bool shift) {
  display.clearDisplay();
  musHeader();
  char b[16];
  snprintf(b, sizeof(b), "%s%d", NOTE_NAMES[tunerMidi % 12], tunerMidi / 12 - 1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(3);
  display.setCursor(2, 14);
  display.print(b);
  int hz = (int)(midiFreq(tunerMidi) + 0.5f);
  snprintf(b, sizeof(b), "%d", hz);
  display.setTextSize(2);
  display.setCursor(64, 14);
  display.print(b);
  display.setTextSize(1);
  display.setCursor(64 + (int)strlen(b) * 12 + 2, 22);
  display.print("Hz");

  display.setCursor(0, 40);
  if (TUNER_MIDI[tunerPreset] == tunerMidi) display.print(TUNER_NAMES[tunerPreset]);
  else display.print("CUSTOM NOTE");
  if (tunerSounding) display.fillCircle(121, 43, 4, SSD1306_WHITE);      // "sounding" dot, clear of the long names
  display.setCursor(0, 48);
  display.print("L/R:note  U/D:octave");
  if (shift) musShiftHint("F:next preset L/R:tab");
  else {
    display.setCursor(0, 56);
    display.print(tunerLatch ? "FIRE:hold SEL:LATCH*" : "FIRE:hold SEL:latch");
  }
  display.display();
}

void musicTunerLoop(unsigned long now, bool shift, int dir) {
  if (shift) {
    if (btnFire.pressed) {                                               // jump to the next instrument string
      tunerPreset = (tunerPreset + 1) % TUNER_N;
      tunerMidi = TUNER_MIDI[tunerPreset];
      beep(1100, 20);
      lastMusDraw = 0;
    }
  } else {
    if ((dir == 3 || dir == 4) && now - lastMusMove > 200) {            // one semitone
      lastMusMove = now;
      tunerMidi = constrain(tunerMidi + (dir == 4 ? 1 : -1), 45, 100);
      lastMusDraw = 0;
    }
    if ((dir == 1 || dir == 2) && now - lastMusMove2 > 300) {           // one octave
      lastMusMove2 = now;
      tunerMidi = constrain(tunerMidi + (dir == 1 ? 12 : -12), 45, 100);
      lastMusDraw = 0;
    }
    if (btnSelect.pressed) {
      tunerLatch = !tunerLatch;
      beep(tunerLatch ? 1300 : 800, 25);
      lastMusDraw = 0;
    }
  }

  bool gate = tunerLatch || (btnFire.stable == LOW && !shift);
  if (gate) {
    if (!tunerSounding || tunerSoundMidi != tunerMidi) {
      holdTone((int)(midiFreq(tunerMidi) + 0.5f));
      tunerSounding = true;
      tunerSoundMidi = tunerMidi;
      lastMusDraw = 0;
    }
  } else if (tunerSounding) {
    stopTone();
    tunerSounding = false;
    tunerSoundMidi = -1;
    lastMusDraw = 0;
  }

  if (tunerSounding) musLed = scaleColor(hueColor((tunerMidi % 12) * 30), 200);
  else musLed = C_OFF;

  if (now - lastMusDraw > 150) {
    lastMusDraw = now;
    drawTuner(shift);
  }
}

// ===== 7. RETRO SOUND BANK =====
// little arcade-style effects built from tone steps; they play without blocking, like the songs do
void sfxAdd(int f, int ms) {
  if (sfxN < SFX_MAX) {
    sfxBuf[sfxN].f = (uint16_t)f;
    sfxBuf[sfxN].ms = (uint8_t)ms;
    sfxN++;
  }
}

void sfxSweep(int f0, int f1, int steps, int ms) {   // glide from f0 to f1 in 'steps' little steps
  for (int i = 0; i < steps; i++) sfxAdd(f0 + (f1 - f0) * i / (steps - 1), ms);
}

void sfxBuild(int id) {
  sfxN = 0;
  switch (id) {
    case 0:                                                   // COIN: two quick rising blips
      sfxAdd(1568, 60); sfxAdd(2093, 220);
      break;
    case 1:                                                   // JUMP: a springy rising slide
      sfxSweep(280, 1000, 12, 14);
      break;
    case 2: {                                                 // POWER UP: arpeggio climbing up
      const int n[6] = {523, 659, 784, 1047, 1319, 1568};
      for (int i = 0; i < 6; i++) sfxAdd(n[i], 55);
      sfxAdd(2093, 180);
      break;
    }
    case 3: {                                                 // BONUS: stacked fifths, then a long top note
      const int n[4] = {440, 660, 880, 1320};
      for (int i = 0; i < 4; i++) sfxAdd(n[i], 80);
      sfxAdd(1760, 260);
      break;
    }
    case 4:                                                   // LASER: fast falling zap
      sfxSweep(2800, 250, 16, 8);
      break;
    case 5:                                                   // BOOM: noise that dies away
      for (int i = 0; i < 28; i++) sfxAdd((int)random(70, 150 + 600 * (28 - i) / 28), 15 + i / 2);
      break;
    case 6: {                                                 // FANFARE: four rising notes and a held top
      const int n[4] = {392, 523, 659, 784};
      for (int i = 0; i < 4; i++) { sfxAdd(n[i], 100); sfxAdd(0, 20); }
      sfxAdd(1047, 450);
      break;
    }
    default:                                                  // YOU LOSE: three drops and a long slide down
      sfxAdd(494, 160); sfxAdd(0, 30);
      sfxAdd(466, 160); sfxAdd(0, 30);
      sfxAdd(440, 160); sfxAdd(0, 30);
      sfxSweep(415, 110, 14, 40);
      break;
  }
}

void sfxStart(int id) {                                       // builds the effect; sfxUpdate() makes the sound
  sfxBuild(id);
  sfxPos = 0;
  sfxHue = id * 45;
  sfxPlaying = true;
  sfxNext = millis();
}

void sfxUpdate(unsigned long now) {
  if (!sfxPlaying || (long)(now - sfxNext) < 0) return;
  if (sfxPos >= sfxN) {                                       // that was the last step
    stopTone();
    sfxPlaying = false;
    lastMusDraw = 0;
    return;
  }
  if (sfxBuf[sfxPos].f == 0) stopTone(); else holdTone(sfxBuf[sfxPos].f);
  sfxNext += sfxBuf[sfxPos].ms;
  if ((long)(now - sfxNext) > 50) sfxNext = now + sfxBuf[sfxPos].ms;
  sfxPos++;
}

void drawRetro(bool shift) {
  display.clearDisplay();
  musHeader();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  const char* nm = SFX_NAMES[sfxSel];
  int nw = (int)strlen(nm) * 12;
  display.setCursor((128 - nw) / 2, 15);
  display.print(nm);
  display.setCursor(0, 15);
  display.print("<");
  display.setCursor(116, 15);
  display.print(">");

  display.setTextSize(1);
  if (sfxPlaying) {
    display.setCursor(0, 36);
    display.print("PLAY");
  }
  for (int i = 0; i < SFX_COUNT; i++) {                       // one dot per effect
    int x = 12 + i * 14;
    if (i == sfxSel) display.fillCircle(x, 47, 3, SSD1306_WHITE);
    else display.drawCircle(x, 47, 3, SSD1306_WHITE);
  }
  if (shift) musShiftHint("L/R:tab");
  else {
    display.setCursor(0, 56);
    display.print("L/R pick F:play S:rnd");
  }
  display.display();
}

void musicRetroLoop(unsigned long now, bool shift, int dir) {
  if (!shift) {
    if ((dir == 3 || dir == 4) && now - lastMusMove > 220) {            // pick an effect
      lastMusMove = now;
      sfxPlaying = false;
      stopTone();
      sfxSel = (sfxSel + (dir == 4 ? 1 : SFX_COUNT - 1)) % SFX_COUNT;
      lastMusDraw = 0;
    }
    if (btnFire.pressed || btnSelect.pressed) {                         // FIRE = play it, SELECT = a surprise one
      if (btnSelect.pressed) sfxSel = (int)random(0, SFX_COUNT);
      stopTone();
      sfxStart(sfxSel);
      drawRetro(false);                                                 // draw first (it is slow), then start the clock
      lastMusDraw = millis();
      sfxNext = millis();
    }
  }

  sfxUpdate(now);

  if (sfxPlaying) musLed = scaleColor(hueColor(sfxHue + sfxPos * 25), 200);
  else musLed = C_OFF;

  if (!sfxPlaying && now - lastMusDraw > 150) {                         // no redraws while it plays: they would stretch the sound
    lastMusDraw = now;
    drawRetro(shift);
  }
}

// ===== entering / running the MUSIC mode =====
void initMusic() {
  musicStopAll();
  musShiftUsed = false;
  thereminOtherZero = tiltOtherRaw;
  songBpm = SONGS[songIdx].bpm;
  drumDirty = true;
  lastMusDraw = 0;
}

void handleMusic(bool joyReleased) {
  unsigned long now = millis();
  bool shift = (btnJoy.stable == LOW);                                  // joystick click held = SHIFT
  int dir = stickDir();
  if (btnJoy.pressed) musShiftUsed = false;
  if (shift && (dir == 1 || dir == 2 || btnFire.pressed || btnSelect.pressed)) musShiftUsed = true;

  if (shift && (dir == 3 || dir == 4) && now - lastMusMove > 350) {     // SHIFT + left / right = change tab
    lastMusMove = now;
    musShiftUsed = true;
    musicSwitchTab((musTab + (dir == 4 ? 1 : MUS_TABS - 1)) % MUS_TABS);
    return;
  }
  if (btnJoy.released) {                                                // a quick click on its own = next tab
    if (joyReleased && !musShiftUsed) {
      lastMusMove = now;
      musicSwitchTab((musTab + 1) % MUS_TABS);
      return;
    }
    musShiftUsed = false;
  }

  switch (musTab) {
    case 0: musicMetroLoop(now, shift, dir); break;
    case 1: musicPianoLoop(now, shift, dir); break;
    case 2: musicThereminLoop(now, shift, dir); break;
    case 3: musicDrumsLoop(now, shift, dir); break;
    case 4: musicSongsLoop(now, shift, dir); break;
    case 5: musicTunerLoop(now, shift, dir); break;
    default: musicRetroLoop(now, shift, dir); break;
  }
}

// ---------------- Toys: 8-ball, dice, coin ----------------
// prints text wrapped on spaces, maxChars per line
void drawWrapped(const char* s, int x, int y, int maxChars) {
  char line[24];
  int len = (int)strlen(s);
  int pos = 0;
  int row = 0;
  while (pos < len && row < 5) {
    int take = len - pos;
    if (take > maxChars) {
      int brk = maxChars;
      while (brk > 0 && s[pos + brk] != ' ') brk--;
      take = (brk > 0) ? brk : maxChars;
    }
    if (take > 22) take = 22;
    memcpy(line, s + pos, take);
    line[take] = 0;
    display.setCursor(x, y + row * 9);
    display.print(line);
    pos += take;
    while (pos < len && s[pos] == ' ') pos++;
    row++;
  }
}

void drawBall() {
  int cx = 30, cy = 38;
  if (toyBusy) { cx += (int)random(-3, 4); cy += (int)random(-3, 4); }   // it shakes
  display.drawCircle(cx, cy, 24, SSD1306_WHITE);
  display.drawCircle(cx, cy, 23, SSD1306_WHITE);
  display.fillCircle(cx, cy, 11, SSD1306_WHITE);
  display.setTextSize(2);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(cx - 5, cy - 7);
  display.print("8");
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (toyBusy) drawWrapped("shaking...", 64, 30, 10);
  else if (ballAnswer < 0) drawWrapped("Think of a question, then SHAKE me", 64, 20, 10);
  else drawWrapped(BALL_TEXT[ballAnswer], 64, 24, 10);
}

void drawDie(int x, int y, int s, int v) {
  display.drawRoundRect(x, y, s, s, 6, SSD1306_WHITE);
  static const uint16_t masks[7] = {0, 0x010, 0x101, 0x111, 0x145, 0x155, 0x16D};   // 3x3 grid of pips
  int q = s / 4;
  int pr = s / 12;
  if (pr < 2) pr = 2;
  if (v < 1) v = 1;
  if (v > 6) v = 6;
  for (int i = 0; i < 9; i++) {
    if (masks[v] & (1 << i)) display.fillCircle(x + q + (i % 3) * q, y + q + (i / 3) * q, pr, SSD1306_WHITE);
  }
}

void drawDice() {
  int n = diceCount;
  int s = (n == 1) ? 42 : (n == 2 ? 38 : 34);
  int x0 = (128 - (n * s + (n - 1) * 6)) / 2;
  int total = 0;
  for (int i = 0; i < n; i++) {
    int v = toyBusy ? diceTmp[i] : diceVal[i];
    total += v;
    int jy = toyBusy ? (int)random(-1, 2) : 0;
    drawDie(x0 + i * (s + 6), 13 + jy, s, v);
  }
  if (n > 1 && !toyBusy) {
    char b[16];
    snprintf(b, sizeof(b), "TOTAL: %d", total);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 56);
    display.print(b);
  }
}

void drawCoin() {
  const int cx = 64, cy = 38, rad = 20;
  float angle = 0;
  int face = (coinResult == 1) ? 1 : 0;
  int yoff = 0;
  if (toyBusy) {
    float p = (float)(millis() - toyStart) / (float)TOY_ANIM_MS;
    if (p > 1.0f) p = 1.0f;
    float e = 1.0f - (1.0f - p) * (1.0f - p);                 // spins fast, then settles
    angle = e * (float)coinFlips * PI;
    face = ((int)floorf(angle / PI + 0.001f)) % 2;           // which side is showing
    yoff = -(int)(sinf(p * PI) * 8.0f);                       // toss
  }
  float scale = fabsf(cosf(angle));                           // coin turning edge-on
  for (int dy = -rad; dy <= rad; dy++) {
    int hw = (int)(sqrtf((float)(rad * rad - dy * dy)) * scale + 0.5f);
    if (hw < 1) hw = 1;
    display.drawFastHLine(cx - hw, cy + yoff + dy, 2 * hw + 1, SSD1306_WHITE);
  }
  if (scale > 0.5f) {
    display.setTextSize(3);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(cx - 9, cy + yoff - 11);
    display.print(coinResult < 0 && !toyBusy ? "?" : (face == 0 ? "H" : "T"));
  }
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (!toyBusy && coinResult >= 0) {
    display.setCursor(92, 34);
    display.print(coinResult == 0 ? "HEADS" : "TAILS");
  }
  char b[16];
  snprintf(b, sizeof(b), "H:%d T:%d", coinHeads, coinTails);
  display.setCursor(0, 56);
  display.print(b);
}

void drawToys() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  drawHeader(toyTabNames[toyTab], toyTab == 1 ? "U/D:n SEL" : "SEL/shake");
  if (toyTab == 0) drawBall();
  else if (toyTab == 1) drawDice();
  else drawCoin();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(98, 56);
  display.print("<L/R>");
  display.display();
}

void toyStartAnim() {
  toyBusy = true;
  toyStart = millis();
  lastToyTick = 0;
  toyDirty = true;
  if (toyTab == 0) {
    ballPending = (int)random(0, BALL_COUNT);
  } else if (toyTab == 1) {
    for (int i = 0; i < 3; i++) { diceVal[i] = (int)random(1, 7); diceTmp[i] = diceVal[i]; }
  } else {
    coinResult = (int)random(0, 2);
    coinFlips = 6 + 2 * (int)random(0, 3) + coinResult;       // even number of half-turns = heads
  }
}

void toyFinish() {
  toyBusy = false;
  toyDirty = true;
  if (toyTab == 0) {
    ballAnswer = ballPending;
    int k = BALL_KIND[ballAnswer];
    Color c = C_GREEN;
    if (k == 1) c = {255, 180, 0};
    else if (k == 2) c = C_RED;
    toyFlashColor = c;
    toyFlashUntil = millis() + 900;
    if (k == 0) { sfx(880, 70); delay(90); sfx(1100, 70); delay(90); sfx(1320, 120); delay(130); }
    else if (k == 2) { sfx(500, 90); delay(110); sfx(330, 160); delay(180); }
    else { sfx(700, 100); delay(120); }
  } else if (toyTab == 1) {
    toyFlashColor = {255, 255, 255};
    toyFlashUntil = millis() + 300;
    sfx(900, 40); delay(60); sfx(1300, 70); delay(80);
  } else {
    if (coinResult == 0) { coinHeads++; toyFlashColor = C_GREEN; sfx(1800, 120); }
    else { coinTails++; toyFlashColor = {0, 80, 255}; sfx(1300, 120); }
    toyFlashUntil = millis() + 500;
    delay(130);
  }
}

void toyAnimStep(unsigned long now) {
  float p = (float)(now - toyStart) / (float)TOY_ANIM_MS;
  if (p >= 1.0f) { toyFinish(); return; }
  unsigned long every = (toyTab == 1) ? (unsigned long)(60.0f + p * 180.0f) : 110UL;   // dice slow down
  if (now - lastToyTick >= every) {
    lastToyTick = now;
    static bool flip = false;
    flip = !flip;
    if (toyTab == 0) {
      sfx(flip ? 160 : 210, 50);
    } else if (toyTab == 1) {
      for (int i = 0; i < 3; i++) diceTmp[i] = (int)random(1, 7);
      sfx(400 + (int)random(0, 300), 15);
    } else {
      sfx(flip ? 1500 : 1200, 12);
    }
  }
}

void initToys() {
  toyBusy = false;
  toyDirty = true;
  lastToyMove = millis();
}

void handleToys() {
  if (btnJoy.pressed) { goToMenu(); return; }
  unsigned long now = millis();
  if (!toyBusy) {
    int vx = analogRead(VRX_PIN);
    int vy = analogRead(VRY_PIN);
    if (now - lastToyMove > 300) {
      if (vy < 1200) { toyTab = (toyTab + 2) % 3; lastToyMove = now; beep(1000, 12); toyDirty = true; }
      else if (vy > 2900) { toyTab = (toyTab + 1) % 3; lastToyMove = now; beep(1000, 12); toyDirty = true; }
      else if (toyTab == 1 && vx < 1200 && diceCount < 3) { diceCount++; lastToyMove = now; beep(1100, 12); toyDirty = true; }
      else if (toyTab == 1 && vx > 2900 && diceCount > 1) { diceCount--; lastToyMove = now; beep(900, 12); toyDirty = true; }
    }
    if (btnSelect.pressed || btnFire.pressed || shakeTriggered) toyStartAnim();
  } else {
    toyAnimStep(now);
  }
  if (toyBusy ? (now - lastToyDraw > 40) : toyDirty) {
    lastToyDraw = now;
    toyDirty = false;
    drawToys();
  }
}

// ---------------- Navigation ----------------
void goToMenu() {
  beep(600, 60);
  torchOn = false;
  digitalWrite(LED_PIN, LOW);
  gamePaused = false;
  shooterPaused = false;
  emoBannerUntil = 0;
  if (state == SETTINGS) saveSettings();   // keep what you changed
  if (state == MUSIC) { saveTempo(); musicStopAll(); }   // keep the tempo, silence every tab
  stopTone();
  toyBusy = false;
  menuTiltZero = tiltOtherRaw;             // tilt-scroll measures from the pose you leave a mode in
  timerPhase = TP_SETUP;                   // leaving the timer cancels it
  timerPaused = false;
  rxPhase = RX_READY;
  setRGB(C_OFF);          // LED is always off in the menu
  state = MENU;
  lastActivityTime = millis();
  drawMenu();
}

// back from DODGE / SHOOTER to the games list
void goToGames() {
  beep(600, 60);
  gamePaused = false;
  shooterPaused = false;
  setRGB(C_OFF);
  state = GAMES;
  lastActivityTime = millis();
  initGames();
}

// ---------------- Gestures ----------------
// redraws whatever the current mode shows (used after waking from face-down sleep)
void redrawCurrent() {
  lastLevelDraw = 0;
  lastMusDraw = 0;
  lastDizzyFrame = 0;
  toyDirty = true;
  tempDirty = true;
  switch (state) {
    case MENU: if (idleActive) drawIdle(idleEyesOpen); else drawMenu(); break;
    case EMO: drawEmo(); break;
    case GAMES: drawGames(); break;
    case GAME: drawGame(); break;
    case STOPWATCH: drawStopwatch(); break;
    case TORCH: drawTorch(); break;
    case TEMP: drawTemp(); break;
    case SHOOTER: drawShooter(); break;
    case TIMER: drawTimer(); break;
    case REACTION: drawReaction(); break;
    case SETTINGS: drawSettings(); break;
    default: break;   // LEVEL, MUSIC and TOYS redraw themselves on the next pass
  }
}

void gestureSleep() {
  beep(500, 80);                       // goodnight beep, then everything goes quiet
  delay(100);
  gestureAsleep = true;
  stopTone();
  setRGB(C_OFF);
  digitalWrite(LED_PIN, LOW);
  display.ssd1306_command(SSD1306_DISPLAYOFF);
}

void gestureWake() {
  gestureAsleep = false;
  faceDownSince = 0;
  display.ssd1306_command(SSD1306_DISPLAYON);
  digitalWrite(LED_PIN, torchOn ? HIGH : LOW);
  rgbRefresh();
  lastActivityTime = millis();
  redrawCurrent();
  beep(1200, 40);
}

// flip the board face-down: screen off and silent.  Flip it back (or press a button) to wake it.
void updateGestures() {
  if (!gesturesOn || !mpuOK) {
    faceDownSince = 0;
    if (gestureAsleep) gestureWake();
    return;
  }
  unsigned long now = millis();
  if (!gestureAsleep) {
    if (accelZ < -6.0f && now > gestureCooldown) {
      if (faceDownSince == 0) faceDownSince = now;
      else if (now - faceDownSince > 700) gestureSleep();
    } else {
      faceDownSince = 0;
    }
  } else {
    if (accelZ > 4.0f) {
      gestureWake();
    } else if (btnJoy.pressed || btnFire.pressed || btnBack.pressed || btnSelect.pressed) {
      gestureCooldown = now + 3000;    // woken by a button while still face-down: don't fall asleep again at once
      gestureWake();
    }
  }
}

// the double-tap "extra SELECT" would get in the way in these modes
bool tapAllowed() {
  return !(state == GAME || state == SHOOTER || state == REACTION || state == LEVEL ||
           state == MUSIC || state == SETTINGS);
}

void handleMenu(int v) {
  bool navInput = (v < 1200 || v > 2900);
  bool anyPress = btnJoy.pressed || btnSelect.pressed || btnFire.pressed || btnBack.pressed;
  bool confirmPressed = btnJoy.pressed || btnSelect.pressed;
  if (!menuTiltInit && mpuOK && lastMpuRead != 0) { menuTiltZero = tiltOtherRaw; menuTiltInit = true; }

  if (idleActive) {
    // any input (joystick, a button, a shake) wakes the screen back to the menu
    if (navInput || anyPress || shakeTriggered) {
      idleActive = false;
      lastActivityTime = millis();
      menuTiltZero = tiltOtherRaw;
      drawMenu();
    } else {
      unsigned long phaseElapsed = millis() - idlePhaseStart;
      if (idleEyesOpen && phaseElapsed > IDLE_OPEN_MS) {
        idleEyesOpen = false;
        idlePhaseStart = millis();
      } else if (!idleEyesOpen && phaseElapsed > IDLE_BLINK_MS) {
        idleEyesOpen = true;
        idlePhaseStart = millis();
      }
      static unsigned long lastIdleDraw = 0;               // redraw ~16x a second so the eyes follow the tilt
      if (millis() - lastIdleDraw >= 60) {
        lastIdleDraw = millis();
        if (spinDizzy) drawDizzy(); else drawIdle(idleEyesOpen);
      }
    }
    return;
  }

  if (anyPress || shakeTriggered) lastActivityTime = millis();

  if (ssTimes[ssIdx] != 0 && millis() - lastActivityTime > ssTimes[ssIdx]) {
    idleActive = true;
    idleEyesOpen = true;
    idlePhaseStart = millis();
    drawIdle(true);
    return;
  }

  if (millis() - lastMove > 250) {
    if (v < 1200) { selected = (selected - 1 + MENU_COUNT) % MENU_COUNT; lastMove = millis(); lastActivityTime = millis(); beep(1000, 15); drawMenu(); }
    else if (v > 2900) { selected = (selected + 1) % MENU_COUNT; lastMove = millis(); lastActivityTime = millis(); beep(1000, 15); drawMenu(); }
  }

  // tilt the board forward / back to scroll the menu (needs GESTURES: ON)
  if (gesturesOn && mpuOK) {
    float rel = (tiltOtherRaw - menuTiltZero) * MENU_TILT_SIGN;
    if (millis() - lastTiltMove > 380) {
      if (rel > MENU_TILT_DEG) {
        selected = (selected + 1) % MENU_COUNT;
        lastTiltMove = millis(); lastActivityTime = millis(); beep(1000, 15); drawMenu();
      } else if (rel < -MENU_TILT_DEG) {
        selected = (selected - 1 + MENU_COUNT) % MENU_COUNT;
        lastTiltMove = millis(); lastActivityTime = millis(); beep(1000, 15); drawMenu();
      }
    }
    static unsigned long lastDrift = 0;                    // a slow change of grip is not a scroll
    if (fabsf(rel) < MENU_TILT_DEG && millis() - lastDrift > 100) {
      lastDrift = millis();
      menuTiltZero += (tiltOtherRaw - menuTiltZero) * 0.03f;
    }
  }

  if (confirmPressed) {
    lastActivityTime = millis();
    if (btnJoy.pressed) swallowJoyRelease = true; // the release of this click must not pause a game
    beep(1800, 60);

    switch (selected) {
      case 0: state = EMO; lastEmoMove = 0; emoBannerUntil = 0; drawEmo(); break;
      case 1: state = GAMES; initGames(); break;
      case 2: state = STOPWATCH; lastSwDraw = 0; drawStopwatch(); break;
      case 3: state = TORCH; lastTorchMove = millis(); setTorch(true); break;
      case 4: state = TEMP; tempPage = 0; tempDirty = true; drawTemp(); break;
      case 5: state = TIMER; initTimer(); break;
      case 6: state = LEVEL; initLevel(); break;
      case 7: state = REACTION; rxPhase = RX_READY; drawReaction(); break;
      case 8: state = MUSIC; initMusic(); break;
      case 9: state = TOYS; initToys(); break;
      case 10: state = SETTINGS; initSettings(); break;
      default: break;
    }
  }
}

void handleGame(bool joyReleased, unsigned long joyHeld) {
  if (gameOver) {
    bool guardPassed = (millis() - gameOverAt > RESTART_GUARD_MS);
    if (btnJoy.pressed) {
      goToGames();
    } else if (guardPassed && (btnSelect.pressed || btnFire.pressed || shakeTriggered)) {
      beep(1700, 60);
      initGame();
      drawGame();
    }
    return;
  }

  if (joyReleased) {
    if (joyHeld >= LONG_PRESS_MS) {
      goToGames();
      return;
    }
    gamePaused = !gamePaused;
    beep(gamePaused ? 500 : 1200, 50);
    drawGame();
  }
  if (btnSelect.pressed) {
    gamePaused = !gamePaused;
    beep(gamePaused ? 500 : 1200, 50);
    drawGame();
  }

  if (!gamePaused && millis() - lastGameUpdate > 40) {
    updateGame();
    lastGameUpdate = millis();
    drawGame();
  }
}

void handleShooter(bool joyReleased, unsigned long joyHeld) {
  if (shooterOver) {
    bool guardPassed = (millis() - gameOverAt > RESTART_GUARD_MS);
    if (btnJoy.pressed) {
      goToGames();
    } else if (guardPassed && (btnSelect.pressed || btnFire.pressed || shakeTriggered)) {
      beep(1700, 60);
      initShooter();
      drawShooter();
    }
    return;
  }

  if (btnFire.pressed) fireQueued = true;

  if (joyReleased) {
    if (joyHeld >= LONG_PRESS_MS) {
      goToGames();
      return;
    }
    shooterPaused = !shooterPaused;
    beep(shooterPaused ? 500 : 1200, 50);
    drawShooter();
  }
  if (btnSelect.pressed) {
    shooterPaused = !shooterPaused;
    beep(shooterPaused ? 500 : 1200, 50);
    drawShooter();
  }

  if (!shooterPaused && millis() - lastShooterUpdate > 40) {
    updateShooter();
    lastShooterUpdate = millis();
    drawShooter();
  }
}

void handleModes() {
  bool joyReleased = btnJoy.released;
  unsigned long joyHeld = btnJoy.held;
  if (joyReleased && swallowJoyRelease) {
    swallowJoyRelease = false;
    joyReleased = false;
  }

  // BACK steps back one level: game -> games list -> main menu
  if (btnBack.pressed) {
    if (state == GAME || state == SHOOTER) goToGames(); else goToMenu();
    return;
  }

  switch (state) {
    case TEMP: {
      int vy = analogRead(VRY_PIN);       // left / right = page
      if (millis() - lastPageMove > 300) {
        if (vy < 1200) { tempPage = (tempPage + 2) % 3; lastPageMove = millis(); beep(1000, 12); tempDirty = true; }
        else if (vy > 2900) { tempPage = (tempPage + 1) % 3; lastPageMove = millis(); beep(1000, 12); tempDirty = true; }
      }
      if (btnSelect.pressed) {            // C <-> F (saved)
        useF = !useF;
        prefs.putBool("usef", useF);
        beep(1300, 30);
        tempDirty = true;
      }
      if (btnFire.released) {
        if (btnFire.held >= LONG_PRESS_MS) { resetTempStats(); doubleBeep(); }   // hold = reset
        else if (tempPage == 1) { graphHum = !graphHum; beep(1100, 15); }        // tap on graph = temp/humidity
        tempDirty = true;
      }
      if (btnJoy.pressed) { goToMenu(); break; }
      if (tempDirty) { drawTemp(); tempDirty = false; }
      break;
    }

    case TORCH: {
      if (btnSelect.pressed) {            // on / off
        setTorch(!torchOn);
        beep(torchOn ? 1500 : 900, 40);
      }
      if (btnFire.pressed) {              // next colour
        torchColorIdx = (torchColorIdx + 1) % TORCH_COUNT;
        if (!torchOn) torchOn = true;
        digitalWrite(LED_PIN, HIGH);
        beep(1100, 15);
        drawTorch();
      }

      int tx = analogRead(VRX_PIN);       // up / down  -> brightness
      int ty = analogRead(VRY_PIN);       // left / right -> colour
      if (millis() - lastTorchMove > 280) {
        int dx = abs(tx - 2048);
        int dy = abs(ty - 2048);
        bool moved = false;
        if (dy >= dx && dy > 900) {
          torchColorIdx = (torchColorIdx + (ty > 2048 ? 1 : TORCH_COUNT - 1)) % TORCH_COUNT;
          moved = true;
        } else if (dx > 900) {
          int step = (tx < 2048) ? 1 : -1;  // joystick up (VRX low) = brighter
          torchLevelIdx = constrain(torchLevelIdx + step, 0, TORCH_LEVEL_COUNT - 1);
          moved = true;
        }
        if (moved) {
          lastTorchMove = millis();
          if (!torchOn) {
            torchOn = true;
            digitalWrite(LED_PIN, HIGH);
          }
          beep(1100, 12);
          drawTorch();
        }
      }

      if (btnJoy.pressed) goToMenu();
      break;
    }

    case STOPWATCH: {
      // SELECT = start / pause.  FIRE must be HELD to reset, and the joystick does nothing here,
      // so a bump or a stick springing back can never wipe the time.
      if (btnSelect.pressed) swToggle();
      if (btnFire.pressed) swResetArmed = true;
      if (swResetArmed && btnFire.stable == LOW && millis() - btnFire.downTime >= LONG_PRESS_MS) {
        swResetArmed = false;
        swReset();
      }
      if (btnFire.released) swResetArmed = false;
      if (btnJoy.pressed) { goToMenu(); break; }

      if (swRunning && millis() - lastSwDraw > 50) {
        drawStopwatch();
        lastSwDraw = millis();
      }
      break;
    }

    case EMO: {
      int vx = analogRead(VRX_PIN);

      // shake the board: unlocks the DIZZY face (once)
      if (shakeTriggered && !dizzyUnlocked) unlockDizzy();

      if (emoBannerUntil != 0) {
        if (millis() > emoBannerUntil) {      // banner done, back to the face list
          emoBannerUntil = 0;
          drawEmo();
        }
      } else {
        int dir = 0;
        if (millis() - lastEmoMove > 300) {
          if (vx < 1200) dir = -1;
          else if (vx > 2900) dir = 1;
        }
        if (btnSelect.pressed) dir = 1;
        if (btnFire.pressed) dir = -1;
        if (dir != 0) {
          int n = emoCount();
          emoIndex = (emoIndex + dir + n) % n;
          lastEmoMove = millis();
          beep(emoIndex == EMO_DIZZY ? 450 : 1000, 15);
          lastDizzyFrame = millis();
          drawEmo();
        }

        // redraw ~20x a second: the pupils follow the tilt, the eyes blink, the dizzy face spins
        if (millis() - lastDizzyFrame > 50) {
          lastDizzyFrame = millis();
          updateBlink();
          drawEmo();
        }
      }

      if (btnJoy.pressed) goToMenu();
      break;
    }

    case GAME:
      handleGame(joyReleased, joyHeld);
      break;

    case SHOOTER:
      handleShooter(joyReleased, joyHeld);
      break;

    case TIMER:
      handleTimer();
      break;

    case LEVEL:
      handleLevel();
      break;

    case REACTION:
      handleReaction();
      break;

    case GAMES:
      handleGames();
      break;

    case MUSIC:
      handleMusic(joyReleased);
      break;

    case TOYS:
      handleToys();
      break;

    case SETTINGS:
      handleSettings();
      break;

    default:
      break;
  }
}

// ---------------- Arduino entry points ----------------
void setup() {
  Serial.begin(115200);
  rgbInit();                // LED off before anything else
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();
  randomSeed(micros());

  prefs.begin("deskbuddy", false);
  loadSettings();          // sound / LED brightness / screensaver / shake level / best reaction time
  prefs.remove("pf"); prefs.remove("pm"); prefs.remove("pe");   // the old pet is gone: free its flash
  dizzyUnlocked = false;   // easter egg: locked at every power-up, never saved (not stored in prefs)

  pinMode(SW_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_FIRE_PIN, INPUT_PULLUP);
  pinMode(BTN_BACK_PIN, INPUT_PULLUP);
  pinMode(BTN_SELECT_PIN, INPUT_PULLUP);

  mpuOK = mpu.begin();
  if (mpuOK) {
    mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  }
  Serial.println(mpuOK ? "MPU6050: found" : "MPU6050: NOT found (check SDA/SCL/VCC/GND)");
  Serial.println(RGB_COMMON_ANODE ? "RGB LED: common-anode mode" : "RGB LED: common-cathode mode");
  Serial.println("DIZZY emo: locked (easter egg - shake the board in EMO mode)");
  Serial.println(gesturesOn ? "Gestures: ON (face-down sleep, double-tap, tilt-scroll menu)" : "Gestures: OFF");

  bootAnimation();
  drawMenu();
  lastActivityTime = millis();
}

void loop() {
  updateMPU();
  updateButton(btnJoy);
  updateButton(btnFire);
  updateButton(btnBack);
  updateButton(btnSelect);

  updateGestures();                         // face-down = sleep, flip back = wake
  if (gestureAsleep) {                      // screen and sound are off; keep logging the temperature
    tempBackground();
    delay(10);
    return;
  }
  if (gesturesOn && doubleTap && tapAllowed()) btnSelect.pressed = true;   // double tap = an extra SELECT

  if (state == MENU) {
    handleMenu(analogRead(VRX_PIN));
  } else {
    handleModes();
  }

  tempBackground();
  updateRGB();

#if DEBUG_TILT
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 300) {
    lastDbg = millis();
    Serial.print("tilt=");
    Serial.print(tiltRaw - tiltZero);
    Serial.print(" g=");
    Serial.println(accelG);
  }
#endif
}
