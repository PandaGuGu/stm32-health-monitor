// ============================================================
// runtime_info.h — 运行时资源（堆 / CPU / 任务 / 温度 / RSSI）
// ============================================================
#pragma once
#include <Arduino.h>

namespace Rt {

String fields();          // "heap":...,"cpu":240,...
void   emitTelemetry();    // telemetry 行 = 运行时 + IO + vitals

String   seriesJson();     // 最近 N 点趋势：[{"t":..,"heap":..,"temp":..,"rssi":..,"bpm":..}]
uint32_t lineCount();      // 已产出多少条 telemetry（给设备侧 /api/status 的 stats 用）

}  // namespace Rt
