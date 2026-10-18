// ============================================================
// device_info.h — 芯片 / 固件身份信息（上电只算一次）
// ============================================================
#pragma once
#include <Arduino.h>

namespace Device {

void          begin();
String        fields();      // "model":"...","cores":2,...（无外层花括号）
String        macStr();
const String &resetName();
void          emitHello();

}  // namespace Device
