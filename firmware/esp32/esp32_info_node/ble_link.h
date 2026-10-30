// ============================================================
// ble_link.h — 蓝牙链路：把 vitals 帧用 BLE 广播给手机
//
// 走 Nordic UART Service（NUS）这组标准 UUID，手机端不需要定制协议：
//   Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   TX(通知) 6E400003-B5A3-F393-E0A9-E50E24DCCA9E   ← 设备发这个
//   RX(写)   6E400002-B5A3-F393-E0A9-E50E24DCCA9E   （预留，暂未用）
//
// 载荷直接是 {"vitals":{...}}，与 HTTP /api/status 的 vitals 段同构，
// 所以手机端解析函数可以完全复用，不用为蓝牙另写一套。
//
// 默认不编译（config.h 里 ENABLE_BLE=0）。与 WiFi 可共存，但射频分时，
// 同时开启时两边的吞吐都会下降。
// ============================================================
#pragma once
#include <Arduino.h>

namespace Ble {

void begin();
void loop();          // 有新帧就 notify 一次

void setEnabled(bool on);
bool enabled();
bool connected();
int  mtu();           // 当前协商到的 MTU，未连接时返回 0
uint32_t notifies();  // 成功发出的通知数
String name();        // 广播名（含 MAC 后四位）

}  // namespace Ble
