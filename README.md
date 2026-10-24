# Flowlog Scale (espresso-scale)

> **Note**
> This is very experimental. Assume nothing here is written, or necessarily read through or fully understood, by a human unless stated, and that includes this text. Use it at your own risk, but you're encouraged to reuse any parts you find useful. The human work here is mainly ideas, testing in reality, and persistence with a vision.

Breadboard espresso scale firmware for **ESP32-S3 Super Mini** + HX711 + SSD1306.

Built as the **companion scale for [Flowlog](https://github.com/isyourbrainfoss/Flowlog)** using the public **Decent Scale BLE** protocol (device still advertises as `Decent Scale` so pairing stays unchanged).

## Features (v1.7.0)

v1.7.0 shows live grams about a second after power-on or wake. The HX711
zeroes in the background while the weight screen is already up; Wi‑Fi (one
directed scan, then a direct join) and Bluetooth start afterwards without
blocking the weighing loop (v1.6.3 needed ~12 s before the first weight). The
boot tare now averages one fresh sample window (~0.6 s at 10 SPS instead of two
back-to-back library tares), and a cup left on the platform through power-on
is no longer zeroed away (see *Zero point* below). Web UI, setup AP and OTA are
protected by a per-device password (digest auth + CSRF).

v1.6.3 joins the saved home network only when that SSID is actually in range.
Away from home the Wi‑Fi radio stays off (Bluetooth only) instead of hunting
for 25 seconds and then starting the setup hotspot. v1.6.2 kept grams on the
OLED during an app brew and moved BLE commands onto the Arduino loop.

- Weight to 0.1 g (HX711 + 2 kg cell) with a fast **auto-tare on boot**
- **OLED**: weight + **pressure (bar)**, cup fill bar (target 36 g / warn 32 g), pressure bar 0–12 bar
- Flow rate (g/s)
- **Phone-free brew**: **long-press Timer** → OLED “Start brew?” → **Timer** to confirm (tare + PRS + record) or **Tare** to cancel. Short Timer is kitchen timer only; Tare short always zeros for dosing beans
- **App brew pressure forward**: Flowlog writes type `0xF0` so the scale shows bar while the phone keeps PRS
- **Shot export**: last **3** phone-free shots on SPIFFS; `GET /shots.json`, `/shot.json` (newest), `/shot/0|1|2.json`; Flowlog History → Import (Wi‑Fi IP or **Bluetooth**); optional Nextcloud WebDAV push
- BLE weight stream whenever a phone is connected (not only after LED-on)
- Softer multi-note buzzer cues
- **Deep sleep**: long-press Tare → radios/OLED/HX711 off; **touch Tare or Timer** to wake
- Battery / **USB** indicator; calibration in NVS
- BLE: Decent Scale API (FFF4 notify, 36F5 write, 10 Hz weight, heartbeat) + Flowlog `0xF0–F2`
- **Wi‑Fi** setup AP + status page + **OTA**

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

### Device password (no shared default)

There are **no compiled-in Wi‑Fi or OTA passwords**. On first boot each scale
generates its own random password (12 symbols from the hardware RNG, e.g.
`k7mq-29xh-tp4c`, ≈59 bits) and stores it in NVS. The same password is used for:

| Where | Login |
|--|--|
| Setup access point `HalfDecent-XXXX` (WPA2) | the device password |
| Web UI (`/`, `/wifi`, `/nextcloud`, `/update`) — HTTP Digest auth | user `admin` + device password |
| ArduinoOTA / `pio run -e ota` | `HALF_DECENT_OTA_PASSWORD` = device password |

The shot export endpoints (`/shot.json`, `/shots.json`, `/shot/N.json`) stay
open and read-only so Flowlog's *Import from scale* keeps working without a login.

`XXXX` is the last 4 hex digits of the board's MAC, so each scale has its own SSID.

**How to see the password**

- **First boot** (or after `auth reset`): it is shown on the OLED for 60 s and
  printed **once** to the USB serial log in a `NEW DEVICE PASSWORD` banner.
  This is the only time it ever goes to serial.
- **Any time:** hold **Timer** for **5 s** (the timer resets at 1 s; keep holding).
  The OLED shows the password (plus IP / setup-AP SSID) for 60 s.
- **USB serial:** `auth show` puts it on the OLED (it is not echoed to serial).

**How to reset it**

- USB serial: `auth reset` → erases the stored password and reboots; a new
  one is generated and displayed as on first boot.
- Full wipe (password, Wi‑Fi, calibration): `pio run -t erase` then re-flash.

> Upgrading from v1.3 or earlier: the old shared `scaleota` / `scale1234`
> passwords stop working. After the update the scale boots with a fresh
> device password on the OLED — note it for your next OTA upload.

### When it joins Wi‑Fi

On a normal boot the scale scans briefly for the saved SSID.

- **Home network in range:** it joins and serves **http://half-decent.local/** (or its DHCP IP).
- **Not in range, or no credentials saved:** Wi‑Fi stays **off**. Bluetooth keeps working. It does not start a hotspot and it does not keep reconnecting.
- **You walk out of range while it is on:** after about 12 seconds it turns Wi‑Fi off until the next boot.

### Opening the setup hotspot

The hotspot does not come on by itself. It opens only when:

- you **hold Timer for the first ~1.2 s after a cold power-on** (not a deep-sleep wake), or
- you **hold Timer 5 s** at runtime while the scale is not on a LAN (if it *is* on a
  LAN, the first 5 s hold just shows the password; hold 5 s again while it is shown to
  force the setup AP), or
- you type `wifi ap` on USB serial.

It closes itself after **10 minutes** with no phone joined and no web request.

1. Open the hotspot (above). The OLED shows `SSID HalfDecent-XXXX`, the device password and `http://192.168.4.1`.
2. Join that network from your phone/laptop with the device password.
3. Open http://192.168.4.1/ — log in as `admin` with the same password.
4. **Wi‑Fi setup** → enter SSID (≤32 bytes) and password (8–63 chars, or empty for an open network).
5. The scale reboots and joins that network if it is in range.

Waking from sleep with Timer does **not** open the hotspot. The pad is still touched from the wake.

**Serial alternative** (USB monitor):

```text
wifi set MySSID MyPassword
wifi set "SSID with spaces" MyPassword
wifi              # status / IP
wifi ap           # force setup AP now
wifi clear        # forget home Wi‑Fi (radio stays off after reboot)
wifi connect      # retry saved network now
auth show         # device password on the OLED
auth reset        # new device password (reboots)
```

### Zero point

The boot tare compares the platform with the stored **empty zero** (raw
reading of the empty platform, kept in NVS and refreshed by every boot or tare
on a nearly empty platform, and by `cal empty`):

- within +40 g of it (or lighter): normal boot tare — absorbs drift;
- 40 g or more heavier (cup left on the scale): keeps the stored empty zero, so
  the cup shows its weight and lifting it reads 0 g instead of −cup.

A tare by button, app or web always zeroes whatever is on the platform —
lifting a tared cup reads negative, as on any scale. `zero clear` (serial)
forgets the reference. The status page shows the boot timing and the zero
decision.

### Status page

On the LAN: **http://half-decent.local/** (or `http://<ip>/`), user `admin` + device password.

- Live weight snapshot + **Tare** button, boot timing / zero decision  
- **Last shot** link → `/shot.json` (Flowlog-compatible JSON)  
- Links to **Wi‑Fi setup**, **Nextcloud** (optional auto-push), and **OTA update**  

OLED shows a small `WiFi` or `AP` label when wireless is up.

### Buttons

| Button | Short press | Long press |
|--------|-------------|------------|
| **Tare** (O) | Zero scale (safe while weighing beans). Cancels brew prompt. | Deep sleep |
| **Timer** (□) | Kitchen timer start/stop. During brew: stop & save. On prompt: confirm. | **Start brew?** prompt |

### Phone-free brew

1. No phone needed (or phone not using the scale).
2. **Long-press Timer** → OLED shows weight + “Start brew? / Timer=OK Tare=cancel”.
3. **Timer** again → tare cup, scan `PRS*`, start recording. **Tare** cancels (does not zero).
4. **Timer** during brew → save `/last_shot.json`, optional Nextcloud PUT.
5. Flowlog **History → Import from scale** (`http://half-decent.local/shot.json`).

Serial: `brew start` / `brew stop` / `prs` / `shot` / `nc push`.

### OTA update (PlatformIO)

```bash
export HALF_DECENT_OTA_PASSWORD='xxxx-xxxx-xxxx'   # from the OLED
pio run -t upload -e ota --upload-port 192.168.1.50
# If mDNS works on your OS:
pio run -t upload -e ota --upload-port half-decent.local
```

Don't commit the password; `platformio.ini` reads it from the environment.

### OTA update (curl)

```bash
F=~/.config/espresso-scale/ota-password; IP=192.168.50.3
cfg() { printf 'user = "admin:%s"\n' "$(cat "$F")"; }   # keeps the password off argv
TOKEN=$(curl -s --digest -K <(cfg) "http://$IP/update" | grep -o 'csrf=[0-9a-f]*' | head -1 | cut -d= -f2)
curl -s --digest -K <(cfg) -o /dev/null "http://$IP/update" \
  --next --digest -K <(cfg) -H "Origin: http://$IP" -w 'HTTP %{http_code}\n' \
  -F "firmware=@.pio/build/esp32-s3-super-mini/firmware.bin" "http://$IP/update?csrf=$TOKEN"
```

A 1.3 MB image takes about 2 minutes over the web upload; `HTTP 200` means it
was accepted and the scale reboots. ArduinoOTA (`pio run -t upload -e ota`)
is faster and uses the same password.

### Provisioning a known password without OLED / serial

If you can't read the OLED or USB serial (e.g. first update to the
authenticated firmware over the old open `/update`), create the untracked
header `include/device_password_provision.h`:

