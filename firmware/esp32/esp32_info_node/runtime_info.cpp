// ============================================================
// runtime_info.cpp
// ============================================================
#include "runtime_info.h"

#include <freertos/task.h>

#include "config.h"
#include "io_probe.h"
#include "json_out.h"
#include "net_info.h"
#include "uart_link.h"

String Rt::fields() {
  uint32_t heap     = ESP.getFreeHeap();
  uint32_t heapMin  = ESP.getMinFreeHeap();
  uint32_t heapMax  = ESP.getMaxAllocHeap();
  uint32_t psram    = ESP.getPsramSize();
  uint32_t psramFr  = ESP.getFreePsram();

  // 碎片度：最大可分配块 / 空闲堆。越接近 100 越"整"
  uint8_t frag = heap ? (uint8_t)(100 - (heapMax * 100UL / heap)) : 0;

  String f;
  f += "\"up\":" + String(millis() / 1000UL);
  f += ",\"heap\":" + String(heap);
  f += ",\"heap_min\":" + String(heapMin);
  f += ",\"heap_max\":" + String(heapMax);
  f += ",\"frag\":" + String(frag);
  f += ",\"cpu\":" + String(getCpuFrequencyMhz());
  f += ",\"tasks\":" + String((uint32_t)uxTaskGetNumberOfTasks());
  f += ",\"temp\":" + Json::num(Io::chipTemp(), 2);
  f += ",\"rssi\":" + String(Net::rssi());
  if (psram) {
    f += ",\"psram_free\":" + String(psramFr);
    f += ",\"psram_total\":" + String(psram);
  }
  return f;
}

namespace {
const int SERIES_N = 60;      // 60 点 ≈ 1 分钟（按 1 Hz）

struct Pt {
  uint32_t t;
  uint32_t heap;
  float    temp;
  int      rssi;
  int      bpm;
};

Pt      g_series[SERIES_N];
uint8_t g_head = 0;           // 下一个写入位置
uint8_t g_cnt  = 0;
uint32_t g_lines = 0;

void pushPoint(uint32_t heap, float temp, int rssi, int bpm) {
  Pt &p = g_series[g_head];
  p.t = millis() / 1000UL;
  p.heap = heap;
  p.temp = temp;
  p.rssi = rssi;
  p.bpm = bpm;
  g_head = (uint8_t)((g_head + 1) % SERIES_N);
  if (g_cnt < SERIES_N) g_cnt++;
}
}  // namespace

String Rt::seriesJson() {
  String s = "[";
  for (uint8_t i = 0; i < g_cnt; i++) {
    uint8_t idx = (uint8_t)((g_head + SERIES_N - g_cnt + i) % SERIES_N);
    const Pt &p = g_series[idx];
    if (i) s += ",";
    s += "{\"t\":" + String(p.t) + ",\"heap\":" + String(p.heap) +
         ",\"temp\":" + Json::num(p.temp, 2) + ",\"rssi\":" + String(p.rssi) +
         ",\"bpm\":" + String(p.bpm) + "}";
  }
  s += "]";
  return s;
}

uint32_t Rt::lineCount() { return g_lines; }

void Rt::emitTelemetry() {
  uint32_t heap = ESP.getFreeHeap();
  float    t    = Io::chipTemp();
  int      rssi = Net::rssi();
  pushPoint(heap, t, rssi, Vitals::bpm());
  g_lines++;

  Json::emit(Json::head("telemetry") + "," + Rt::fields() + "," + Io::fields() + "," +
             Vitals::fields() + "}");
}
