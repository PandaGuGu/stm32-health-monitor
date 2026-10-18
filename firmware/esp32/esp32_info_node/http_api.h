// ============================================================
// http_api.h — WiFi 直连调试接口（设备侧只出 JSON，不托管前端）
// ============================================================
#pragma once
#include <Arduino.h>

namespace HttpApi {

void begin();
void loop();

}  // namespace HttpApi
