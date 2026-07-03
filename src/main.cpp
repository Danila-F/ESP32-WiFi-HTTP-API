#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

#ifndef DEFAULT_POWER_GPIO
#define DEFAULT_POWER_GPIO 2
#endif

#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 15000
#endif

static constexpr const char *API_VERSION = "1";
static constexpr const char *SETUP_AP_PASSWORD = "esp32setup";
static constexpr const char *MDNS_SERVICE_TYPE = "espctrl";
static constexpr const char *MDNS_SERVICE_PROTO = "tcp";
static constexpr uint16_t HTTP_PORT = 80;
static constexpr size_t MAX_JSON_BODY_BYTES = 4096;

struct DeviceConfig {
    String deviceId;
    String deviceName;
    String room;
    String mdnsHost;
    String authToken;
    String wifiSsid;
    String wifiPassword;
    bool provisioned = false;
    int powerGpio = DEFAULT_POWER_GPIO;
    bool powerGpioActiveHigh = true;
};

Preferences prefs;
WebServer server(HTTP_PORT);
DeviceConfig cfg;

bool networkOnline = false;
bool powerState = false;
String setupApSsid;
IPAddress setupApIp;
bool otaAuthorized = false;
bool otaHadError = false;
String otaError;
uint32_t restartAtMs = 0;

static String chipHex() {
    const uint64_t mac = ESP.getEfuseMac();
    char buffer[13];
    snprintf(buffer, sizeof(buffer), "%04X%08X", static_cast<uint16_t>(mac >> 32), static_cast<uint32_t>(mac));
    String result(buffer);
    result.toLowerCase();
    return result;
}

static String sanitizeHostname(const String &value) {
    String result;
    result.reserve(value.length());
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = static_cast<char>(tolower(value[i]));
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') {
            result += c;
        } else {
            result += '-';
        }
    }
    result.trim();
    while (result.startsWith("-")) result.remove(0, 1);
    while (result.endsWith("-")) result.remove(result.length() - 1);
    if (result.isEmpty()) result = "esp32-device";
    if (result.length() > 31) result = result.substring(0, 31);
    return result;
}

static String defaultToken() {
    char buffer[49];
    snprintf(
        buffer,
        sizeof(buffer),
        "%08lx-%08lx-%s",
        static_cast<unsigned long>(esp_random()),
        static_cast<unsigned long>(esp_random()),
        chipHex().c_str()
    );
    return String(buffer);
}

static bool constantTimeEquals(const String &left, const String &right) {
    if (left.length() != right.length()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < left.length(); ++i) {
        diff |= static_cast<uint8_t>(left[i] ^ right[i]);
    }
    return diff == 0;
}

static void saveConfig() {
    prefs.putString("device_id", cfg.deviceId);
    prefs.putString("name", cfg.deviceName);
    prefs.putString("room", cfg.room);
    prefs.putString("mdns", cfg.mdnsHost);
    prefs.putString("token", cfg.authToken);
    prefs.putString("ssid", cfg.wifiSsid);
    prefs.putString("wifi_pass", cfg.wifiPassword);
    prefs.putBool("provisioned", cfg.provisioned);
    prefs.putInt("power_gpio", cfg.powerGpio);
    prefs.putBool("gpio_high", cfg.powerGpioActiveHigh);
}

static void loadConfig() {
    cfg.deviceId = prefs.getString("device_id", "");
    cfg.deviceName = prefs.getString("name", "");
    cfg.room = prefs.getString("room", "");
    cfg.mdnsHost = prefs.getString("mdns", "");
    cfg.authToken = prefs.getString("token", "");
    cfg.wifiSsid = prefs.getString("ssid", "");
    cfg.wifiPassword = prefs.getString("wifi_pass", "");
    cfg.provisioned = prefs.getBool("provisioned", false);
    cfg.powerGpio = prefs.getInt("power_gpio", DEFAULT_POWER_GPIO);
    cfg.powerGpioActiveHigh = prefs.getBool("gpio_high", true);

    const String chip = chipHex();
    if (cfg.deviceId.isEmpty()) cfg.deviceId = "esp32-" + chip;
    if (cfg.deviceName.isEmpty()) cfg.deviceName = "ESP32 Device " + chip.substring(chip.length() > 4 ? chip.length() - 4 : 0);
    if (cfg.mdnsHost.isEmpty()) cfg.mdnsHost = sanitizeHostname(cfg.deviceId);
    if (cfg.authToken.isEmpty()) cfg.authToken = defaultToken();
    saveConfig();
}

