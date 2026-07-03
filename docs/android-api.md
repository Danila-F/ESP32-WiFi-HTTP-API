# Android integration API

The firmware exposes a small JSON HTTP API intended for a native Android app and Android Device Controls.

## Discovery

When connected to Wi-Fi, the device publishes an mDNS service:

- Service: `_espctrl._tcp.local`
- Port: `80`
- TXT records: `id`, `name`, `api`, `fw`, `type`

The Android app can discover the device with `NsdManager` and then call `GET /api/info`.

## Authentication

Protected endpoints require one of these headers:

```http
X-Device-Token: <token>
```

or:

```http
Authorization: Bearer <token>
```

The token is generated on first boot. During first provisioning, `/api/provision` returns it to the Android app. It is also printed to the serial monitor for development.

## Endpoints

### GET `/api/info`

Public endpoint for discovery and pairing UI.

### POST `/api/provision`

Initial provisioning endpoint. Before the device is provisioned, it does not require a token. After provisioning, it requires the current token.

Request:

```json
{
  "ssid": "Home WiFi",
  "password": "wifi-password",
  "name": "Desk lamp",
  "room": "Bedroom",
  "authToken": "optional-app-generated-token"
}
```

Response contains `deviceId`, `name`, `authToken` and `restartRequired`.

### GET `/api/state`

Returns current device state for the Android app and Device Controls refresh.

### POST `/api/command`

Supported commands:

```json
{ "command": "set_power", "value": true }
```

```json
{ "command": "toggle_power" }
```

```json
{ "command": "reboot" }
```

### GET `/api/actions`

Returns action metadata that the Android app can map to `ToggleTemplate` or `StatelessTemplate` controls.

### GET/POST `/api/config`

Reads or updates display and GPIO settings.

### POST `/api/factory-reset`

Clears provisioning data and restarts the device.

## Suggested Android Device Controls mapping

- `set_power` -> `ToggleTemplate` + `BooleanAction`
- `toggle_power` -> `StatelessTemplate` + `CommandAction`
- `reboot` should normally stay inside the app UI, not in the system Device Controls tile.
