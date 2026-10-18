// ============================================================
// net_info.cpp
// ============================================================
#include "net_info.h"

#include <Preferences.h>
#include <WiFi.h>

#include "config.h"
#include "device_info.h"
#include "json_out.h"

namespace {

Preferences g_prefs;
String      g_ssid;        // STA 凭据（NVS 持久化）
String      g_pass;
String      g_lastState;   // 上一状态，用于变化上报
uint32_t    g_lastRetry = 0;
uint32_t    g_lastNetEmit = 0;

const char *encName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
    default:                        return "UNKNOWN";
  }
}

String stateStr() {
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) return "off";
  if (mode == WIFI_AP)  return "ap_listening";
  switch (WiFi.status()) {
    case WL_CONNECTED:  return "connected";
    case WL_IDLE_STATUS:
    case WL_DISCONNECTED:
    case WL_NO_SSID_AVAIL: return (WiFi.status() == WL_NO_SSID_AVAIL) ? "no_ssid" : "connecting";
    case WL_CONNECT_FAILED: return "failed";
    default: return "disconnected";
  }
}

}  // namespace

void Net::begin() {
  WiFi.persistent(false);
  WiFi.setSleep(false);          // 关省电，保证 HTTP 响应及时

  g_prefs.begin(NVS_NS, false);
  g_ssid = g_prefs.getString("ssid", "");
  g_pass = g_prefs.getString("pass", "");
  g_prefs.end();

  if (g_ssid.length()) {
    String err;
    connectSta(g_ssid, g_pass, err);
  } else {
    String apSsid = String(AP_PREFIX) + Device::macStr().substring(12);
    apSsid.replace(":", "");
    String err;
    startAp(apSsid, AP_PASS, err);
    Json::log('I', "no credentials, fallback AP: " + apSsid);
  }
}

void Net::loop() {
  String st = stateStr();
  if (st != g_lastState) {
    g_lastState = st;
    emitNet();
    Json::log('I', "net state -> " + st);
  }

  // STA 掉线重连（10 s 一次，别把主循环堵住）
  if (WiFi.getMode() != WIFI_OFF && WiFi.getMode() != WIFI_AP && g_ssid.length()) {
    if (WiFi.status() != WL_CONNECTED && millis() - g_lastRetry > 10000) {
      g_lastRetry = millis();
      Json::log('W', "sta reconnect");
      WiFi.disconnect();
      WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    }
  }

  // 联网状态 5 s 兜底刷新（RSSI 会变）
  if (millis() - g_lastNetEmit > 5000) {
    g_lastNetEmit = millis();
    if (connected()) emitNet();
  }
}

bool Net::saveCredentials(const String &ssid, const String &pass) {
  g_prefs.begin(NVS_NS, false);
  g_prefs.putString("ssid", ssid);
  g_prefs.putString("pass", pass);
  g_prefs.end();
  g_ssid = ssid;
  g_pass = pass;
  return true;
}

bool Net::connectSta(const String &ssid, const String &pass, String &errCode) {
  if (!ssid.length()) { errCode = "bad_args"; return false; }
  g_ssid = ssid;
  g_pass = pass;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("esp32-info-node");
  WiFi.begin(ssid.c_str(), pass.c_str());
  Json::log('I', "sta begin ssid=" + ssid);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(200);
  }
  if (WiFi.status() != WL_CONNECTED) {
    errCode = "wifi_fail";
    Json::log('E', "sta connect timeout");
    return false;
  }
  g_lastRetry = millis();
  emitNet();
  return true;
}

bool Net::startAp(const String &ssid, const String &pass, String &errCode) {
  if (!ssid.length()) { errCode = "bad_args"; return false; }
  WiFi.mode(WIFI_AP);
  bool ok = pass.length() >= 8 ? WiFi.softAP(ssid.c_str(), pass.c_str())
                               : WiFi.softAP(ssid.c_str());
  if (!ok) { errCode = "wifi_fail"; return false; }
  g_lastRetry = millis();
  emitNet();
  return true;
}

