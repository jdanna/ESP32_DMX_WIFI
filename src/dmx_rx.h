// DMX512 receiver on the RS485 port, driven by a small UART interrupt handler.
#pragma once

#include <stdint.h>

#define DMX_FRAME_SIZE 513  // start code + 512 slots

void dmxBegin(int txPin, int rxPin);

// Number of valid frames received since boot. Changes whenever a new frame lands.
uint32_t dmxFrameCount();
uint32_t dmxErrorCount();

// Copies the latest complete frame (start code at [0]) into `out`.
// Returns the number of slots in it, excluding the start code.
uint16_t dmxCopyFrame(uint8_t out[DMX_FRAME_SIZE]);
