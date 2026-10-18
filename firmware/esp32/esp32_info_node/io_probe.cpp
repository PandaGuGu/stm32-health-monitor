// ============================================================
// io_probe.cpp
// ============================================================
#include "io_probe.h"

#include "config.h"
#include "json_out.h"

namespace {

// 可输出引脚：避开 0（strap）、1/3（UART0）、6-11（SPI Flash）、34-39（仅输入）
const uint8_t kOutputPins[] = {2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};
const uint8_t kInputOnlyPins[] = {34, 35, 36, 39};
const uint8_t kAdcPins[] = {32, 33, 34, 35, 36, 39};

bool g_monitor[40];
bool g_adcOn[40];
uint8_t g_lastMode[40];   // 记录我们设过的模式，便于 ack 回报

bool inList(const uint8_t *list, size_t n, uint8_t pin) {
  for (size_t i = 0; i < n; i++) if (list[i] == pin) return true;
  return false;
}

}  // namespace

void Io::begin() {
  memset(g_monitor, 0, sizeof(g_monitor));
  memset(g_adcOn, 0, sizeof(g_adcOn));
  memset(g_lastMode, 0xFF, sizeof(g_lastMode));

  for (uint8_t p : kOutputPins)     g_monitor[p] = true;
  for (uint8_t p : kInputOnlyPins)  g_monitor[p] = true;

  const uint8_t defAdc[] = DEFAULT_ADC_PINS;
  for (uint8_t p : defAdc) if (isAdcPin(p)) g_adcOn[p] = true;

  analogReadResolution(12);

  // 板载 LED 作为心跳/可写指示
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  g_lastMode[LED_PIN] = OUTPUT;
}

bool Io::isOutputPin(uint8_t pin)  { return inList(kOutputPins, sizeof(kOutputPins), pin); }
bool Io::isAdcPin(uint8_t pin)     { return inList(kAdcPins, sizeof(kAdcPins), pin); }
bool Io::isMonitored(uint8_t pin)  { return pin < 40 && g_monitor[pin]; }

int Io::read(uint8_t pin) { return digitalRead(pin); }

bool Io::write(uint8_t pin, int value, String &errCode) {
  if (!isOutputPin(pin)) { errCode = "pin_not_output"; return false; }
  if (g_lastMode[pin] != OUTPUT) {
    pinMode(pin, OUTPUT);
    g_lastMode[pin] = OUTPUT;
  }
  digitalWrite(pin, value ? HIGH : LOW);
  return true;
}

bool Io::setMode(uint8_t pin, const String &mode, String &errCode) {
  if (isAdcPin(pin) && pin >= 34) { errCode = "pin_not_output"; return false; }
  if (!isOutputPin(pin) && !isAdcPin(pin)) { errCode = "pin_reserved"; return false; }

  uint8_t m;
  if (mode == "out")             m = OUTPUT;
  else if (mode == "in")         m = INPUT;
  else if (mode == "in_pullup")  m = INPUT_PULLUP;
  else if (mode == "in_pulldown") m = INPUT_PULLDOWN;
  else { errCode = "bad_args"; return false; }

  if (m == OUTPUT && !isOutputPin(pin)) { errCode = "pin_not_output"; return false; }

  pinMode(pin, m);
  g_lastMode[pin] = m;
  return true;
}

bool Io::adcRead(uint8_t pin, int &raw, int &mv) {
  if (!isAdcPin(pin)) return false;
  g_adcOn[pin] = true;
  raw = analogRead(pin);
  mv  = analogReadMilliVolts(pin);
  return true;
}

float Io::chipTemp() {
  // 内部温度传感器：读数比环境高 5~15℃，仅用于趋势观察
  return temperatureRead();
}

String Io::gpioFields() {
  String s = "\"gpio\":{";
  bool first = true;
  for (uint8_t p = 0; p < 40; p++) {
    if (!g_monitor[p]) continue;
    if (!first) s += ",";
    first = false;
    s += "\"" + String(p) + "\":" + String(digitalRead(p) ? 1 : 0);
  }
  s += "}";
  return s;
}

String Io::adcFields() {
  String s = "\"adc\":{";
  bool first = true;
  for (uint8_t p = 0; p < 40; p++) {
    if (!g_adcOn[p]) continue;
    int raw = analogRead(p);
    int mv  = analogReadMilliVolts(p);
    if (!first) s += ",";
    first = false;
    s += "\"" + String(p) + "\":{\"raw\":" + String(raw) + ",\"mv\":" + String(mv) + "}";
  }
  s += "}";
  return s;
}

String Io::fields() {
  return gpioFields() + "," + adcFields();
}
