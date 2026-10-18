// ============================================================
// http_api.cpp — ESP32 自带 HTTP 接口（供网关轮询 / curl 手测）
// ============================================================
#include "http_api.h"

#include <WebServer.h>

#include "cmd_router.h"
#include "device_info.h"
#include "io_probe.h"
#include "json_out.h"
#include "net_info.h"
#include "runtime_info.h"
#include "uart_link.h"
#include "config.h"

namespace {

WebServer g_server(HTTP_PORT);

void sendJson(int code, const String &body) {
  g_server.sendHeader("Access-Control-Allow-Origin", "*");
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(code, "application/json", body);
}

void handleRoot() {
  String ip = Net::ip();
  String t;
  t += FW_NAME " " FW_VERSION "\n";
  t += "mode : " + Net::modeStr() + "\n";
  t += "ip   : " + ip + "\n";
  t += "mac  : " + Device::macStr() + "\n";
  t += "heap : " + String(ESP.getFreeHeap()) + " B\n";
  t += "up   : " + String(millis() / 1000UL) + " s\n\n";
  t += "手机直连：App 里把地址填 http://" + ip + "\n\n";
  t += "GET  /api/status    （与 PC 网关同构的快照，App 直连用）\n";
  t += "GET  /api/snapshot | /api/info | /api/telemetry | /api/net | /api/scan\n";
  t += "GET  /api/cmd?cmd=gpio.get&id=1\n";
  t += "POST /api/cmd  {\"cmd\":\"gpio.set 2 1\"}\n";
  g_server.send(200, "text/plain; charset=utf-8", t);
}

void handleSnapshot() {
  String b = "{\"device\":{" + Device::fields() + "}";
  b += ",\"net\":{" + Net::fields() + "}";
  b += ",\"runtime\":{" + Rt::fields() + "}";
  b += ",\"io\":{" + Io::fields() + "}";
  b += "}";
  sendJson(200, b);
}

void handleInfo() {
  sendJson(200, Json::head("hello") + "," + Device::fields() + "}");
}

void handleTelemetry() {
  Json::startCapture();
  Rt::emitTelemetry();
  auto lines = Json::stopCapture();
  sendJson(200, lines.size() ? lines[0] : "{}");
}

void handleNet() {
  Json::startCapture();
  Net::emitNet();
  auto lines = Json::stopCapture();
  sendJson(200, lines.size() ? lines[0] : "{}");
}

void handleScan() {
  Json::startCapture();
  uint32_t dur = 0;
  int n = Net::scanAndEmit(&dur);
  auto lines = Json::stopCapture();
  if (n < 0) { sendJson(503, "{\"error\":\"radio off\"}"); return; }
  sendJson(200, lines.size() ? lines[0] : "{}");
}

void handleCmd() {
  String cmd = g_server.arg("cmd");
  String id  = g_server.arg("id");
  cmd.trim();
  if (!cmd.length()) { sendJson(400, "{\"error\":\"missing cmd\"}"); return; }

  String line = id.length() ? (id + " " + cmd) : cmd;
  String out  = Cmd::execute(line);

  // 把多行结果包成数组，保持合法 JSON
  String body = "{\"lines\":[";
  bool first = true;
  int start = 0;
  while (start <= (int)out.length()) {
    int nl = out.indexOf('\n', start);
    String one = (nl < 0) ? out.substring(start) : out.substring(start, nl);
    if (one.length()) {
      if (!first) body += ",";
      first = false;
      body += one;
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  body += "]}";
  sendJson(200, body);
}

void handleNotFound() {
  sendJson(404, "{\"error\":\"not found\"}");
}

// ---------------------------------------------------------------- 直连模式
// 手机连上 ESP32 的热点后，App 直接打设备。为了复用同一套 App，
// /api/status 的字段与 PC 网关的 snapshot 保持一致（source=device）。
String bodyField(const String &body, const char *key) {
  String pat = String("\"") + key + "\"";
  int i = body.indexOf(pat);
  if (i < 0) return "";
  i = body.indexOf(':', i + pat.length());
  if (i < 0) return "";
  i++;
  while (i < (int)body.length() && body[i] == ' ') i++;
  if (i >= (int)body.length() || body[i] != '"') return "";
  int j = body.indexOf('"', i + 1);
  if (j < 0) return "";
  return body.substring(i + 1, j);
}

void handleStatus() {
  String b = "{";
  b += "\"source\":\"device\",\"connected\":true,\"port\":\"\",\"host\":" + Json::q(Net::ip());
  b += ",\"device\":{" + Device::fields() + "}";
  b += ",\"net\":{" + Net::fields() + "}";
  b += ",\"runtime\":{" + Rt::fields() + "}";
  b += ",\"io\":{" + Io::fields() + "}";
  b += ",\"scan\":" + Net::lastScan();
  b += "," + Vitals::fields();                       // 自带 "vitals":{...}
  b += ",\"series\":" + Rt::seriesJson();
  b += ",\"last_seen_age\":0";
  b += ",\"stats\":{\"lines\":" + String(Rt::lineCount()) +
       ",\"errors\":" + String(Vitals::bad()) + ",\"reconnects\":0,\"acks\":0}";
  b += ",\"proto_mismatch\":false";
  b += ",\"ts\":" + String(millis() / 1000UL);
  b += "}";
  sendJson(200, b);
}

// App 直连时会 POST /api/connect，设备侧没有"数据源"概念，直接回成功
void handleConnect() {
  sendJson(200, "{\"ok\":true,\"source\":\"device\",\"host\":" + Json::q(Net::ip()) + "}");
}

// App 枚举串口用；设备上没有串口可枚举
void handlePorts() {
  sendJson(200, "{\"ports\":[]}");
}

// POST /api/cmd  {"cmd":"gpio.set 2 1"} → {"ok":true,"ack":{...}}
void handleCmdPost() {
  String body = g_server.arg("plain");
  String cmd  = bodyField(body, "cmd");
  cmd.trim();
  if (!cmd.length()) { sendJson(400, "{\"error\":\"missing cmd\"}"); return; }

  String out = Cmd::execute(String("9 ") + cmd);     // 固定 id，设备侧无需配对

  String ack = "";
  int s = 0;
  while (s < (int)out.length()) {
    int nl = out.indexOf('\n', s);
    String one = (nl < 0) ? out.substring(s) : out.substring(s, nl);
    if (one.indexOf("\"t\":\"ack\"") >= 0 || one.indexOf("\"t\":\"err\"") >= 0) {
      ack = one;
      break;
    }
    if (nl < 0) break;
    s = nl + 1;
  }

  String resp = "{\"ok\":true,\"id\":9,\"cmd\":" + Json::q(cmd) +
                ",\"ack\":" + (ack.length() ? ack : String("null")) + "}";
  sendJson(200, resp);
}

}  // namespace

void HttpApi::begin() {
  g_server.on("/",            HTTP_GET, handleRoot);
  g_server.on("/api/status",    HTTP_GET, handleStatus);     // 直连模式（App）
  g_server.on("/api/connect",   HTTP_POST, handleConnect);
  g_server.on("/api/ports",     HTTP_GET, handlePorts);
  g_server.on("/api/cmd",       HTTP_POST, handleCmdPost);
  g_server.on("/api/snapshot",  HTTP_GET, handleSnapshot);
  g_server.on("/api/info",      HTTP_GET, handleInfo);
  g_server.on("/api/telemetry", HTTP_GET, handleTelemetry);
  g_server.on("/api/net",       HTTP_GET, handleNet);
  g_server.on("/api/scan",      HTTP_GET, handleScan);
  g_server.on("/api/cmd",       HTTP_GET, handleCmd);
  g_server.onNotFound(handleNotFound);
  g_server.begin();
  Json::log('I', "http api on :" + String(HTTP_PORT));
}

void HttpApi::loop() { g_server.handleClient(); }
