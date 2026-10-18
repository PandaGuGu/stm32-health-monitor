// ============================================================
// json_out.cpp
// ============================================================
#include "json_out.h"
#include "config.h"

namespace {
Stream *g_sink = nullptr;
bool    g_capture = false;
std::vector<String> g_captured;
}  // namespace

void Json::begin(Stream *s) { g_sink = s; }

void Json::emit(const String &line) {
  if (g_capture) {
    if (g_captured.size() < 32) g_captured.push_back(line);
  }
  if (g_sink) {
    g_sink->print(line);
    g_sink->print('\n');
  }
}

void Json::startCapture() {
  g_captured.clear();
  g_capture = true;
}

std::vector<String> Json::stopCapture() {
  g_capture = false;
  return g_captured;
}

String Json::esc(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", (uint8_t)c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

String Json::q(const String &in)  { return "\"" + esc(in) + "\""; }
String Json::q(const char *in)    { return q(String(in ? in : "")); }

String Json::num(double v, int dec) { return String(v, dec); }

String Json::head(const String &type) {
  return "{\"v\":" + String(PROTO_VERSION) + ",\"t\":" + q(type) +
         ",\"ts\":" + String(millis());
}

void Json::log(char lvl, const String &msg) {
  char l[2] = {lvl, 0};
  emit(head("log") + ",\"lvl\":" + q(l) + ",\"msg\":" + q(msg) + "}");
}

void Json::ack(const String &id, const String &cmd, bool ok, const String &dataJson) {
  String line = head("ack") + ",\"id\":" + q(id) + ",\"cmd\":" + q(cmd) +
                ",\"ok\":" + String(ok ? "true" : "false");
  if (dataJson.length()) line += ",\"data\":" + dataJson;
  line += "}";
  emit(line);
}

void Json::err(const String &id, const String &cmd, const String &code, const String &msg) {
  emit(head("err") + ",\"id\":" + q(id) + ",\"cmd\":" + q(cmd) +
       ",\"code\":" + q(code) + ",\"msg\":" + q(msg) + "}");
}
