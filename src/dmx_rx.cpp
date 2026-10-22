// The ESP-IDF UART driver reports BREAK and DATA as separate queue events, so the
// tail of one frame and the start of the next can arrive out of order. Instead this
// installs its own ISR that drains the FIFO and handles the break in the same pass,
// which keeps slot alignment exact. Approach follows someweisguy/esp_dmx (MIT).

#include "dmx_rx.h"

#include <Arduino.h>
#include <driver/uart.h>
#include <esp_intr_alloc.h>
#include <hal/uart_ll.h>
#include <soc/interrupts.h>
#include <string.h>

namespace {

constexpr uart_port_t kPort = UART_NUM_1;
constexpr uint32_t kRxIntr =
    UART_INTR_RXFIFO_FULL | UART_INTR_BRK_DET | UART_INTR_FRAM_ERR | UART_INTR_RXFIFO_OVF;

// A macro rather than a const pointer so the IRAM ISR never reads flash.
#define DMX_UART UART_LL_GET_HW(1)
intr_handle_t isrHandle;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// Frame being received. One extra byte for the 0x00 the UART clocks in for the break.
DRAM_ATTR uint8_t rxBuf[DMX_FRAME_SIZE + 1];
DRAM_ATTR int rxHead = -1;   // -1 until the first break, so we never start mid-frame
DRAM_ATTR int rxErrAt = -1;  // rxHead when a framing error was last seen, -1 if none

// Last good frame, handed to the application under `mux`.
DRAM_ATTR uint8_t frame[DMX_FRAME_SIZE];
DRAM_ATTR uint16_t frameSlots;
DRAM_ATTR volatile uint32_t frameCount;
DRAM_ATTR volatile uint32_t errorCount;

void IRAM_ATTR dmxIsr(void *) {
  for (;;) {
    const uint32_t st = uart_ll_get_intsts_mask(DMX_UART) & kRxIntr;
    if (!st) break;

    if (st & UART_INTR_RXFIFO_OVF) {
      uart_ll_rxfifo_rst(DMX_UART);
      if (rxHead > 0) errorCount = errorCount + 1;
      rxHead = -1;  // resync on the next break
    } else {
      uint32_t n = uart_ll_get_rxfifo_len(DMX_UART);
      if (n) {
        if (rxHead >= 0 && rxHead + n <= sizeof(rxBuf)) {
          uart_ll_read_rxfifo(DMX_UART, rxBuf + rxHead, n);
          rxHead += n;
        } else {
          // Not synced yet, or more than 512 slots: discard until the next break.
          uart_ll_rxfifo_rst(DMX_UART);
          if (rxHead >= 0) {
            errorCount = errorCount + 1;
            rxHead = -1;
          }
        }
      }
    }

    // An error before the start code can't corrupt slots (the break's own error may
    // be reported after the break was handled), so only track errors mid-frame.
    if ((st & UART_INTR_FRAM_ERR) && rxHead > 0) rxErrAt = rxHead;

    if (st & UART_INTR_BRK_DET) {
      // rxBuf holds the frame that just ended plus the break's 0x00. The break
      // itself raises a framing error on that last byte; an error any earlier
      // means a corrupted slot, so the frame is dropped.
      const int len = rxHead - 1;
      const bool clean = rxErrAt < 0 || rxErrAt >= rxHead - 1;
      if (len >= 2 && clean && rxBuf[0] == 0x00) {
        portENTER_CRITICAL_ISR(&mux);
        memcpy(frame, rxBuf, len);
        frameSlots = len - 1;
        frameCount = frameCount + 1;
        portEXIT_CRITICAL_ISR(&mux);
      } else if (len >= 2 && !clean) {
        errorCount = errorCount + 1;
      }
      rxHead = 0;
      rxErrAt = -1;
    }

    uart_ll_clr_intsts_mask(DMX_UART, st);
  }
}

}  // namespace

void dmxBegin(int txPin, int rxPin) {
  uart_config_t cfg = {};
  cfg.baud_rate = 250000;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_2;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_param_config(kPort, &cfg));
  // TX is never written, but it must be claimed so it idles high: the auto-direction
  // RS485 transceiver would otherwise drive the DMX line from a floating pin.
  ESP_ERROR_CHECK(uart_set_pin(kPort, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  uart_ll_disable_intr_mask(DMX_UART, UART_LL_INTR_MASK);
  uart_ll_clr_intsts_mask(DMX_UART, UART_LL_INTR_MASK);
  uart_ll_set_rxfifo_full_thr(DMX_UART, 1);
  uart_ll_rxfifo_rst(DMX_UART);
  ESP_ERROR_CHECK(esp_intr_alloc(ETS_UART1_INTR_SOURCE, ESP_INTR_FLAG_IRAM, dmxIsr, nullptr, &isrHandle));
  uart_ll_ena_intr_mask(DMX_UART, kRxIntr);
}

uint32_t dmxFrameCount() { return frameCount; }
uint32_t dmxErrorCount() { return errorCount; }

uint16_t dmxCopyFrame(uint8_t out[DMX_FRAME_SIZE]) {
  portENTER_CRITICAL(&mux);
  uint16_t slots = frameSlots;
  memcpy(out, frame, slots + 1);
  portEXIT_CRITICAL(&mux);
  return slots;
}
