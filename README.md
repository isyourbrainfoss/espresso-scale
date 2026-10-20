# Half Decent Espresso Scale

> **Note**
> This is very experimental. Assume nothing here is written, or necessarily read through or fully understood, by a human unless stated, and that includes this text. Use it at your own risk, but you're encouraged to reuse any parts you find useful. The human work here is mainly ideas, testing in reality, and persistence with a vision.

Breadboard espresso scale firmware for **ESP32-S3 Super Mini** + HX711 + SSD1306.

Built to work with **[Flowlog](https://github.com/isyourbrainfoss/Flowlog)** via the public **Decent Scale BLE** protocol (device name `Decent Scale`).

## Features (v1.2)

- Weight to 0.1 g (HX711 + 2 kg cell) with **auto-tare on boot**
- Tare + shot timer + flow rate (g/s) on OLED
- Softer multi-note buzzer cues
- **Deep sleep**: long-press Tare → radios/OLED/HX711 off; **touch Tare or Timer** to wake (full reboot)
- Battery / **USB** indicator on OLED (ADC optional for real %)
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

### Device password

There are **no default passwords**. On first boot the scale generates a random
12-character **device password** and stores it in flash (NVS). It is used for:

- the setup access point (WPA2 key),
- the web UI (HTTP Digest login, user **`scale`**),
- PlatformIO / ArduinoOTA uploads.

To see it, **hold Timer for ~5 s** (keep holding past the ~1 s timer reset):
the OLED shows the password, web user and IP for 60 s. Over USB serial,
`wifi pass` shows it on the OLED too (it is only printed to serial if the OLED
is not working). Logs only ever show a masked form such as `ab********yz`.
`wifi pass reset` (USB serial) generates a new one and reboots.

### First-time Wi‑Fi setup

On first boot (or after `wifi clear`), with no saved network, the scale opens a
setup access point and shows its details on the OLED:

| | |
|--|--|
| SSID | `HalfDecent-XXXX` (last 4 hex digits of the MAC) |
| Password | the device password (on the OLED) |
| Portal | http://192.168.4.1/ (or `/wifi`), login `scale` + device password |

1. Join the AP from your phone/laptop.
2. Open the portal, log in, and enter your home Wi‑Fi SSID + password
   (SSID up to 32 bytes; password 8–63 characters, or empty for an open network).
3. The scale reboots, joins STA, and serves **http://half-decent.local/** (or its DHCP IP).

The setup AP closes after 10 minutes with nobody connected. It does **not**
open automatically when a saved network is unreachable; the scale keeps
retrying that network instead. To open it again, **hold Timer ~5 s** while not
connected to Wi‑Fi, or use `wifi ap` on serial.

**Serial alternative** (USB monitor):

```text
wifi set MySSID MyPassword
wifi              # status / IP
wifi clear        # wipe creds → setup AP on reboot
wifi ap           # open setup AP now
wifi pass         # show device password on the OLED
```

### Status page

On the LAN: **http://half-decent.local/** (or `http://<ip>/`), login `scale` + device password.

- Live weight snapshot  
- Links to **Wi‑Fi setup** and **OTA update**  

All pages need the login. Form submissions (Wi‑Fi settings, firmware upload)
also need a one-time token from the form and a same-origin `Origin`/`Referer`
header, so other websites cannot submit them through your browser.

OLED shows a small `WiFi` or `AP` label when wireless is up.

> The web UI is plain HTTP on your LAN. Digest auth keeps the password off the
> wire, but traffic is not encrypted. Firmware images are not signed yet (see
> below).

### OTA update (PlatformIO)

Use the device password via an environment variable (nothing is stored in the repo):

```bash
# Discover IP from serial ("wifi") or the status page, then:
SCALE_OTA_PASSWORD=xxxxxxxxxxxx pio run -t upload -e ota --upload-port 192.168.1.50

# If mDNS works on your OS:
SCALE_OTA_PASSWORD=xxxxxxxxxxxx pio run -t upload -e ota --upload-port half-decent.local
```

### OTA update (browser)

1. Build: `pio run`
2. Open **http://half-decent.local/update** and log in
3. Upload `.pio/build/esp32-s3-super-mini/firmware.bin`
4. Wait for reboot

### Firmware signing (not yet)

OTA images are authenticated only by the device password, not by a signature.
Signed updates (`Update.installSignature()` with an RSA/ECDSA public key) exist
only in arduino-esp32 **3.3.8+** (earlier 3.x releases had a verification
bypass). This project builds on PlatformIO's `espressif32` platform, which
ships arduino-esp32 2.0.17 without that API, so signing would mean moving to
the pioarduino platform / core 3.x first. That is a follow-up.

During OTA the OLED shows `OTA...`. Prefer not to pour a shot mid-update.

## Buttons

| Action | Control |
|--------|---------|
| Tare | Tare short press |
| **Deep sleep** | **Tare long press** (~1 s) — OLED shows Sleep… then powers down |
| **Wake** | Touch **Tare** or **Timer** (full cold start + auto-tare + Wi‑Fi) |
| Start/stop timer | Timer short press |
| Reset timer | Timer long press (~1 s) |
| **Wi‑Fi password / setup AP** | Keep holding Timer to **~5 s**: shows device password (on Wi‑Fi) or opens the setup AP |
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
