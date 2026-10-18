// ============================================================
// cmd_router.cpp
// ============================================================
#include "cmd_router.h"

#include <vector>

#include "config.h"
#include "device_info.h"
#include "io_probe.h"
#include "json_out.h"
#include "net_info.h"
#include "runtime_info.h"

namespace {

String   g_lineBuf;
uint32_t g_interval = DEFAULT_INTERVAL;

const char *kVerbs[] = {
    "help", "info", "telemetry", "net", "scan", "temp",
    "gpio.get", "gpio.set", "gpio.mode", "adc.get",
    "interval", "wifi.sta", "wifi.ap", "wifi.off",
    "ping", "echo", "reboot",
};

bool isVerb(const String &v) {
  for (const char *k : kVerbs) if (v == k) return true;
  return false;
}

// 取第 k 个 token 之后的所有内容（用于含空格的参数）
String restAfter(const String &s, int k) {
  int i = 0, n = s.length(), cnt = 0;
  while (i < n && cnt < k) {
    while (i < n && s[i] == ' ') i++;
    while (i < n && s[i] != ' ') i++;
    cnt++;
  }
  while (i < n && s[i] == ' ') i++;
  return s.substring(i);
}

void splitFirst(const String &s, String &a, String &b) {
  int sp = s.indexOf(' ');
  if (sp < 0) { a = s; b = ""; }
  else { a = s.substring(0, sp); b = s.substring(sp + 1); b.trim(); }
}

String joinLines(const std::vector<String> &lines) {
  String out;
  for (size_t i = 0; i < lines.size(); i++) {
    if (i) out += "\n";
    out += lines[i];
  }
  return out;
}

void runScan(const String &id) {
  uint32_t dur = 0;
  int n = Net::scanAndEmit(&dur);
  if (n < 0) {
    Json::err(id, "scan", "wifi_fail", "radio is off");
    return;
  }
  Json::ack(id, "scan", true, "{\"n\":" + String(n) + ",\"dur_ms\":" + String(dur) + "}");
}

}  // namespace

void Cmd::begin() { g_lineBuf.reserve(256); }

uint32_t Cmd::intervalMs()          { return g_interval; }
void     Cmd::setIntervalMs(uint32_t ms) {
  if (ms < MIN_INTERVAL) ms = MIN_INTERVAL;
  if (ms > MAX_INTERVAL) ms = MAX_INTERVAL;
  g_interval = ms;
}

void Cmd::poll() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_lineBuf;
      g_lineBuf = "";
      line.trim();
      if (line.length()) execute(line);
    } else if (g_lineBuf.length() < 250) {
      g_lineBuf += c;
    }
  }
}

