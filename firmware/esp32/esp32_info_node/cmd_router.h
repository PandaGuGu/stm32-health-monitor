// ============================================================
// cmd_router.h — 下行文本命令解析与派发
//   语法：[<id>] <verb> [args...]      id 由上位机自增，用于 ack 配对
// ============================================================
#pragma once
#include <Arduino.h>

namespace Cmd {

void     begin();
void     poll();                        // 主循环里调：收串口命令
String   execute(const String &line);   // 执行一条命令，返回本次产生的所有行（\n 分隔）
uint32_t intervalMs();
void     setIntervalMs(uint32_t ms);

}  // namespace Cmd
