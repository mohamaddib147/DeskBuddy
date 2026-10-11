# DeskBuddy — ESP32 OLED Pocket Gadget

A joystick-and-button-controlled desk gadget built on an ESP32 with a 128×64 SSD1306 OLED display. One scrollable menu, six modes: an animated robot-face "emo" screen, a dodge-the-blocks mini game, a space-shooter mini game, a stopwatch, a torch (LED flashlight), and a live temperature/humidity readout — all with buzzer feedback on every action.

## Features

- **Scrollable menu** — joystick navigation with an inverted-highlight selected row
- **EMO MODE** — five animated robot-eye expressions (Happy, Sad, Angry, Surprised, Wink), cycled with the joystick
- **GAME** — dodge-the-falling-blocks: move a paddle to avoid falling squares, score rises as you dodge, difficulty ramps up, session high score
- **SHOOTER** — a small Space-Invaders-style shooter: move a ship, shoot descending enemies before they reach you, 3 lives, its own session high score
- **STOPWATCH** — start/pause/reset entirely from the joystick, no extra buttons
- **TORCH** — turns the onboard LED into a simple flashlight
- **TEMP** — live temperature and humidity from a DHT11 sensor
- **Audio feedback** — every scroll, select, start/pause/reset, shot, score, hit, and collision has its own tone through a passive piezo buzzer

## Hardware

| Component | Notes |
|---|---|
| ESP32 dev board | Any standard ESP32 (WROOM-32 etc.) |
| 0.96" SSD1306 OLED, 128×64, I2C | White panel |
| Analog joystick module | 2-axis + push button |
| DHT11 temperature/humidity sensor | |
| LED | Used as the torch |
| Passive piezo buzzer | Driven with `tone()`/`noTone()` |

### Wiring

| Signal | ESP32 Pin |
|---|---|
| OLED SDA | GPIO 21 |
| OLED SCL | GPIO 22 |
| Joystick VRX | GPIO 34 |
| Joystick VRY | GPIO 35 |
| Joystick SW (button) | GPIO 32 |
| DHT11 data | GPIO 15 |
| LED (torch) | GPIO 26 |
| Buzzer | GPIO 27 |

> **Note on display color:** this project uses a standard white-panel SSD1306. The code refers to pixels as `SSD1306_WHITE`, which is just the library's naming for "pixel on" — it renders in whatever color the physical panel is. To change the visible color, swap in a different-colored SSD1306 module; no code changes are needed.
>
> **Note on joystick axis naming:** on this joystick module, `VRX_PIN` reads the physical up/down motion and `VRY_PIN` reads the physical left/right motion (reversed from what the pin names suggest). The code accounts for this throughout — if you use a different joystick module, you may need to swap `VRX_PIN`/`VRY_PIN` in the relevant `analogRead()` calls.

## Libraries

Install via the Arduino IDE Library Manager:

- `Adafruit GFX Library`
- `Adafruit SSD1306`
- `DHT sensor library` (Adafruit)

## Controls

| Context | Joystick | Button |
|---|---|---|
| Menu | Up/Down — scroll | Select / enter mode |
| EMO MODE | Left/Right — cycle face | Back to menu |
| GAME (dodge) | Left/Right — move paddle | Back to menu |
| SHOOTER | Left/Right — move ship, Up — fire | Back to menu |
| STOPWATCH | Push one way — start/pause, other way — reset | Back to menu |
| TORCH / TEMP | — | Back to menu |

The button always returns to the menu, from any mode and at any point (including from a GAME OVER screen) — there's no separate "restart" control; just re-enter the mode from the menu to start a fresh run.

## High Scores

GAME and SHOOTER each track their own high score for the current power session (`highScore` and `shooterHighScore` in the code). Both reset to zero on reboot or re-upload — they live in RAM only, not flash. Persisting them across power cycles via the ESP32's `Preferences`/NVS library is a natural next step (see Roadmap).

## Getting Started

1. Wire the components per the table above.
2. Install the required libraries.
3. Open `DeskBuddy.ino` in the Arduino IDE.
4. Select your ESP32 board under **Tools → Board**, pick the correct port, and upload.

## Roadmap

- [ ] Persist high scores across power cycles (ESP32 `Preferences`/NVS)
- [ ] GAME placeholder — none currently; all six menu slots are implemented

## License

MIT — do whatever you like with it.
