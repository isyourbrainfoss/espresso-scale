# Half Decent Espresso Scale

Breadboard espresso scale firmware for **ESP32-S3 Super Mini** + HX711 + SSD1306.

Built to work with **[Flowlog](https://github.com/isyourbrainfoss/Flowlog)** via the public **Decent Scale BLE** protocol (device name `Decent Scale`).

## Features (v1.1)

- Weight to 0.1 g (HX711 + 2 kg cell)
- Tare + shot timer + flow rate (g/s) on OLED
- Passive buzzer feedback
- Calibration stored in NVS
- BLE: Decent Scale API (FFF4 notify, 36F5 write, 10 Hz weight, heartbeat)
- **Wi‑Fi** with setup AP + status page + **OTA** (ArduinoOTA + browser upload)

## Hardware

See [HARDWARE.md](HARDWARE.md) for the pin table matching this breadboard build.

| Function | GPIO |
|----------|------|
| OLED SDA / SCL | 8 / 9 |
| HX711 DT / SCK | 6 / 7 |
| Tare / Timer buttons | 4 / 5 |
| Buzzer | 10 |

## Build & flash

Requires [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html).

```bash
export PATH="$PATH:$HOME/.platformio/penv/bin"
cd espresso-scale
pio run -t upload
pio device monitor -b 115200
```

ESP32-S3 Super Mini: hold **BOOT** if upload fails, then reset after flash. USB serial uses CDC (`ARDUINO_USB_CDC_ON_BOOT`).

## Wi‑Fi & wireless OTA

After the first USB flash, the scale can join your LAN and accept updates without a cable.

### First-time Wi‑Fi setup

If no credentials are stored (or join fails), the scale starts a setup access point:

| | |
|--|--|
| SSID | `HalfDecent-Setup` |
| Password | `scale1234` |
| Portal | http://192.168.4.1/ (or `/wifi`) |

1. Join `HalfDecent-Setup` from your phone/laptop.
2. Open the portal and enter your home Wi‑Fi SSID + password.
3. The scale reboots, joins STA, and serves **http://half-decent.local/** (or its DHCP IP).

**Serial alternative** (USB monitor):

```text
wifi set MySSID MyPassword
wifi              # status / IP
wifi clear        # wipe creds → setup AP on reboot
wifi ap           # force setup AP now
```

### Status page

On the LAN: **http://half-decent.local/** (or `http://<ip>/`)

- Live weight snapshot  
- Links to **Wi‑Fi setup** and **OTA update**  

OLED shows a small `WiFi` or `AP` label when wireless is up.

### OTA update (PlatformIO)

Password is `scaleota` (change in `include/config.h` → `kOtaPassword`).

```bash
# Discover IP from serial ("wifi") or the status page, then:
pio run -t upload -e ota --upload-port 192.168.1.50

# If mDNS works on your OS:
pio run -t upload -e ota --upload-port half-decent.local
```

### OTA update (browser)

1. Build: `pio run`
2. Open **http://half-decent.local/update**
3. Upload `.pio/build/esp32-s3-super-mini/firmware.bin`
4. Wait for reboot

During OTA the OLED shows `OTA...`. Prefer not to pour a shot mid-update.

## Buttons

| Action | Control |
|--------|---------|
| Tare | Tare button short press |
| Start/stop timer | Timer button short press |
| Reset timer | Timer button long press (~1 s) |
| Calibration mode | Hold **both** ~3 s |

Optional auto-start: timer starts when weight rises past 0.5 g after a tare (see `include/config.h`).

## Calibration (serial)

Open the serial monitor at 115200 baud:

```
help
tare
cal empty          # empty platform, zero
# place a known mass (e.g. 100 g)
cal 100            # compute & save factor
weight
factor             # print stored factor
```

Or: both buttons → tare empty is captured → place mass → press Tare to finish with default 100 g.

## Flowlog / BLE

1. Flash the scale and power it (USB).
2. In [Flowlog](https://github.com/isyourbrainfoss/Flowlog), pair a **Decent Scale**.
3. The board advertises as **`Decent Scale`**.
4. Flowlog connects → subscribes to FFF4 → sends LED-on → streams weight at 10 Hz.
5. Heartbeat (`03 0a 03 ff ff 00 0a`) every 5 s is enforced when the app opts in (Flowlog does this).

Protocol notes used by Flowlog:  
https://github.com/isyourbrainfoss/Flowlog/blob/main/docs/protocols/decent-scale-ble.md

### BLE UUIDs

| Role | UUID |
|------|------|
| Notify | `0000fff4-0000-1000-8000-00805f9b34fb` |
| Write | `000036f5-0000-1000-8000-00805f9b34fb` |

Weight is a 10-byte v1.2 packet; grams = signed big-endian int16 at bytes 2–3, divided by 10.

## Project layout

```
include/pins.h config.h
src/main.cpp scale.* display.* buttons.* buzzer.* shot_timer.* ble_decent.* wifi_ota.*
platformio.ini   # envs: esp32-s3-super-mini (USB), ota (espota)
```

## License

MIT — see [LICENSE](LICENSE).