```c
#pragma once
static constexpr const char* kProvisionDevicePassword = "your-password-8-63-chars";
```

Build and flash once; on boot it overwrites the stored device password. Then
delete the header, rebuild and OTA the normal image with that password — it
keeps the stored password. The header is in `.gitignore`.

### OTA update (browser)

1. Build: `pio run`
2. Open **http://half-decent.local/update** (log in as `admin`)
3. Upload `.pio/build/esp32-s3-super-mini/firmware.bin`
4. Wait for reboot

During OTA the OLED shows `OTA`. Prefer not to pour a shot mid-update.

### Wi‑Fi security notes

- Web UI: HTTP **Digest** auth (Basic is refused). The digest `uri` must match
  the requested path. 5 bad logins → 60 s lockout (HTTP 429).
- Every POST (`/wifi`, `/nextcloud`, `/tare`, `/update`) needs a per-session random **CSRF token**
  (embedded in the form; regenerated each time the web server starts) **and**
  an `Origin` / `Referer` matching the device; the `Host` header must be the
  device IP or `half-decent.local` (blocks DNS rebinding). Rejected uploads
  never touch flash.
- Responses send `Cache-Control: no-store`, `X-Frame-Options: DENY` and a strict CSP.
- Limits: it's still plain HTTP. On your home LAN someone who can sniff traffic
  could see a CSRF token and replay a digest response while the nonce is valid
  (≤30 min). The setup AP is WPA2-protected with the device password.

