#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
namespace trim::esp32::board {
// E32R35T candidate revision. LCD reset is wired to ESP32 EN, not an output GPIO.
constexpr gpio_num_t sck=GPIO_NUM_14, mosi=GPIO_NUM_13, miso=GPIO_NUM_12, cs=GPIO_NUM_15, dc=GPIO_NUM_2,
    backlight=GPIO_NUM_27, touch_cs=GPIO_NUM_33, sd_cs=GPIO_NUM_5;
constexpr auto spi=SPI2_HOST;
constexpr auto uart=UART_NUM_0;
// The onboard CH340 bridge is wired to UART0's TX/RX pads.
constexpr gpio_num_t uart_tx=GPIO_NUM_1, uart_rx=GPIO_NUM_3;
constexpr int baud=115200, spi_hz=10000000;
}
