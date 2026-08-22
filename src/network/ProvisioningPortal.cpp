#include "ProvisioningPortal.hpp"

#include "Board.hpp"

#include <M5Unified.h>
#include <WebServer.h>
#include <WiFi.h>

#include "AppConfig.hpp"

namespace {

WebServer* server = nullptr;
ConfigStore* store = nullptr;
DeviceConfig working;
bool saved = false;

String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

String field(const char* name, const char* label, const String& value, const char* type = "text") {
    return String("<label>") + label + "<input name='" + name + "' type='" + type + "' value='" +
           htmlEscape(value) + "'></label>";
}

void handleRoot() {
    String page = F(
        "<!doctype html><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>M5Companion setup</title><style>"
        "body{font-family:system-ui,sans-serif;background:#12161a;color:#e8eaed;margin:0;padding:24px}"
        "h1{font-size:20px;color:#ff8a3d;margin:0 0 4px}p{color:#9aa5ad;margin:0 0 20px;font-size:13px}"
        "label{display:block;margin:0 0 14px;font-size:13px;color:#9aa5ad}"
        "input,select{width:100%;box-sizing:border-box;margin-top:5px;padding:10px;border-radius:8px;"
        "border:1px solid #2c343b;background:#1b2127;color:#e8eaed;font-size:15px}"
        "button{width:100%;padding:13px;border:0;border-radius:8px;background:#ff8a3d;color:#12161a;"
        "font-size:16px;font-weight:600}</style>"
        "<h1>M5Companion</h1><p>Wi-Fi and companion server</p><form method='POST' action='/save'>");

    // Offer what the radio can actually see; typing an SSID by hand on a phone
    // is where most first-run setups go wrong.
    const int found = WiFi.scanNetworks();
    if (found > 0) {
        page += F("<label>Nearby networks<select onchange=\"document.getElementsByName('ssid')[0]"
                  ".value=this.value\"><option value=''>-- pick one --</option>");
        for (int i = 0; i < found && i < 20; ++i) {
            page += "<option value='" + htmlEscape(WiFi.SSID(i)) + "'>" + htmlEscape(WiFi.SSID(i)) +
                    " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
        }
        page += F("</select></label>");
    }

    page += field("ssid", "Wi-Fi SSID", working.wifiSsid);
    page += field("pass", "Wi-Fi password", working.wifiPassword, "password");
    page += field("host", "Server host or IP", working.serverHost);
    page += field("port", "Server port", String(working.serverPort), "number");
    page += field("path", "WebSocket path", working.serverPath);
    page += field("name", "Device name", working.deviceName);
    page += field("pack", "Character pack", working.packName);
    page += F("<button type='submit'>Save and reboot</button></form>");

    server->send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
    working.wifiSsid = server->arg("ssid");
    working.wifiPassword = server->arg("pass");
    working.serverHost = server->arg("host");
    working.serverPort = static_cast<uint16_t>(server->arg("port").toInt());
    if (!working.serverPort) working.serverPort = appcfg::kDefaultServerPort;
    working.serverPath = server->arg("path");
    if (working.serverPath.isEmpty()) working.serverPath = appcfg::kDefaultServerPath;
    working.deviceName = server->arg("name");
    if (working.deviceName.isEmpty()) working.deviceName = board::kDefaultDeviceName;
    working.packName = server->arg("pack");
    if (working.packName.isEmpty()) working.packName = "claudecode";

    saved = store && store->save(working);
    server->send(200, "text/html; charset=utf-8",
                 saved ? F("<meta charset='utf-8'><body style='font-family:sans-serif'>"
                           "<h2>Saved. The M5GO is rebooting.</h2>")
                       : F("<meta charset='utf-8'><body style='font-family:sans-serif'>"
                           "<h2>Save failed. Try again.</h2>"));
}

}  // namespace

void ProvisioningPortal::drawScreen(const char* ssid, const char* ip, const char* status) {
    auto& d = M5.Display;
    d.setRotation(1);
    d.fillScreen(0x0841);
    d.setFont(&fonts::Font4);
    d.setTextColor(0xFBE0, 0x0841);
    d.setCursor(20, 24);
    d.print("Setup mode");

    d.setFont(&fonts::Font2);
    d.setTextColor(0xFFFF, 0x0841);
    d.setCursor(20, 78);
    d.printf("Wi-Fi: %s", ssid);
    d.setCursor(20, 104);
    d.printf("Open:  http://%s", ip);

    d.setFont(&fonts::Font0);
    d.setTextColor(0x8C71, 0x0841);
    d.setCursor(20, 150);
    d.print("Connect a phone to that network,");
    d.setCursor(20, 164);
    d.print("then open the address above.");
    d.setCursor(20, 200);
    d.print(status);
}

bool ProvisioningPortal::run(ConfigStore& configStore, const DeviceConfig& current) {
    store = &configStore;
    working = current;
    saved = false;

    uint8_t mac[6] = {};
    WiFi.macAddress(mac);
    char apName[24];
    snprintf(apName, sizeof(apName), "M5Companion-%02X%02X", mac[4], mac[5]);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName);
    delay(300);
    const String ip = WiFi.softAPIP().toString();

    server = new WebServer(80);
    server->on("/", HTTP_GET, handleRoot);
    server->on("/save", HTTP_POST, handleSave);
    server->onNotFound(handleRoot);  // acts as a crude captive portal
    server->begin();

    drawScreen(apName, ip.c_str(), "waiting for setup...");

    const uint32_t start = millis();
    while (!saved && millis() - start < appcfg::kProvisioningTimeoutMs) {
        server->handleClient();
        M5.update();
        // Escape hatch: hold C to leave setup without saving.
        if (M5.BtnC.wasHold()) break;
        delay(2);
    }

    drawScreen(apName, ip.c_str(), saved ? "saved - rebooting" : "leaving setup");
    delay(900);

    server->stop();
    delete server;
    server = nullptr;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    return saved;
}
