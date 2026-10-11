# DeskBuddy — ESP32 OLED Pocket Gadget

A joystick-and-button-controlled desk gadget built on an ESP32 with a 128×64 SSD1306 OLED display. One scrollable menu, five modes: an animated robot-face "emo" screen, a dodge-the-blocks mini game, a stopwatch, a torch (LED flashlight), and a live temperature/humidity readout — all with buzzer feedback on every action.

## Features

- **Scrollable menu** — joystick navigation with an inverted-highlight selected row
- **EMO MODE** — five animated robot-eye expressions (Happy, Sad, Angry, Surprised, Wink), cycled with the joystick
- **GAME** — a dodge-the-falling-blocks game with a live score, ramping difficulty, and a persistent (session) high score
- **STOPWATCH** — start/pause/reset entirely from the joystick, no extra buttons
- **TORCH** — turns the onboard LED into a simple flashlight
- **TEMP** — live temperature and humidity from a DHT11 sensor
- **Audio feedback** — every scroll, select, start/pause/reset, score, and collision has its own tone through a passive piezo buzzer

## Hardware

| Component | Notes |
|---|---|
| ESP32 dev board | Any standard ESP32 (WROOM-32 etc.) |
| 0.96" SSD1306 OLED, 128×64, I2C | White panel (see note below) |
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

> **Note on display color:** this project uses a standard white-panel SSD1306. The code refers to pixels as `SSD1306_WHITE`, which is just the library's naming for "pixel on" — it renders in whatever color the physical panel is (white, blue, or yellow/blue two-tone). To change the visible color, swap in a different-colored SSD1306 module; no code changes are needed.

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
| GAME | Left/Right — move paddle | Back to menu |
| STOPWATCH | Push one way — start/pause, other way — reset | Back to menu |
| TORCH / TEMP | — | Back to menu |

> The joystick axis mapping (which pin drives which physical direction) can vary by module — if up/down and left/right feel swapped on your hardware, swap `VRX_PIN`/`VRY_PIN` in the relevant read calls.

## Getting Started

1. Wire the components per the table above.
2. Install the required libraries.
3. Open `DeskBuddy.ino` in the Arduino IDE.
4. Select your ESP32 board under **Tools → Board**, pick the correct port, and upload.

## Roadmap

- [ ] Space-invaders-style shooter mode
- [ ] Persist high score across power cycles (ESP32 `Preferences`/NVS)
- [ ] GAME placeholder mode (menu slot currently unused)

## License

MIT — do whatever you like with it.
