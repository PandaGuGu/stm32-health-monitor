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

void Rt::emitTelemetry() {
  Json::emit(Json::head("telemetry") + "," + Rt::fields() + "," + Io::fields() + "," +
             Vitals::fields() + "}");
}
