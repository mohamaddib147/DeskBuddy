# DeskBuddy

A pocket-sized ESP32 gadget with a tiny robot face, games, a stopwatch, a weather station, a spirit level, a music workshop and more, all on a 128x64 OLED, an analog joystick and three buttons.

**Current version: v11** (single sketch, `DeskBuddy.ino`, about 4,400 lines, no extra files).

---

## Contents

1. [Features at a glance](#features-at-a-glance)
2. [Hardware](#hardware)
3. [Wiring](#wiring)
4. [Software setup](#software-setup)
5. [Controls](#controls)
6. [Modes in detail](#modes-in-detail)
7. [Settings](#settings)
8. [What is saved](#what-is-saved)
9. [Customising the sketch](#customising-the-sketch)
10. [Troubleshooting](#troubleshooting)
11. [Version history](#version-history)

---

## Features at a glance

| Menu item | What it does |
|---|---|
| **EMO MODE** | An animated robot face whose pupils follow how you tilt the board. Five moods, plus a hidden DIZZY face. |
| **GAMES** | A games list with **DODGE** (avoid falling blocks) and **SHOOTER** (blast the enemies). Steer with the stick or by tilting the board. |
| **STOPWATCH** | Start, pause, and hold-to-reset stopwatch with hundredths of a second. |
| **TORCH** | The RGB LED as a coloured torch: 10 colours (including a rainbow) and 5 brightness levels. Also switches the plain torch LED. |
| **TEMP** | A three-page weather instrument: live reading with trend, a 33-minute graph, and statistics. |
| **TIMER** | Pomodoro / countdown timer with work and break phases. |
| **LEVEL** | A spirit level: a bubble on screen, a green LED when the board is flat. |
| **REACTION** | Reaction-time tester with a saved best time. |
| **MUSIC** | Seven tabs: metronome, piano with looper, tilt theremin, drum machine, songs, tuner and a retro 8-bit sound bank. |
| **TOYS** | Magic 8-ball, dice (1 to 3) and a coin flip. Shake the board to roll. |
| **SETTINGS** | Sound, LED brightness, screensaver, shake sensitivity, sensor calibration, gestures and reset. |

Extras that work everywhere:

- **Screensaver:** after a while the menu turns into a pair of idle eyes that follow your tilt.
- **Tilt to scroll:** tip the board forward or back to move through the menu.
- **Face-down sleep:** put the board face-down and the screen and sound switch off. Flip it back or press any button to wake it.
- **Double tap:** tap the board twice for an extra SELECT press (not in games, level, reaction, music or settings).
- **Spin it:** spin the board and the face gets dizzy.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32 development board | Any common ESP32 DevKit (30 or 38 pin). Board setting in the IDE: **ESP32 Dev Module**. |
| OLED display | 128x64, SSD1306 controller, **I2C**, address `0x3C`. |
| MPU6050 module (GY-521) | I2C, address `0x68`. Gives tilt, shake, spin, tap and face-down detection. |
| DHT11 sensor | Temperature and humidity. Bare sensor or 3-pin module. |
| Analog joystick module | Two potentiometer axes plus a push switch (the "click"). |
| 3 push buttons | FIRE, BACK, SELECT. Any momentary buttons. |
| Passive buzzer | **Passive**, not active. The sketch plays real notes with `tone()`. |
| RGB LED | Common-cathode by default (common-anode supported by one setting). |
| 4 x 220 ohm resistors | One for each RGB colour leg and one for the torch LED. |
| 1 plain LED (optional) | The "torch" LED. Leave it out if you do not want it. |
| Breadboard or perfboard, jumper wires | |

Everything runs from **3.3 V**. Power the board over USB.

---

## Wiring

### Pin map

| Part | Part pin | ESP32 pin | Notes |
|---|---|---|---|
| **OLED** | SDA | **GPIO 21** | I2C bus, shared with the MPU6050 |
| | SCL | **GPIO 22** | |
| | VCC | 3V3 | |
| | GND | GND | |
| **MPU6050** | SDA | **GPIO 21** | same two wires as the OLED |
| | SCL | **GPIO 22** | |
| | VCC | 3V3 | |
| | GND | GND | |
| | AD0 | GND (or left open on most modules) | keeps the address at `0x68` |
| **DHT11** | DATA | **GPIO 15** | needs a 4.7 k to 10 k pull-up to 3V3 if you use the bare 4-pin sensor. 3-pin modules already have it. |
| | VCC | 3V3 | |
| | GND | GND | |
| **Joystick** | VRX | **GPIO 34** | stick **up / down** axis (input-only ADC pin) |
| | VRY | **GPIO 35** | stick **left / right** axis (input-only ADC pin) |
| | SW | **GPIO 32** | the click, active LOW, uses the internal pull-up |
| | VCC | 3V3 | |
| | GND | GND | |
| **FIRE button** | one leg | **GPIO 4** | other leg to GND. No resistor needed (internal pull-up). |
| **BACK button** | one leg | **GPIO 5** | other leg to GND |
| **SELECT button** | one leg | **GPIO 23** | other leg to GND |
| **Passive buzzer** | + | **GPIO 27** | the other pin to GND |
| **Torch LED** (optional) | anode (long leg) | **GPIO 26** via 220 ohm | cathode (short leg) to GND |
| **RGB LED** | Red leg | **GPIO 16** via 220 ohm | |
| | Green leg | **GPIO 17** via 220 ohm | |
| | Blue leg | **GPIO 19** via 220 ohm | |
| | Common leg | **GND** (common-cathode) | or **3V3** for common-anode, see below |

### Block diagram

```
                           +-------------------------------+
   OLED SSD1306 -- SDA ----|  GPIO21                       |
   (0x3C)       -- SCL ----|  GPIO22                       |
   MPU6050      -- SDA ----|  (same two wires)             |
   (0x68)       -- SCL ----|                               |
                           |                               |
   DHT11 DATA -------------|  GPIO15                       |
                           |                               |
   Joystick VRX (up/down) -|  GPIO34  (input only)         |
   Joystick VRY (left/rt) -|  GPIO35  (input only)         |
   Joystick SW (click) ----|  GPIO32                       |
                           |            ESP32              |
   FIRE   button -> GND ---|  GPIO4                        |
   BACK   button -> GND ---|  GPIO5                        |
   SELECT button -> GND ---|  GPIO23                       |
                           |                               |
   Passive buzzer + -------|  GPIO27   (buzzer - to GND)   |
   Torch LED (220R) -------|  GPIO26   (LED cathode to GND)|
                           |                               |
   RGB red   (220R) -------|  GPIO16                       |
   RGB green (220R) -------|  GPIO17                       |
   RGB blue  (220R) -------|  GPIO19                       |
                           |                               |
   All VCC pins -----------|  3V3          GND -- all GNDs |
                           +-------------------------------+
```

### Wiring notes

- **I2C devices share two wires.** The OLED and the MPU6050 both connect to GPIO 21 (SDA) and GPIO 22 (SCL). They do not clash because they have different addresses (`0x3C` and `0x68`).
- **Buttons need no resistors.** The sketch turns on the ESP32's internal pull-ups. A button just connects its pin to GND when pressed.
- **GPIO 34 and 35 are input-only.** That is fine for the joystick axes, which are only ever read.
- **Joystick orientation.** The sketch treats VRX as the up/down axis and VRY as the left/right axis. If your joystick module is mounted differently, the stick will feel rotated. Swap the two wires (or the two `VRX_PIN` / `VRY_PIN` numbers at the top of the sketch) to fix it.
- **Passive buzzer only.** An *active* buzzer (one that beeps by itself when powered) will make only a flat tone and ruin the music tab. If your buzzer has a sticker on top and sounds when you connect it straight to 3V3, it is active.
- **RGB LED polarity.**
  - **Common-cathode** (default): the long common leg goes to **GND**. A pin set HIGH lights the colour.
  - **Common-anode:** the common leg goes to **3V3**, and you set `#define RGB_COMMON_ANODE 1` at the top of the sketch.
  - Quick check: at boot the screen shows "LED test: OFF" and the LED must be completely dark. If it glows, flip `RGB_COMMON_ANODE`. The test then flashes red, green, blue, and the screen names each colour.
- **Boot-time pins.** GPIO 5 (BACK) and GPIO 15 (DHT) are ESP32 "strapping" pins that are sampled at reset. They work fine in this wiring, but **do not hold the BACK button while you reset or power on the board.**
- **Torch LED is optional.** Leaving GPIO 26 unconnected does no harm.

---

## Software setup

### 1. Arduino IDE and ESP32 board package

1. Install the **Arduino IDE** (1.8.x or 2.x).
2. Add the ESP32 board package (Boards Manager, search "esp32" by Espressif). The sketch compiles on both **core 2.x and core 3.x**.
3. Select the board: **Tools > Board > ESP32 Dev Module**.

### 2. Libraries (Library Manager)

| Library | Author |
|---|---|
| Adafruit GFX Library | Adafruit |
| Adafruit SSD1306 | Adafruit |
| DHT sensor library | Adafruit |
| Adafruit Unified Sensor | Adafruit |
| Adafruit MPU6050 | Adafruit |
| Adafruit BusIO | Adafruit (installed automatically as a dependency) |

`Wire` and `Preferences` come with the ESP32 core, so there is nothing else to install.

### 3. Upload

1. Put the sketch in a folder with the **same name** as the file. For example `DeskBuddy/DeskBuddy.ino`.
2. Plug in the ESP32 over USB, pick the right COM port.
3. Click **Upload**. If the upload hangs on "Connecting...", hold the board's **BOOT** button until it starts.
4. Open the Serial Monitor at **115200 baud** to see start-up messages (whether the MPU6050 was found, the LED mode, whether gestures are on).

### 4. First boot

You will see the eyes open, the "DESKBUDDY" title with "motion OK" (or "motion --" if the MPU6050 is not found), then the RGB LED test, then the menu.

---

## Controls

### Everywhere

| Input | Action |
|---|---|
| **Stick up / down** | Scroll the menu |
| **Tilt the board forward / back** | Also scrolls the menu (needs GESTURES: ON) |
| **Joystick click** or **SELECT** | Open the highlighted item |
| **BACK** | Go back one level. From a game it goes to the games list. From anywhere else it goes to the main menu. |
| **Joystick click** (inside most modes) | Back to the main menu |
| **Face-down** for 0.7 s | Screen and sound off. Flip back or press any button to wake. |
| **Double tap** on the board | An extra SELECT (not in games, level, reaction, music, settings) |

### Quick reference per mode

| Mode | Stick | FIRE | SELECT | Click |
|---|---|---|---|---|
| EMO | up/down: change face | previous face | next face | menu |
| GAMES | up/down: pick | play | play | play |
| DODGE / SHOOTER | left/right: move | (Shooter) shoot | pause | tap: pause, hold: exit |
| STOPWATCH | - | hold: reset | start / pause | menu |
| TORCH | left/right: colour, up/down: brightness | next colour | on / off | menu |
| TEMP | left/right: page | graph: temp/humidity, hold: reset stats | C / F | menu |
| TIMER | up/down: minutes | switch WORK/BREAK field, or stop | start / pause | menu |
| LEVEL | - | clear zero | set zero here | menu |
| REACTION | - | start / react | start | menu |
| MUSIC | see the MUSIC section | | | tap: next tab, hold = SHIFT |
| TOYS | left/right: toy, up/down: dice count | roll | roll | menu |
| SETTINGS | up/down: row, left/right: value | change | change | save and exit |

---

## Modes in detail

### EMO MODE

Five faces: **HAPPY, SAD, ANGRY, SURPRISED, WINK**. The pupils follow how you tilt the board, the eyes blink on their own, and the RGB LED glows in a colour matching the mood.

- **Change face:** stick up/down, or FIRE (previous) / SELECT (next).
- **Spin the board** and the face turns into the spinning-spiral DIZZY face until you stop.
- **Easter egg:** shake the board hard once in EMO mode and **DIZZY** is unlocked as a sixth face in the list, with a banner and a small fanfare. It is locked again at every power-up (never saved).

### GAMES

A list with **DODGE** and **SHOOTER**. Stick up/down chooses, SELECT / FIRE / click starts. Your high score for each is shown. High scores are kept only until you power off.

**Steering (both games):** tilt the board left or right. The tilt is measured from the pose you hold when the game starts. If you do not tilt past a small dead-zone, the joystick (left/right) steers instead.

**DODGE:** slide the paddle along the bottom and avoid the falling blocks. Every block you dodge scores a point, and the game speeds up as you go.

**SHOOTER:** your ship sits at the bottom. **Hold FIRE** (or push the stick up) to shoot the falling enemies. You have 3 lives. An enemy reaching the bottom costs a life.

- **Pause:** a quick click or SELECT.
- **Exit:** BACK, or hold the click for half a second. You land on the games list.
- **Game over:** the LED turns solid red. After one second, SELECT / FIRE / a shake plays again; BACK or click returns to the games list.

### STOPWATCH

- **SELECT** starts and pauses it (LED green while running, red when stopped with a time on the clock, off when reset).
- **Hold FIRE** for half a second to reset. A short tap does nothing, so a bump can never wipe your time.
- Shows minutes, seconds and hundredths.

### TORCH

The RGB LED turns into a torch (and the plain LED on GPIO 26 comes on with it).

- **Left/right:** colour. White, Red, Orange, Yellow, Green, Cyan, Blue, Purple, Pink, Rainbow.
- **Up/down:** brightness, 5 levels.
- **SELECT:** on or off. **FIRE:** next colour.

### TEMP

Three pages, switched with stick left/right. Readings are taken every 2 seconds in the background, **even while you are in other modes** (except games, reaction and music, which need steady timing).

1. **NOW:** big temperature and humidity, a rising / steady / falling trend arrow, "feels like" (heat index), dew point, session low/high, and a comfort word (COLD, COOL, COMFY, DRY, MUGGY, WARM, STICKY, HOT).
2. **TREND:** a labelled graph of the last 33 minutes (100 points, each a 20-second average). **FIRE** switches between temperature and humidity.
3. **STATS:** dew point, feels-like, average, temperature range, humidity range, and number of readings.

**SELECT** toggles °C / °F (remembered). **Hold FIRE** resets the history and statistics. If the sensor stops answering, a "NO SENSOR" screen tells you which wire to check.

Calibration offsets for temperature and humidity are in SETTINGS.

### TIMER

A pomodoro or plain countdown timer.

- **Setup screen:** stick up/down changes the minutes (hold to go faster), **FIRE** switches between **WORK** (1 to 99 min) and **BREAK** (0 to 30 min), **SELECT** starts. A break of 0 gives a plain one-shot countdown.
- **Running:** **SELECT** pauses and resumes, **FIRE** stops. A progress bar fills as time passes, and the last three seconds tick.
- LED is green during work, red during break, and blinks when paused or when time is up. Different little tunes play when work and break finish. Pomodoro keeps cycling until you stop it.

### LEVEL

A spirit level. A bubble moves in a vial as you tilt the board, with X / Y angle readouts. When the board is within 1 degree of level the bubble fills in, "FLAT" appears, it beeps and the LED goes green.

- **SELECT:** call the current pose "level" (zero here).
- **FIRE:** go back to the sensor's own zero.

### REACTION

Press FIRE to start, wait for the screen to flash white (the RGB LED flashes white too), then hit FIRE as fast as you can. Pressing too early shows "TOO EARLY!" with a buzz. The result gets a rating (LIGHTNING!, FAST, GOOD, OK, SLEEPY...). Your **best time is saved** in flash.

### MUSIC

Seven tabs. The **joystick click works as a SHIFT key**:

- **Tap the click** (press and release on its own): next tab.
- **Hold the click + stick left/right:** previous / next tab.
- **Hold the click + stick up/down, FIRE or SELECT:** the tab's extra options. The bottom line of the screen shows what they are while you hold it.
- **BACK:** back to the main menu. Everything stops and your tempo is saved.

The RGB LED flashes in time with the sound on every tab.

| Tab | Normal controls | With SHIFT held |
|---|---|---|
| **1. METRONOME** | Stick up/down: BPM 40 to 220 (hold to go faster). Left/right: beats per bar (2, 3, 4, 6). FIRE: start/stop. SELECT: **tap tempo** (tap the rhythm you want). | Up/down: click sound (CLICK, BEEP, TICK, LOW). FIRE: quarter, eighth or triplet ticks. SELECT: accent on beat 1 on/off. |
| **2. PIANO** | Six keys: stick **left, up, down, right**, plus **FIRE** and **SELECT**. | Up/down: octave. FIRE: scale (PENTA, MAJOR, MINOR, BLUES). SELECT: **looper** (record, then loop, then stop). |
| **3. THEREMIN** | **Tilt** left/right for pitch (three octaves), **hold FIRE** to sound it. Tilt forward/back for vibrato. SELECT snaps pitch to real notes. | Up/down: shift the three-octave range. |
| **4. DRUMS** | A 16-step grid with Kick / Snare / Hat rows. Stick moves the cursor, **FIRE** switches a step on/off (and plays it), **SELECT** plays/stops. | Up/down: BPM. FIRE: preset beat (ROCK, DISCO, HIPHOP, EMPTY). SELECT: clear the pattern. |
| **5. SONGS** | Left/right: pick a song. FIRE: play/pause. SELECT: restart from the top. | Up/down: tempo (40 to 360 BPM). FIRE: cycle **play once, LOOP, RAMP**. |
| **6. TUNER** | Left/right: one semitone. Up/down: one octave. **Hold FIRE** to sound the note, or **SELECT** to latch it on. | FIRE: next preset (A4 pitch pipe, ukulele G C E A, guitar E A D G B e). |
| **7. RETRO** | Left/right: pick an effect. FIRE: play it. SELECT: a random one. | - |

**Songs included:** TWINKLE, ODE JOY, BIRTHDAY, JINGLE, ELISE, KOROBEI (the Russian folk tune known from falling-blocks games), SPEEDRUN (an original fast finger-twister) and **MY SONG** (an empty slot for your own tune).

- **LOOP** repeats the song. **RAMP** repeats it about 10% faster every time round, up to 360 BPM.

**Retro effects:** COIN, JUMP, POWER UP, BONUS, LASER, BOOM, FANFARE, YOU LOSE.

**Notes on the sound:** the board has a single small buzzer, so everything is one note at a time. The drum machine fakes kick, snare and hat with a falling tone, a burst of random pitches and a short high tick. Very low notes are quiet on a small buzzer (the guitar E and A presets are shown an octave up, marked "+8va").

#### Adding your own song (MY SONG)

Find `SONG_MINE` in the sketch. Each entry is `{note, length}`:

- **note** is a MIDI number: C4 = 60, C#4 = 61, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71, C5 = 72, and so on up to 84. `0` is a rest.
- **length** is counted in sixteenth notes: 1 = sixteenth, 2 = eighth, 4 = quarter, 8 = half, 16 = whole.

```cpp
const SongNote SONG_MINE[] = {
  {60, 4}, {62, 4}, {64, 4}, {65, 4}, {67, 8}, {0, 4}, {67, 4}, {65, 4}, {64, 4}, {62, 4}, {60, 8}
};
```

The song's length is counted automatically. Use only tunes you have the right to use.

### TOYS

- **8-BALL:** think of a question, shake the board (or press SELECT / FIRE). 16 answers: yes, maybe or no. The LED glows green, amber or red.
- **DICE:** one to three dice. Stick up/down chooses how many, then roll. The total is shown.
- **COIN:** a flip animation with a running count of heads and tails.

Stick left/right switches toy.

---

## Settings

Stick up/down picks a row, left/right changes the value, FIRE / SELECT cycles it. **Click or BACK to leave** (your choices are saved on the way out).

| Setting | Values |
|---|---|
| **SOUND** | ON / OFF. OFF mutes everything (the music tab shows "MUTED"). |
| **LED BRIGHT** | 25%, 50%, 75%, 100%. Shows white while you adjust it. |
| **SCREENSAVER** | 15 sec, 30 sec, 1 min, 2 min, 5 min, NEVER. |
| **SHAKE LEVEL** | 1.5 g to 4.0 g in steps of 0.25. A live meter shows the current and peak g-force so you can pick a value. |
| **TEMP OFFSET** | -5.0 to +5.0 degrees C, to correct your sensor. Live preview of raw and corrected values. |
| **HUM OFFSET** | -15 to +15 percentage points. |
| **GESTURES** | ON / OFF. Controls face-down sleep, double tap and tilt-to-scroll. |
| **RESET ALL** | Press SELECT / FIRE. Restores every default and clears the saved best reaction time and tempo. |

---

## What is saved

Saved to the ESP32's flash (survives power-off), in the namespace `deskbuddy`:

| Key | What |
|---|---|
| `snd`, `led`, `ss`, `shk` | sound, LED brightness, screensaver time, shake level |
| `toff`, `hoff` | temperature and humidity offsets |
| `usef` | °C / °F |
| `gest` | gestures on/off |
| `rbest` | best reaction time |
| `bpm` | metronome / drum tempo |

**Not saved** (reset at every power-up): the DIZZY unlock, game high scores, the TEMP history and statistics, the selected song, drum patterns and piano loops.

At start-up the sketch also removes three leftover flash keys (`pf`, `pm`, `pe`) from the old pet feature that was retired in v10.

---

## Customising the sketch

All the switches are `#define`s and constants near the top of the file.

| Setting | Default | What it does |
|---|---|---|
| `RGB_PWM` | `1` | `1` = smooth colours and brightness. `0` = plain on/off pins (8 colours only). Use `0` if PWM ever misbehaves. |
| `RGB_COMMON_ANODE` | `0` | `0` common-cathode, `1` common-anode. |
| `RGB_GAIN_R / G / B` | 100 / 190 / 255 | Colour balance. Cheap LEDs have a very bright red die, so red is turned down. With the torch on WHITE: reddish, lower R; greenish, lower G; bluish, lower B. |
| `RGB_GAMMA` | `2.0` | Makes mixed colours (orange, pink, purple) look right. `1.0` turns it off. |
| `GAMEOVER_BLINK` | `0` | `1` makes the LED blink red on game over instead of staying solid. |
| `DEBUG_TILT` | `0` | `1` streams tilt and g-force to the Serial Monitor at 115200 baud. |
| `TILT_SIGN`, `TILT_USE_X` | `1.0`, `false` | If tilting right moves the ship left, set `TILT_SIGN` to `-1.0`. If your board's tilt axis is the other one, set `TILT_USE_X` to `true`. |
| `GAZE_FLIP_X / Y` | `false` | Flip the way the eyes look if they follow the tilt backwards. |
| `LEVEL_FLIP_X / Y` | `false` | Flip the spirit-level bubble direction. |
| `MENU_TILT_SIGN` | `1.0` | Set to `-1.0` if tilting forward scrolls the menu up. |
| `MENU_TILT_DEG` | `18` | How far to tip the board to scroll the menu. |
| `TAP_DELTA` | `0.35` | Sensitivity of the double tap (lower = easier to trigger). |
| `SPIN_RATE` | `4.5` | How fast you must spin the board to make the face dizzy. |

---

## Troubleshooting

| Problem | What to check |
|---|---|
| **Screen stays black** | Check SDA / SCL on GPIO 21 / 22 and 3V3 / GND. Some OLEDs use address `0x3D`; change `0x3C` in `display.begin(...)` in `setup()`. Run an I2C scanner sketch to see what is connected. |
| **Boot says "motion --" / LEVEL says "MPU6050 not found"** | MPU6050 wiring: SDA, SCL, VCC, GND. Tilt, gestures and theremin need it. The rest of the gadget still works without it. |
| **LED is lit during "LED test: OFF"** | Your LED is the other polarity: flip `RGB_COMMON_ANODE`. |
| **LED colours look wrong (too red, etc.)** | Adjust `RGB_GAIN_R / G / B` with the torch on WHITE. |
| **TEMP shows "NO SENSOR"** | DHT11 DATA must be on GPIO 15 with a pull-up (4.7 k to 10 k to 3V3 for a bare sensor). Check power and ground. The DHT11 is slow: it is read every 2 seconds. |
| **No sound / only clicks** | Make sure the buzzer is **passive**, wired to GPIO 27 and GND, and that SOUND is ON in SETTINGS. A flat constant tone means an active buzzer. |
| **Menu scrolls by itself or jumps** | Tilt-scroll is sensitive to how you hold the board. Raise `MENU_TILT_DEG`, or turn GESTURES off. |
| **Menu scrolls the wrong way when tilting** | Set `MENU_TILT_SIGN` to `-1.0`. |
| **Ship / eyes / bubble move the wrong way** | Flip `TILT_SIGN`, `GAZE_FLIP_X/Y` or `LEVEL_FLIP_X/Y`. |
| **Double tap triggers too easily / never** | Raise or lower `TAP_DELTA`. Or turn GESTURES off in SETTINGS. |
| **Shake does not register** | Lower SHAKE LEVEL in SETTINGS (use the live meter to see your peak). |
| **Stick drifts or does nothing** | The sketch treats the stick as pushed below about 1200 and above about 2900 of 4095. A worn joystick that rests off-centre may need a new module. Also check VRX / VRY are not swapped (see "Joystick orientation" above). |
| **Board will not start with a button held** | Do not hold BACK (GPIO 5) while resetting; it is a boot-strapping pin. |
| **Screen freezes on fast songs** | On purpose above about 250 BPM: the screen update is skipped to keep the notes steady. The sound keeps playing. |
| **Upload fails at "Connecting..."** | Hold the BOOT button on the ESP32 while uploading starts. Try another USB cable. |

---

## Version history

| Version | Highlights |
|---|---|
| v1 to v5 | Menu, animated robot face, dodge game, stopwatch, torch, temperature, buzzer, RGB LED, hidden DIZZY face. |
| v6 / v7 | Timer (pomodoro), spirit level, reaction tester, settings page with saved options. |
| v8 | TEMP rebuilt as a three-page instrument (now / graph / stats) with calibration. |
| v9 | Stopwatch hold-to-reset, tilt-following eyes, music tools, toys (8-ball, dice, coin), gesture controls. |
| v10 | Removed the pet. Games grouped in one **GAMES** menu. MUSIC expanded to six tabs with the joystick-click SHIFT scheme, drum machine, songs and tuner. Saved tempo. |
| **v11** | Three new songs (KOROBEI, SPEEDRUN, MY SONG slot), LOOP and RAMP modes, tempo up to 360 BPM, and a seventh **RETRO** tab with eight 8-bit sound effects. |

---

## Notes

- The sketch is written to be non-blocking (no long delays), so the buttons, sound and screen stay responsive.
- Built and syntax-checked on ESP32 Arduino core 2.x and 3.x with every combination of `RGB_PWM` and `RGB_COMMON_ANODE`. Direction constants (tilt, gaze, level, menu scroll) and the tap sensitivity depend on how your board is mounted, so expect to tune them once on real hardware.
- The built-in songs are public-domain tunes or originals. If you add your own to `MY SONG`, make sure you are allowed to use them.
