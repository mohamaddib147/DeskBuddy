#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

Adafruit_SSD1306 display(128, 64, &Wire, -1);

#define DHTPIN 15
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

const int VRX_PIN = 34;
const int VRY_PIN = 35;
const int SW_PIN = 32;
const int LED_PIN = 26;
const int BUZZER_PIN = 27;

const char* menuItems[] = {"EMO MODE", "GAME", "STOPWATCH", "TORCH", "TEMP"};
const int MENU_COUNT = 5;
const int ROW_H = 12; // 5 rows x 12px = 60px, fits the 64px screen

enum State { MENU, EMO, GAME, STOPWATCH, TORCH, TEMP };
State state = MENU;

int selected = 0;
unsigned long lastMove = 0;
unsigned long lastTempRead = 0;
bool lastButton = HIGH;

// --- Stopwatch state ---
unsigned long swElapsed = 0;     // accumulated time while paused, in ms
unsigned long swStartTime = 0;   // millis() when it was last (re)started
bool swRunning = false;
unsigned long lastSwDraw = 0;
unsigned long lastSwMove = 0;

// --- Emo mode state ---
const char* emoNames[] = {"HAPPY", "SAD", "ANGRY", "SURPRISED", "WINK"};
const int EMO_COUNT = 5;
int emoIndex = 0;
unsigned long lastEmoMove = 0;

// --- Game state ---
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
unsigned long lastGameUpdate = 0;
unsigned long lastObstacleSpawn = 0;
unsigned long gameSpawnInterval = 900;
float gameFallSpeed = 1.5;

// --- Buzzer helpers ---
void beep(int freq, int durMs) {
  tone(BUZZER_PIN, freq, durMs);
}

void doubleBeep() {
  tone(BUZZER_PIN, 700, 40);
  delay(60); // short and infrequent (only on stopwatch reset) - fine to block here
  tone(BUZZER_PIN, 700, 40);
}

void drawMenu() {
  display.clearDisplay();
  for (int i = 0; i < MENU_COUNT; i++) {
    int y = i * ROW_H;
    if (i == selected) {
      display.fillRect(0, y, 128, ROW_H, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y + 2);
    display.setTextSize(1);
    display.println(menuItems[i]);
  }
  display.display();
}

void drawTemp() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("TEMP");

  display.setTextSize(2);
  display.setCursor(0, 16);
  if (isnan(t)) {
    display.println("--.-C");
  } else {
    display.print(t, 1);
    display.println("C");
  }

  display.setTextSize(2);
  display.setCursor(0, 40);
  if (isnan(h)) {
    display.println("--%");
  } else {
    display.print(h, 0);
    display.println("%");
  }

  display.display();
}

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

  display.setTextSize(2);
  display.setCursor(0, 20);
  char buf[12];
  sprintf(buf, "%02u:%02u.%02u", minutes, seconds, hundredths);
  display.println(buf);

  display.setTextSize(1);
  display.setCursor(0, 50);
  display.println(swRunning ? "UP:pause DN:reset" : "UP:start DN:reset");

  display.display();
}

void drawRobotEye(int x, int y, int w, int h, int r, int topCutSide, int cutAmt) {
  display.fillRoundRect(x, y, w, h, r, SSD1306_WHITE);
  if (topCutSide == 1 && cutAmt > 0) {
    display.fillTriangle(x, y, x + cutAmt, y, x, y + cutAmt, SSD1306_BLACK);
  } else if (topCutSide == 2 && cutAmt > 0) {
    display.fillTriangle(x + w - cutAmt, y, x + w, y, x + w, y + cutAmt, SSD1306_BLACK);
  }
}

void drawEmo() {
  display.clearDisplay();

  int eyeW = 38, eyeH = 30, gap = 10;
  int totalW = eyeW * 2 + gap;
  int leftX = (128 - totalW) / 2;
  int rightX = leftX + eyeW + gap;
  int topY = 10;
  int r = 10;

  switch (emoIndex) {
    case 0: // HAPPY
      drawRobotEye(leftX, topY, eyeW, eyeH, r, 2, 14);
      drawRobotEye(rightX, topY, eyeW, eyeH, r, 1, 14);
      break;

    case 1: // SAD
      drawRobotEye(leftX, topY + 6, eyeW, eyeH - 6, r, 1, 14);
      drawRobotEye(rightX, topY + 6, eyeW, eyeH - 6, r, 2, 14);
      break;

    case 2: // ANGRY
      drawRobotEye(leftX + 4, topY + 4, eyeW - 8, eyeH - 10, 2, 2, 18);
      drawRobotEye(rightX - 4, topY + 4, eyeW - 8, eyeH - 10, 2, 1, 18);
      break;

    case 3: // SURPRISED
      drawRobotEye(leftX - 4, topY - 4, eyeW + 8, eyeH + 8, 14, 0, 0);
      drawRobotEye(rightX - 4, topY - 4, eyeW + 8, eyeH + 8, 14, 0, 0);
      break;

    case 4: // WINK
      display.fillRoundRect(leftX, topY + eyeH / 2 - 3, eyeW, 6, 3, SSD1306_WHITE);
      drawRobotEye(rightX, topY, eyeW, eyeH, r, 1, 14);
      break;
  }

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 56);
  display.print(emoNames[emoIndex]);

  display.display();
}

