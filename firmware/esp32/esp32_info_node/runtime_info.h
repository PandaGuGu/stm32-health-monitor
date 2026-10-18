// ============================================================
// runtime_info.h — 运行时资源（堆 / CPU / 任务 / 温度 / RSSI）
// ============================================================
#pragma once
#include <Arduino.h>

namespace Rt {

String fields();          // "heap":...,"cpu":240,...
void   emitTelemetry();    // telemetry 行 = 运行时 + IO

}  // namespace Rt