static void sendCorsHeaders() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Authorization, X-Device-Token, Content-Type");
    server.sendHeader("Cache-Control", "no-store");
}

static void sendJson(JsonDocument &doc, int statusCode = 200) {
    String output;
    serializeJson(doc, output);
    sendCorsHeaders();
    server.send(statusCode, "application/json", output);
}

static void sendError(int statusCode, const char *code, const String &message) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"]["code"] = code;
    doc["error"]["message"] = message;
    sendJson(doc, statusCode);
}

static bool parseJsonBody(JsonDocument &doc) {
    if (!server.hasArg("plain")) {
        sendError(400, "empty_body", "Expected JSON request body");
        return false;
    }
    const String body = server.arg("plain");
    if (body.length() > MAX_JSON_BODY_BYTES) {
        sendError(413, "body_too_large", "JSON body is too large");
        return false;
    }
    const DeserializationError error = deserializeJson(doc, body);
    if (error) {
        sendError(400, "invalid_json", error.c_str());
        return false;
    }
    return true;
}

static bool isAuthorized() {
    if (cfg.authToken.isEmpty()) return true;
    if (server.hasHeader("X-Device-Token") && constantTimeEquals(server.header("X-Device-Token"), cfg.authToken)) return true;
    if (server.hasHeader("Authorization")) {
        const String authorization = server.header("Authorization");
        const String prefix = "Bearer ";
        if (authorization.startsWith(prefix)) {
            return constantTimeEquals(authorization.substring(prefix.length()), cfg.authToken);
        }
    }
    return false;
}

static void scheduleRestart(uint32_t delayMs = 1200) {
    restartAtMs = millis() + delayMs;
}

static void applyPowerState(bool enabled) {
    powerState = enabled;
    pinMode(cfg.powerGpio, OUTPUT);
    const int level = cfg.powerGpioActiveHigh ? (enabled ? HIGH : LOW) : (enabled ? LOW : HIGH);
    digitalWrite(cfg.powerGpio, level);
    prefs.putBool("power", powerState);
}

static void handleInfo() {
    JsonDocument doc;
    doc["apiVersion"] = API_VERSION;
    doc["firmwareVersion"] = APP_VERSION;
    doc["deviceId"] = cfg.deviceId;
    doc["name"] = cfg.deviceName;
    doc["room"] = cfg.room;
    doc["type"] = "switch";
    doc["manufacturer"] = "DIY ESP32";
    doc["model"] = "ESP32 WiFi HTTP API";
    doc["authRequired"] = !cfg.authToken.isEmpty();
    doc["provisioned"] = cfg.provisioned;
    doc["mdnsHost"] = cfg.mdnsHost + ".local";
    doc["mdnsService"] = String("_") + MDNS_SERVICE_TYPE + "._" + MDNS_SERVICE_PROTO;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["setupApSsid"] = setupApSsid;
    doc["setupApIp"] = setupApIp.toString();
    doc["ota"]["enabled"] = true;
    doc["ota"]["endpoint"] = "/api/ota";
    doc["ota"]["method"] = "POST multipart/form-data field=firmware";
    sendJson(doc);
}

static void handleState() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    JsonDocument doc;
    doc["online"] = true;
    doc["networkOnline"] = networkOnline;
    doc["power"] = powerState;
    doc["uptimeMs"] = millis();
    doc["freeHeap"] = ESP.getFreeHeap();
    doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    doc["ip"] = WiFi.localIP().toString();
    doc["firmwareVersion"] = APP_VERSION;
    sendJson(doc);
}

static void handleActions() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    JsonDocument doc;
    JsonArray actions = doc["actions"].to<JsonArray>();

    JsonObject setPower = actions.add<JsonObject>();
    setPower["id"] = "set_power";
    setPower["title"] = "Set power";
    setPower["type"] = "boolean";

    JsonObject togglePower = actions.add<JsonObject>();
    togglePower["id"] = "toggle_power";
    togglePower["title"] = "Toggle power";
    togglePower["type"] = "command";

    JsonObject reboot = actions.add<JsonObject>();
    reboot["id"] = "reboot";
    reboot["title"] = "Reboot device";
    reboot["type"] = "command";

    sendJson(doc);
}

