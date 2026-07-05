# ESP32-WiFi-HTTP-API

PlatformIO firmware for ESP32 devices controlled from a local Android app over Wi-Fi.

The firmware provides:

- local HTTP JSON API for discovery, provisioning, state and commands;
- mDNS discovery for Android `NsdManager`;
- persistent device configuration in ESP32 NVS;
- setup Access Point mode for first Wi-Fi provisioning;
- token-protected commands;
- Wi-Fi OTA firmware upload through the same API;
- GPIO-backed example power control suitable for Android Device Controls.

## Build

```bash
pio run
```

## Flash over USB

```bash
pio run -t upload
pio device monitor
```

The first boot prints the generated `deviceId` and `authToken` to the serial monitor.

## First provisioning flow

If Wi-Fi is not configured, the ESP32 starts a setup access point:

```text
SSID: ESP32-Setup-xxxxxx
Password: esp32setup
Default IP: 192.168.4.1
```

The Android app should connect to this AP and send:

```http
POST http://192.168.4.1/api/provision
Content-Type: application/json
```

```json
{
  "ssid": "Home WiFi",
  "password": "wifi-password",
  "name": "Desk lamp",
  "room": "Bedroom"
}
```

The response returns the generated `authToken`. The Android app should store this token together with the device ID and discovered address.

## Discovery after provisioning

After reboot and connection to the home Wi-Fi network, the firmware publishes:

```text
_espctrl._tcp.local
```

The Android app can discover it with Android `NsdManager` and then call:

```http
GET /api/info
GET /api/state
```

Protected endpoints require:

```http
X-Device-Token: <saved-token>
```

or:

```http
Authorization: Bearer <saved-token>
```

## Command API

Set power state:

```http
POST /api/command
X-Device-Token: <saved-token>
Content-Type: application/json
```

```json
{ "command": "set_power", "value": true }
```

Toggle power state:

```json
{ "command": "toggle_power" }
```

By default, power is mapped to GPIO 2. This is configurable with `POST /api/config`.

## OTA update from Android app

Build a new binary:

```bash
pio run
```

Upload `.pio/build/esp32dev/firmware.bin` from the Android app as multipart form data:

```http
POST /api/ota
X-Device-Token: <saved-token>
Content-Type: multipart/form-data
```

Form field name:

```text
firmware
```

See [docs/ota.md](docs/ota.md) for details.

## Android Device Controls mapping

The firmware side is intentionally generic. The Android app should map the HTTP API to Android Device Controls like this:

- `GET /api/info` → pairing/discovery metadata;
- `GET /api/state` → current `Control` state;
- `POST /api/command` with `set_power` → `ToggleTemplate` + `BooleanAction`;
- `POST /api/command` with `toggle_power` → `StatelessTemplate` + `CommandAction`;
- `POST /api/ota` → in-app firmware update screen.
