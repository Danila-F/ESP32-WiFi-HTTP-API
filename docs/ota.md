# OTA update over Wi-Fi

The firmware supports upload of a new PlatformIO/Arduino binary through the same local HTTP API that the Android app uses for device control.

## Partition table

The project uses `partitions_ota.csv` with two OTA app slots:

- `app0` / `ota_0`
- `app1` / `ota_1`

This is required because the new firmware image must be written to the inactive slot before reboot.

## Endpoint

```http
POST /api/ota
X-Device-Token: <token>
Content-Type: multipart/form-data
```

The binary file must be sent in form field `firmware`.

Example with curl:

```bash
curl -X POST \
  -H "X-Device-Token: $TOKEN" \
  -F "firmware=@.pio/build/esp32dev/firmware.bin" \
  http://esp32-device.local/api/ota
```

Successful response:

```json
{
  "ok": true,
  "restartRequired": true,
  "message": "Firmware uploaded. Device will restart."
}
```

The device restarts automatically after sending the response.

## Android app implementation notes

Use a multipart upload from the Android app. The request should include the saved device token in `X-Device-Token`. After a successful response, the app should wait for the device to reboot, rediscover it through mDNS, and then call `GET /api/info` and `GET /api/state` again.