static void handleGetConfig() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    JsonDocument doc;
    doc["deviceId"] = cfg.deviceId;
    doc["name"] = cfg.deviceName;
    doc["room"] = cfg.room;
    doc["mdnsHost"] = cfg.mdnsHost;
    doc["wifiSsid"] = cfg.wifiSsid;
    doc["provisioned"] = cfg.provisioned;
    doc["powerGpio"] = cfg.powerGpio;
    doc["powerGpioActiveHigh"] = cfg.powerGpioActiveHigh;
    sendJson(doc);
}

static void handlePostConfig() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    JsonDocument body;
    if (!parseJsonBody(body)) return;

    cfg.deviceName = body["name"] | cfg.deviceName;
    cfg.room = body["room"] | cfg.room;
    cfg.powerGpio = body["powerGpio"] | cfg.powerGpio;
    cfg.powerGpioActiveHigh = body["powerGpioActiveHigh"] | cfg.powerGpioActiveHigh;
    saveConfig();
    applyPowerState(powerState);

    JsonDocument doc;
    doc["ok"] = true;
    sendJson(doc);
}

static void handleProvision() {
    if (cfg.provisioned && !isAuthorized()) {
        sendError(401, "unauthorized", "Provisioning already completed. Provide the current token or factory-reset the device.");
        return;
    }
    JsonDocument body;
    if (!parseJsonBody(body)) return;

    const String ssid = body["ssid"] | "";
    if (ssid.isEmpty()) {
        sendError(400, "invalid_provisioning", "Field 'ssid' is required");
        return;
    }

    cfg.wifiSsid = ssid;
    cfg.wifiPassword = body["password"] | "";
    cfg.deviceName = body["name"] | cfg.deviceName;
    cfg.room = body["room"] | cfg.room;
    cfg.authToken = body["authToken"] | cfg.authToken;
    cfg.mdnsHost = sanitizeHostname(cfg.deviceId);
    cfg.provisioned = true;
    saveConfig();

    JsonDocument doc;
    doc["ok"] = true;
    doc["deviceId"] = cfg.deviceId;
    doc["name"] = cfg.deviceName;
    doc["authToken"] = cfg.authToken;
    doc["restartRequired"] = true;
    sendJson(doc);
    scheduleRestart();
}

static void handleCommand() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    JsonDocument body;
    if (!parseJsonBody(body)) return;

    const String command = body["command"] | "";
    if (command == "set_power") {
        applyPowerState(body["value"] | false);
    } else if (command == "toggle_power") {
        applyPowerState(!powerState);
    } else if (command == "reboot") {
        scheduleRestart();
    } else {
        sendError(400, "unknown_command", "Unsupported command: " + command);
        return;
    }

    JsonDocument doc;
    doc["ok"] = true;
    doc["power"] = powerState;
    doc["restartScheduled"] = restartAtMs != 0;
    sendJson(doc);
}

static void handleFactoryReset() {
    if (!isAuthorized()) {
        sendError(401, "unauthorized", "Missing or invalid device token");
        return;
    }
    const bool ok = prefs.clear();
    JsonDocument doc;
    doc["ok"] = ok;
    doc["restartRequired"] = true;
    sendJson(doc, ok ? 200 : 500);
    scheduleRestart();
}

static void handleOtaUpload() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        otaAuthorized = isAuthorized();
        otaHadError = false;
        otaError = "";
        if (!otaAuthorized) {
            otaHadError = true;
            otaError = "Missing or invalid device token";
            return;
        }
        Serial.printf("OTA upload started: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            otaHadError = true;
            otaError = Update.errorString();
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!otaAuthorized || otaHadError) return;
        const size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            otaHadError = true;
            otaError = Update.errorString();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!otaAuthorized || otaHadError) return;
        if (!Update.end(true)) {
            otaHadError = true;
            otaError = Update.errorString();
            return;
        }
        Serial.printf("OTA upload complete: %u bytes\n", upload.totalSize);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        otaHadError = true;
        otaError = "Upload aborted";
        Update.abort();
    }
}