### Signed OTA images (not implemented yet — follow-up)

Firmware images are checked only for the ESP image header and the esptool
SHA-256 digest (integrity, not authenticity). Anyone who knows the device
password can flash any image. Planned follow-up:

- Sign `firmware.bin` in a PlatformIO post-build script (ECDSA P-256 over the
  image SHA-256, signature appended), verify with mbedTLS in the web `/update`
  handler before `Update.end()`, and route ArduinoOTA through the same check
  (ArduinoOTA in Arduino-ESP32 2.x calls `Update` directly, so it needs its
  own verifying receiver or must be disabled), **or**
- Move to ESP-IDF Secure Boot v2 + signed app images (needs a custom
  bootloader and eFuse burning — irreversible, so not done blindly).

It was left out because it can't be tested without hardware and a mistake
breaks the only wireless recovery path.

## Buttons

| Action | Control |
|--------|---------|
| Tare | Tare short press |
| **Deep sleep** | **Tare long press** (~1 s) — OLED shows Sleep… then powers down |
| **Wake** | Touch **Tare** or **Timer** (full cold start + auto-tare + Wi‑Fi; the waking touch is ignored) |
| Start/stop timer | Timer short press |
| Reset timer | Timer long press (~1 s) |
| **Show device password / open setup AP** | Keep holding **Timer** to ~5 s (see Wi‑Fi section) |
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
src/main.cpp scale.* display.* buttons.* buzzer.* shot_timer.* ble_decent.* wifi_ota.* device_auth.*
platformio.ini   # envs: esp32-s3-super-mini (USB), ota (espota)
```

## License

MIT — see [LICENSE](LICENSE).
