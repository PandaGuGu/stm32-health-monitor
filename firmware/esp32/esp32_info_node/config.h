// ============================================================
// config.h — ESP32 信息节点 · 编译期配置
// 引脚策略 / 上报周期 / AP 兜底参数都在这里，改完重新编译即可
// ============================================================
#pragma once
#include <Arduino.h>

#define FW_NAME         "esp32-info-node"
#define FW_VERSION      "0.1.0"
#define PROTO_VERSION   1

// ---- 串口 ----
#define SERIAL_BAUD     115200

// ---- 遥测 ----
#define DEFAULT_INTERVAL 1000      // 默认 1 Hz
#define MIN_INTERVAL     100
#define MAX_INTERVAL     60000

// ---- AP 兜底（NVS 里没有 WiFi 凭据时启用）----
#define AP_PREFIX        "esp32-info-"
#define AP_PASS          "esp32info"   // >= 8 位，否则起不来
#define HTTP_PORT        80

// ---- NVS ----
#define NVS_NS           "info_node"

// ---- 上电默认采样的 ADC 引脚（ADC1；ADC2 与 WiFi 冲突，不开放）----
#define DEFAULT_ADC_PINS { 34, 35 }

// ---- 板载 LED ----
#define LED_PIN          2