static void handleOtaFinish() {
    if (!otaAuthorized) {
        sendError(401, "unauthorized", otaError.isEmpty() ? "Missing or invalid device token" : otaError);
        return;
    }
    if (otaHadError) {
        sendError(500, "ota_failed", otaError.isEmpty() ? "OTA update failed" : otaError);
        return;
    }
    JsonDocument doc;
    doc["ok"] = true;
    doc["restartRequired"] = true;
    doc["message"] = "Firmware uploaded. Device will restart.";
    sendJson(doc);
    scheduleRestart(800);
}

static void handleNotFound() {
    if (server.method() == HTTP_OPTIONS) {
        sendCorsHeaders();
        server.send(204);
        return;
    }
    sendError(404, "not_found", "Endpoint not found");
}

static void registerRoutes() {
    const char *headers[] = {"Authorization", "X-Device-Token", "Content-Type", "Content-Length"};
    server.collectHeaders(headers, sizeof(headers) / sizeof(headers[0]));

    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/actions", HTTP_GET, handleActions);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.on("/api/provision", HTTP_POST, handleProvision);
    server.on("/api/command", HTTP_POST, handleCommand);
    server.on("/api/factory-reset", HTTP_POST, handleFactoryReset);
    server.on("/api/ota", HTTP_POST, handleOtaFinish, handleOtaUpload);
    server.onNotFound(handleNotFound);
}

static bool connectToConfiguredWiFi() {
    if (cfg.wifiSsid.isEmpty()) {
        Serial.println("WiFi is not provisioned yet");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg.mdnsHost.c_str());
    WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPassword.c_str());

    Serial.printf("Connecting to WiFi SSID '%s'", cfg.wifiSsid.c_str());
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_CONNECT_TIMEOUT_MS) {
        delay(250);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi connection failed");
        return false;
    }

    Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    return true;
}

static void startSetupAccessPoint() {
    const String suffix = cfg.deviceId.substring(cfg.deviceId.length() > 6 ? cfg.deviceId.length() - 6 : 0);
    setupApSsid = "ESP32-Setup-" + suffix;

    WiFi.mode(WiFi.status() == WL_CONNECTED ? WIFI_AP_STA : WIFI_AP);
    if (!WiFi.softAP(setupApSsid.c_str(), SETUP_AP_PASSWORD)) {
        Serial.println("Failed to start setup access point");
        return;
    }

    setupApIp = WiFi.softAPIP();
    Serial.printf("Setup AP started: SSID=%s password=%s IP=%s\n", setupApSsid.c_str(), SETUP_AP_PASSWORD, setupApIp.toString().c_str());
}

static void startMdns() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (!MDNS.begin(cfg.mdnsHost.c_str())) {
        Serial.println("mDNS start failed");
        return;
    }

    MDNS.addService(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, HTTP_PORT);
    MDNS.addServiceTxt(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, "id", cfg.deviceId.c_str());
    MDNS.addServiceTxt(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, "name", cfg.deviceName.c_str());
    MDNS.addServiceTxt(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, "api", API_VERSION);
    MDNS.addServiceTxt(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, "fw", APP_VERSION);
    MDNS.addServiceTxt(MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO, "type", "switch");

    Serial.printf("mDNS started: http://%s.local/ service=_%s._%s\n", cfg.mdnsHost.c_str(), MDNS_SERVICE_TYPE, MDNS_SERVICE_PROTO);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("ESP32 WiFi HTTP API firmware starting");

    if (!prefs.begin("esp-api", false)) {
        Serial.println("Failed to initialize NVS storage");
    }
    loadConfig();
    powerState = prefs.getBool("power", false);
    applyPowerState(powerState);

    Serial.printf("Device ID: %s\n", cfg.deviceId.c_str());
    Serial.printf("Device token: %s\n", cfg.authToken.c_str());

    networkOnline = connectToConfiguredWiFi();
    if (!networkOnline || !cfg.provisioned) {
        startSetupAccessPoint();
    }

    startMdns();
    registerRoutes();
    server.begin();
    Serial.printf("HTTP API listening on port %u\n", HTTP_PORT);
}

void loop() {
    server.handleClient();
    if (restartAtMs != 0 && static_cast<int32_t>(millis() - restartAtMs) >= 0) {
        Serial.println("Restarting now");
        delay(50);
        ESP.restart();
    }
}