void initGame() {
  for (int i = 0; i < MAX_OBSTACLES; i++) obstacles[i].active = false;
  playerX = 64 - PLAYER_W / 2;
  gameScore = 0;
  gameOver = false;
  lastGameUpdate = millis();
  lastObstacleSpawn = millis();
  gameSpawnInterval = 900;
  gameFallSpeed = 1.5;
}

void updateGame() {
  int v = analogRead(VRY_PIN);
  playerX = map(v, 0, 4095, 0, 128 - PLAYER_W);
  playerX = constrain(playerX, 0, 128 - PLAYER_W);

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
      if (gameScore > highScore) highScore = gameScore;
      beep(200, 300); // collision buzz
      return;
    }

    if (obstacles[i].y > 64) {
      obstacles[i].active = false;
      gameScore++;
      beep(2000, 20); // dodge ding
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
    display.setCursor(30, 14);
    display.println("GAME OVER");
    display.setCursor(30, 28);
    display.print("Score: ");
    display.println(gameScore);
    display.setCursor(24, 40);
    display.print("Best: ");
    display.println(highScore);
    display.setCursor(14, 54);
    display.println("press to go back");
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
  display.setCursor(0, 0);
  display.print("HI ");
  display.print(highScore);

  display.setCursor(96, 0);
  display.print(gameScore);

  display.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  dht.begin();
  randomSeed(micros());

  pinMode(SW_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  drawMenu();
}

void loop() {
  int v = analogRead(VRX_PIN);
  bool button = digitalRead(SW_PIN);
  bool pressed = (lastButton == HIGH && button == LOW);
  lastButton = button;

  if (state == MENU) {
    if (millis() - lastMove > 250) {
      if (v < 1200) { selected = (selected - 1 + MENU_COUNT) % MENU_COUNT; lastMove = millis(); beep(1000, 15); drawMenu(); }
      else if (v > 2900) { selected = (selected + 1) % MENU_COUNT; lastMove = millis(); beep(1000, 15); drawMenu(); }
    }

    if (pressed) {
      beep(1800, 60); // confirm select

      if (selected == 4) { state = TEMP; lastTempRead = 0; }
      else if (selected == 3) { state = TORCH; digitalWrite(LED_PIN, HIGH); }
      else if (selected == 2) { state = STOPWATCH; lastSwDraw = 0; drawStopwatch(); }
      else if (selected == 1) { state = GAME; initGame(); drawGame(); }
      else if (selected == 0) { state = EMO; lastEmoMove = 0; drawEmo(); }
      else { state = (State)(selected + 1); }

      if (state != TORCH && state != TEMP && state != STOPWATCH && state != EMO && state != GAME) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println(menuItems[selected]);
        display.println("(coming soon)");
        display.println("");
        display.println("press to go back");
        display.display();
      }
    }
  } else {
    if (state == TEMP && millis() - lastTempRead > 1500) {
      drawTemp();
      lastTempRead = millis();
    }

    if (state == STOPWATCH) {
      int vy = analogRead(VRY_PIN);

      if (millis() - lastSwMove > 300) {
        if (vy < 1200) {
          if (swRunning) {
            swElapsed += millis() - swStartTime;
            swRunning = false;
            beep(900, 50); // pause
          } else {
            swStartTime = millis();
            swRunning = true;
            beep(1500, 50); // start
          }
          lastSwMove = millis();
          drawStopwatch();
          lastSwDraw = millis();
        } else if (vy > 2900) {
          swRunning = false;
          swElapsed = 0;
          lastSwMove = millis();
          doubleBeep(); // reset
          drawStopwatch();
          lastSwDraw = millis();
        }
      }

      if (swRunning && millis() - lastSwDraw > 50) {
        drawStopwatch();
        lastSwDraw = millis();
      }
    }

    if (state == EMO) {
      int vx = analogRead(VRX_PIN);

      if (millis() - lastEmoMove > 300) {
        if (vx < 1200) { emoIndex = (emoIndex - 1 + EMO_COUNT) % EMO_COUNT; lastEmoMove = millis(); beep(1000, 15); drawEmo(); }
        else if (vx > 2900) { emoIndex = (emoIndex + 1) % EMO_COUNT; lastEmoMove = millis(); beep(1000, 15); drawEmo(); }
      }
    }

    if (state == GAME) {
      if (!gameOver && millis() - lastGameUpdate > 40) {
        updateGame();
        lastGameUpdate = millis();
      }
      drawGame();
    }

    if (pressed) {
      beep(600, 60); // back to menu
      digitalWrite(LED_PIN, LOW);
      state = MENU;
      drawMenu();
    }
  }
}