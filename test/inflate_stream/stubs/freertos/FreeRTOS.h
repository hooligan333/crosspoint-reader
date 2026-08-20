#pragma once

// Host stub for the FreeRTOS kernel header. BuildScratch.cpp's owner guard
// (only the lending task may claim the loan) asks for the current task
// handle; the host test runs on one thread, so every call is the lender.
using TaskHandle_t = void*;
