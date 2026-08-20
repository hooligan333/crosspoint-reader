#pragma once

// lib/hal/HalHeapGauge.h includes <Arduino.h> for ESP. This suite's ESP stub
// (with its adjustable largest block) lives in the HalStorage.h stub, which
// SdCardFont.cpp includes only after the gauge, so pull it in from here.
#include "HalStorage.h"
