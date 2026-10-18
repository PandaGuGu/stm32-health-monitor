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
  t += "GET /api/snapshot | /api/info | /api/telemetry | /api/net | /api/scan\n";
  t += "GET /api/cmd?cmd=gpio.get&id=1\n";
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

}  // namespace

void HttpApi::begin() {
  g_server.on("/",            HTTP_GET, handleRoot);
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
