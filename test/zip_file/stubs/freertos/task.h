#pragma once

#include "FreeRTOS.h"

// One host thread stands in for one task: a stable non-null handle.
inline TaskHandle_t xTaskGetCurrentTaskHandle() {
  static int hostTask;
  return &hostTask;
}
