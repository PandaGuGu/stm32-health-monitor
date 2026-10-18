// ============================================================
// io_probe.h — 板载 IO / ADC / 芯片温度
//   引脚白名单写在 config/本文件：可输出、仅输入、硬禁用
// ============================================================
#pragma once
#include <Arduino.h>

namespace Io {

void   begin();

bool   isOutputPin(uint8_t pin);     // gpio.set 只接受这些
bool   isAdcPin(uint8_t pin);        // ADC1：32/33/34/35/36/39
bool   isMonitored(uint8_t pin);

String fields();                     // "gpio":{...},"adc":{...}
String gpioFields();
String adcFields();

int    read(uint8_t pin);
bool   write(uint8_t pin, int value, String &errCode);
bool   setMode(uint8_t pin, const String &mode, String &errCode);
bool   adcRead(uint8_t pin, int &raw, int &mv);

float  chipTemp();

}  // namespace Io