String Cmd::execute(const String &rawLine) {
  String line = rawLine;
  line.trim();
  if (!line.length()) return "";

  // 首 token 是已知动词 → 无 id；否则第一个当 id
  String first, rest;
  splitFirst(line, first, rest);

  String id, verb, args;
  if (isVerb(first)) {
    id = "";
    verb = first;
    args = rest;
  } else {
    id = first;
    String second;
    splitFirst(rest, second, args);
    verb = second;
  }
  verb.toLowerCase();
  args.trim();

  Json::startCapture();

  // ---------------- 基础 ----------------
  if (verb == "ping") {
    Json::ack(id, verb, true, "{\"pong\":" + String(millis()) + "}");

  } else if (verb == "help") {
    String cmds;
    for (const char *k : kVerbs) cmds += (cmds.length() ? ",\"" : "\"") + String(k) + "\"";
    Json::ack(id, verb, true, "{\"fw\":\"" FW_VERSION "\",\"cmds\":[" + cmds + "]}");

  } else if (verb == "info") {
    Device::emitHello();
    Net::emitNet();
    Json::ack(id, verb, true, "{\"refreshed\":[\"hello\",\"net\"]}");

  } else if (verb == "telemetry") {
    Rt::emitTelemetry();

  } else if (verb == "net") {
    Net::emitNet();

  } else if (verb == "scan") {
    runScan(id);

  } else if (verb == "temp") {
    float t = Io::chipTemp();
    Json::ack(id, verb, true, "{\"temp\":" + Json::num(t, 2) + "}");

  // ---------------- GPIO / ADC ----------------
  } else if (verb == "gpio.get") {
    if (!args.length() || args == "all") {
      Json::ack(id, verb, true, "{" + Io::gpioFields() + "}");
    } else {
      int pin = args.toInt();
      if (pin < 0 || pin >= 40 || !Io::isMonitored((uint8_t)pin)) {
        Json::err(id, verb, "pin_reserved", "pin " + String(pin) + " not monitored");
      } else {
        Json::ack(id, verb, true, "{\"pin\":" + String(pin) + ",\"value\":" +
                                       String(Io::read((uint8_t)pin) ? 1 : 0) + "}");
      }
    }

  } else if (verb == "gpio.set") {
    String a, b;
    splitFirst(args, a, b);
    if (!a.length() || !b.length()) {
      Json::err(id, verb, "bad_args", "usage: gpio.set <pin> <0|1>");
    } else {
      int pin = a.toInt();
      int val = (b == "1" || b.equalsIgnoreCase("high") || b == "on") ? 1 : 0;
      String e;
      if (Io::write((uint8_t)pin, val, e)) {
        Json::ack(id, verb, true, "{\"pin\":" + String(pin) + ",\"value\":" + String(val) +
                                       ",\"mode\":\"out\"}");
      } else {
        Json::err(id, verb, e, "pin " + String(pin) + " rejects output");
      }
    }

  } else if (verb == "gpio.mode") {
    String a, b;
    splitFirst(args, a, b);
    if (!a.length() || !b.length()) {
      Json::err(id, verb, "bad_args", "usage: gpio.mode <pin> in|out|in_pullup|in_pulldown");
    } else {
      String e;
      int pin = a.toInt();
      if (Io::setMode((uint8_t)pin, b, e)) {
        Json::ack(id, verb, true, "{\"pin\":" + String(pin) + ",\"mode\":" + Json::q(b) + "}");
      } else {
        Json::err(id, verb, e, "pin " + String(pin) + " mode rejected");
      }
    }

  } else if (verb == "adc.get") {
    if (!args.length()) {
      Json::err(id, verb, "bad_args", "usage: adc.get <32|33|34|35|36|39>");
    } else {
      int pin = args.toInt();
      int raw = 0, mv = 0;
      if (Io::adcRead((uint8_t)pin, raw, mv)) {
        Json::ack(id, verb, true, "{\"pin\":" + String(pin) + ",\"raw\":" + String(raw) +
                                       ",\"mv\":" + String(mv) + "}");
      } else {
        Json::err(id, verb, "pin_reserved", "adc only on ADC1 pins 32/33/34/35/36/39");
      }
    }

  // ---------------- 配置 ----------------
  } else if (verb == "interval") {
    if (!args.length()) {
      Json::ack(id, verb, true, "{\"interval_ms\":" + String(g_interval) + "}");
    } else {
      setIntervalMs((uint32_t)args.toInt());
      Json::ack(id, verb, true, "{\"interval_ms\":" + String(g_interval) + "}");
    }

  // ---------------- 网络 ----------------
  } else if (verb == "wifi.sta") {
    String ssid, pass;
    splitFirst(args, ssid, pass);
    if (!ssid.length()) {
      Json::err(id, verb, "bad_args", "usage: wifi.sta <ssid> <pass>");
    } else {
      String e;
      if (Net::connectSta(ssid, pass, e)) {
        Net::saveCredentials(ssid, pass);
        Json::ack(id, verb, true, "{\"ssid\":" + Json::q(ssid) + ",\"ip\":" +
                                      Json::q(Net::ip()) + ",\"saved\":true}");
      } else {
        Json::err(id, verb, e, "cannot join " + ssid);
      }
    }

  } else if (verb == "wifi.ap") {
    String ssid, pass;
    splitFirst(args, ssid, pass);
    if (!ssid.length()) ssid = String(AP_PREFIX) + Device::macStr().substring(12);
    String e;
    if (Net::startAp(ssid, pass, e)) {
      Json::ack(id, verb, true, "{\"ssid\":" + Json::q(ssid) + ",\"ip\":" +
                                    Json::q(Net::ip()) + ",\"open\":" +
                                    String(pass.length() ? "false" : "true") + "}");
    } else {
      Json::err(id, verb, e, "cannot start softap");
    }

  } else if (verb == "wifi.off") {
    Net::radioOff();
    Json::ack(id, verb, true, "{\"mode\":\"off\"}");

  // ---------------- 其他 ----------------
  } else if (verb == "echo") {
    String text = restAfter(line, isVerb(first) ? 1 : 2);
    Json::ack(id, verb, true, "{\"text\":" + Json::q(text) + "}");

  } else if (verb == "reboot") {
    Json::ack(id, verb, true, "{\"restarting\":true}");
    Json::stopCapture();
    delay(200);
    ESP.restart();

  } else {
    Json::err(id, verb, "unknown_cmd", "type 'help' for the command list");
  }

  std::vector<String> lines = Json::stopCapture();
  return joinLines(lines);
}