void Net::radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  g_lastRetry = millis();
  emitNet();
}

bool   Net::connected() { return WiFi.status() == WL_CONNECTED; }
int    Net::rssi()      { return connected() ? WiFi.RSSI() : 0; }

String Net::ip() {
  if (WiFi.getMode() == WIFI_AP) return WiFi.softAPIP().toString();
  return WiFi.localIP().toString();
}

String Net::modeStr() {
  switch (WiFi.getMode()) {
    case WIFI_STA:    return "sta";
    case WIFI_AP:     return "ap";
    case WIFI_AP_STA: return "apsta";
    default:          return "off";
  }
}

String Net::fields() {
  wifi_mode_t mode = WiFi.getMode();
  bool ap = (mode == WIFI_AP || mode == WIFI_AP_STA);
  bool sta = (mode == WIFI_STA || mode == WIFI_AP_STA);

  String f;
  f += "\"mode\":" + Json::q(modeStr());
  f += ",\"state\":" + Json::q(stateStr());
  f += ",\"ssid\":" + Json::q(ap ? WiFi.softAPSSID() : (sta ? WiFi.SSID() : String("")));
  f += ",\"ip\":" + Json::q(ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString());
  f += ",\"gw\":" + Json::q(sta && connected() ? WiFi.gatewayIP().toString() : String("0.0.0.0"));
  f += ",\"mask\":" + Json::q(sta && connected() ? WiFi.subnetMask().toString() : String("0.0.0.0"));
  f += ",\"dns\":" + Json::q(sta && connected() ? WiFi.dnsIP().toString() : String("0.0.0.0"));
  f += ",\"mac\":" + Json::q(WiFi.macAddress());
  f += ",\"bssid\":" + Json::q(connected() ? WiFi.BSSIDstr() : String(""));
  f += ",\"ch\":" + String(connected() ? WiFi.channel() : (ap ? WiFi.channel() : 0));
  f += ",\"rssi\":" + String(connected() ? WiFi.RSSI() : 0);
  f += ",\"host\":" + Json::q(String(WiFi.getHostname() ? WiFi.getHostname() : "esp32-info-node"));
  f += ",\"clients\":" + String(ap ? WiFi.softAPgetStationNum() : 0);
  return f;
}

void Net::emitNet() {
  Json::emit(Json::head("net") + "," + fields() + "}");
}

int Net::scanAndEmit(uint32_t *durMs) {
  wifi_mode_t prev = WiFi.getMode();
  if (prev == WIFI_OFF) { if (durMs) *durMs = 0; return -1; }
  if (prev == WIFI_AP) WiFi.mode(WIFI_AP_STA);   // 扫描需要 station 接口

  uint32_t t0 = millis();
  int n = WiFi.scanNetworks(false, true);        // 同步、显示隐藏 SSID
  uint32_t dur = millis() - t0;
  if (durMs) *durMs = dur;

  if (n < 0) n = 0;
  if (n > 24) n = 24;                            // 控制单行长度

  String aps;
  for (int i = 0; i < n; i++) {
    if (i) aps += ",";
    String ssid = WiFi.SSID(i);
    aps += "{\"ssid\":" + Json::q(ssid) +
           ",\"bssid\":" + Json::q(WiFi.BSSIDstr(i)) +
           ",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"ch\":" + String(WiFi.channel(i)) +
           ",\"enc\":" + Json::q(encName(WiFi.encryptionType(i))) + "}";
  }
  WiFi.scanDelete();

  Json::emit(Json::head("scan") + ",\"n\":" + String(n) + ",\"dur_ms\":" + String(dur) +
             ",\"aps\":[" + aps + "]}");

  if (prev == WIFI_AP) WiFi.mode(WIFI_AP);
  return n;
}

void Net::emitScan() { scanAndEmit(nullptr); }
