// ============================================================
// esp32_info_node — ESP32 信息节点（上位机调试用）
//
// 职责：只做"采集 + 上报 + 听从命令"，不做业务判定。
//   · 串口 115200：JSON Lines 上行 + 文本命令下行
//   · WiFi 可选：自带 HTTP /api/*，供网关轮询或 curl 手测
// 协议见 docs/esp32/protocol.md
// ============================================================
#include <Arduino.h>

#include "cmd_router.h"
#include "config.h"
#include "device_info.h"
#include "http_api.h"
#include "io_probe.h"
#include "json_out.h"
#include "net_info.h"
#include "runtime_info.h"
#include "uart_link.h"

static uint32_t s_lastTelemetry = 0;
static uint32_t s_firstTelemetry = 0;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(150);

  Json::begin(&Serial);
  Io::begin();
  Device::begin();
  Cmd::begin();
  Vitals::begin();          // UART2：接 STM32
  Net::begin();
  HttpApi::begin();

  // 一上电就把身份和网络状态吐出来，上位机不必等轮询
  Device::emitHello();
  Net::emitNet();
  Rt::emitTelemetry();

  Json::log('I', String(FW_NAME) + " " FW_VERSION " boot, reset=" + Device::resetName());

  s_firstTelemetry = millis();
  s_lastTelemetry  = millis();
}

void loop() {
  Net::loop();
  Cmd::poll();
  Vitals::loop();           // 收 STM32 的 vitals 帧
  HttpApi::loop();

  uint32_t now = millis();
  if (now - s_lastTelemetry >= Cmd::intervalMs()) {
    s_lastTelemetry = now;
    Rt::emitTelemetry();
  }

  // 心跳：不占带宽，只是让 LED 有节奏（固件活着的最直观证据）
  if (now - s_firstTelemetry >= 1000) {
    s_firstTelemetry = now;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  delay(2);
}
