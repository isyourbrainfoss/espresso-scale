# Hardware — Half Decent Espresso Scale (breadboard)

## Components

| Part | Notes |
|------|--------|
| ESP32-S3 Super Mini | Main MCU, USB-C power/program, BLE |
| 0.96" SSD1306 OLED | I2C, 128×64 |
| HX711 | Load cell amplifier |
| 2 kg load cell | E+/E−/A+/A− → HX711 (R/B/G/W typical) |
| 2× TTP223 | Capacitive touch, active-high OUT |
| Passive buzzer | Driven with PWM on GPIO 10 |
| Breadboard + 22 AWG | Shared 3.3V / 5V / GND rails |

Power: USB-C (or USB-A cable) to Super Mini.

## Pin map

| Component | Signal | ESP32-S3 GPIO |
|-----------|--------|---------------|
| OLED | SDA | 8 |
| OLED | SCL | 9 |
| HX711 | DT / DO | 6 |
| HX711 | SCK | 7 |
| Touch 1 (Tare) | SIG | 4 |
| Touch 2 (Timer) | SIG | 5 |
| Buzzer | + | 10 |

Common: 3.3V (or 5V for HX711 VCC if your module expects it — many HX711 boards take 5V VCC with 3.3V-safe DT/SCK; match your module datasheet) and GND on the breadboard rails.

## Load cell wiring (typical)

| Load cell | HX711 |
|-----------|-------|
| Red (E+) | E+ |
| Black (E−) | E− |
| Green (A+) | A+ |
| White (A−) | A− |

If readings invert, swap A+/A− or negate the calibration factor in software.

## Notes

- Keep HX711 digital lines short; noise shows up as weight flicker.
- Hot cups can thermally drift a 2 kg cell; a thin silicone pad helps.
- TTP223 modules often need VCC=3.3V on ESP32 breadboards.
