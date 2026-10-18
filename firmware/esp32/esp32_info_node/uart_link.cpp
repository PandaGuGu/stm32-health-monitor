// ============================================================
// uart_link.cpp
// ============================================================
#include "uart_link.h"

#include "config.h"
#include "json_out.h"

namespace {

struct VState {
  bool  ok = false;
  int   bpm = 0;
  int   spo2 = 0;
  int   temp10 = 0;      // 0.1℃ 定点：365 = 36.5℃
  int   fall = 0;
  char  from[8] = "none";
  uint32_t lastMs = 0;
  uint32_t frames = 0;
  uint32_t bad = 0;
};

VState   g;
String   g_buf;
bool     g_uartOn = true;
uint32_t g_lastRxMs = 0;

// ---- 极简 JSON 取值（不引 ArduinoJson）----
const char *findKey(const char *line, const char *key) {
  String pat = String("\"") + key + "\"";
  const char *p = strstr(line, pat.c_str());
  if (!p) return nullptr;
  p = strchr(p, ':');
  if (!p) return nullptr;
  p++;
  while (*p == ' ') p++;
  return p;
}

bool getInt(const char *line, const char *key, int *out) {
  const char *p = findKey(line, key);
  if (!p) return false;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  if (*p < '0' || *p > '9') return false;
  long v = 0;
  while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
  *out = (int)(neg ? -v : v);
  return true;
}

// 36.5 → 365；36 → 360（单位：0.1℃）
bool getTenths(const char *line, const char *key, int *out) {
  const char *p = findKey(line, key);
  if (!p) return false;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  if (*p < '0' || *p > '9') return false;
  int whole = 0;
  while (*p >= '0' && *p <= '9') { whole = whole * 10 + (*p - '0'); p++; }
  int tenth = 0;
  if (*p == '.') {
    p++;
    if (*p >= '0' && *p <= '9') tenth = *p - '0';
  }
  int t = whole * 10 + tenth;
  *out = neg ? -t : t;
  return true;
}

}  // namespace

void Vitals::begin() {
  if (g_uartOn) {
    Serial2.begin(VITALS_BAUD, SERIAL_8N1, VITALS_RX_PIN, VITALS_TX_PIN);
    Serial2.setTimeout(20);
    Json::log('I', String("vitals: uart2 on rx=") + VITALS_RX_PIN + " tx=" + VITALS_TX_PIN +
                       " @" + VITALS_BAUD);
  }
}

void Vitals::loop() {
  if (!g_uartOn) return;
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = g_buf;
      g_buf = "";
      line.trim();
      if (line.length()) {
        g_lastRxMs = millis();
        if (!feedLine(line, "uart")) {
          Json::log('W', "vitals: bad frame: " + line.substring(0, 60));
        }
      }
    } else if (g_buf.length() < 190) {
      g_buf += c;
    }
  }
}

bool Vitals::feedLine(const String &line, const char *from) {
  const char *s = line.c_str();
  int bpm = 0, spo2 = 0, temp10 = 0, fall = 0;
  if (!getInt(s, "bpm", &bpm)) {      // bpm 是必需字段，没有就当垃圾帧
    g.bad++;
    return false;
  }
  if (!getInt(s, "spo2", &spo2)) spo2 = 0;
  if (!getTenths(s, "temp", &temp10)) temp10 = 0;
  if (!getInt(s, "fall", &fall)) fall = 0;

  // 基本合理性：越界数据不采信，避免把 STM32 的异常值原样透传
  if (bpm < 0 || bpm > 250 || spo2 < 0 || spo2 > 100) {
    g.bad++;
    return false;
  }

  g.ok = true;
  g.bpm = bpm;
  g.spo2 = spo2;
  g.temp10 = temp10;
  g.fall = fall ? 1 : 0;
  snprintf(g.from, sizeof(g.from), "%s", from ? from : "uart");
  g.lastMs = millis();
  g.frames++;
  return true;
}

void Vitals::inject(int bpm, int spo2, int temp10, int fall) {
  String line = "{\"bpm\":" + String(bpm) + ",\"spo2\":" + String(spo2) +
                ",\"temp\":" + String(temp10 / 10) + "." + String(abs(temp10 % 10)) +
                ",\"fall\":" + String(fall) + ",\"src\":\"sim\"}";
  feedLine(line, "sim");
}

void Vitals::echoSample() {
  if (!g_uartOn) return;
  int t10 = g.temp10 ? g.temp10 : 365;
  String line = "{\"bpm\":78,\"spo2\":97,\"temp\":" + String(t10 / 10) + "." +
                String(abs(t10 % 10)) + ",\"fall\":0,\"src\":\"stm32\"}\r\n";
  Serial2.print(line);
  Serial2.flush();
}

bool Vitals::uartOn()      { return g_uartOn; }
int  Vitals::frames()      { return (int)g.frames; }
int  Vitals::bad()         { return (int)g.bad; }
int  Vitals::bpm()         { return g.ok ? g.bpm : 0; }

void Vitals::setUart(bool on) {
  g_uartOn = on;
  if (on) {
    Serial2.begin(VITALS_BAUD, SERIAL_8N1, VITALS_RX_PIN, VITALS_TX_PIN);
    Serial2.setTimeout(20);
    g_buf = "";
    Json::log('I', "vitals: uart2 enabled");
  } else {
    Serial2.end();
    g.ok = false;
    snprintf(g.from, sizeof(g.from), "off");
    Json::log('I', "vitals: uart2 disabled");
  }
}

String Vitals::fields() {
  uint32_t age = g.ok ? (millis() - g.lastMs) : 0xFFFFFFFFUL;
  String f = "\"vitals\":{";
  f += "\"ok\":" + String(g.ok && age < VITALS_STALE_MS ? "true" : "false");
  f += ",\"bpm\":" + String(g.bpm);
  f += ",\"spo2\":" + String(g.spo2);
  f += ",\"temp\":" + Json::num(g.temp10 / 10.0, 1);
  f += ",\"fall\":" + String(g.fall);
  f += ",\"src\":" + Json::q(String(g.from));
  f += ",\"age_ms\":";
  if (g.ok) f += String(age); else f += "null";
  f += ",\"frames\":" + String(g.frames);
  f += ",\"bad\":" + String(g.bad);
  f += ",\"uart\":" + String(g_uartOn ? "true" : "false");
  f += "}";
  return f;
}
