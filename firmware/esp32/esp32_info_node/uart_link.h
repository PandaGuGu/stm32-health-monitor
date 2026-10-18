// ============================================================
// uart_link.h — 与 STM32 的数据链路（UART2 ↔ USART1）
//
// STM32 每周期发一行：
//   {"bpm":75,"spo2":98,"temp":36.5,"fall":0,"src":"stm32"}
// 本模块负责收/解析，并把结果并入 telemetry 的 vitals 字段。
// 没有 STM32 时用 vitals.sim 注入同一格式的帧，解析路径完全一致。
// ============================================================
#pragma once
#include <Arduino.h>

namespace Vitals {

void   begin();
void   loop();

bool   feedLine(const String &line, const char *from);   // 解析一行 JSON
String fields();                                          // "vitals":{...}

void   inject(int bpm, int spo2, int temp10, int fall);    // 模拟源（vitals.sim）
void   echoSample();                                       // 往 UART2 发一帧样例（环回自测）

bool   uartOn();
void   setUart(bool on);

int    frames();
int    bad();
int    bpm();            // 最近一次有效帧的心率，无有效帧返回 0

}  // namespace Vitals
