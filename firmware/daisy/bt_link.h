// bt_link.h — receives pedal control data from the ESP32 over UART.
//
// The ESP32 (pedalboard client) bridges BLE -> UART and streams a fixed
// 14-byte frame whenever a control changes. This module decodes that stream
// in the background (DMA + interrupt) and exposes the latest values.
//
// Wiring: ESP32 ESP_TX -> Daisy USART1_RX (pin 15 / D14 / PB7), shared GND.
//
// Usage:
//   BTLink::Init();              // call once after hw.Init()
//   float cutoff = BTLink::Knob1f();   // 0..1
//   uint16_t raw = BTLink::JoyX();     // 0..4095
//   if (BTLink::Switch()) { ... }      // joystick button pressed
//
// All getters are safe to call from the audio callback or main loop.

#pragma once
#include <cstdint>

namespace BTLink
{
/** Initializes USART1 (115200 8N1) and starts background reception. */
void Init();

/** Raw 12-bit values, 0..4095 (latest received). */
uint16_t Knob1();
uint16_t Knob2();
uint16_t Ldr();
uint16_t JoyX();
uint16_t JoyY();

/** Normalized 0..1 convenience versions. */
float Knob1f();
float Knob2f();
float Ldrf();
float JoyXf();
float JoyYf();

/** Joystick push-button: true while pressed. */
bool Switch();

/** True if a valid frame arrived within the last 500 ms. */
bool Connected();

/** Diagnostics: total bytes seen on the UART, and valid frames decoded. */
uint32_t RxBytes();
uint32_t RxFrames();

} // namespace BTLink
