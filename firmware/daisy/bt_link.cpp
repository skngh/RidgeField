// bt_link.cpp — see bt_link.h
//
// Frame format (from the ESP32 client, little-endian):
//   [0]      0xAA   sync
//   [1]      0x55   sync
//   [2..3]   Knob1  uint16
//   [4..5]   Knob2
//   [6..7]   LDR
//   [8..9]   JoyX
//   [10..11] JoyY
//   [12]     Switch (0/1)
//   [13]     checksum = XOR of bytes [2..12]

#include "bt_link.h"
#include "daisy_seed.h"

using namespace daisy;

namespace
{
UartHandler uart;
uint8_t     dma_buf[256]; // DMA landing buffer (DMA-accessible RAM)

struct Values
{
    uint16_t knob1, knob2, ldr, joyX, joyY;
    uint8_t  sw;
};
volatile Values   vals       = {0, 0, 0, 0, 0, 0};
volatile uint32_t last_rx_ms = 0;
volatile uint32_t rx_bytes   = 0; // total bytes seen (diagnostic)
volatile uint32_t rx_frames  = 0; // total valid frames decoded (diagnostic)

// Streaming frame parser ------------------------------------------------------
enum ParseState
{
    WAIT_AA,
    WAIT_55,
    PAYLOAD
};
ParseState pstate = WAIT_AA;
uint8_t payload[12]; // 11 data bytes + 1 checksum (everything after the sync)
uint8_t pidx = 0;

inline void ParseByte(uint8_t b)
{
    switch(pstate)
    {
        case WAIT_AA:
            if(b == 0xAA)
                pstate = WAIT_55;
            break;

        case WAIT_55:
            if(b == 0x55)
            {
                pstate = PAYLOAD;
                pidx   = 0;
            }
            else if(b != 0xAA) // allow back-to-back 0xAA before the 0x55
                pstate = WAIT_AA;
            break;

        case PAYLOAD:
            payload[pidx++] = b;
            if(pidx >= sizeof(payload))
            {
                uint8_t cksum = 0;
                for(int i = 0; i < 11; i++)
                    cksum ^= payload[i];

                if(cksum == payload[11]) // good frame -> publish
                {
                    vals.knob1 = payload[0] | (payload[1] << 8);
                    vals.knob2 = payload[2] | (payload[3] << 8);
                    vals.ldr   = payload[4] | (payload[5] << 8);
                    vals.joyX  = payload[6] | (payload[7] << 8);
                    vals.joyY  = payload[8] | (payload[9] << 8);
                    vals.sw    = payload[10];
                    last_rx_ms = System::GetNow();
                    rx_frames++;
                }
                pstate = WAIT_AA;
            }
            break;
    }
}

// Called from the DMA/UART interrupt with each chunk of new bytes.
void RxCallback(uint8_t* data, size_t size, void*, UartHandler::Result res)
{
    if(res != UartHandler::Result::OK)
        return;
    rx_bytes += size;
    for(size_t i = 0; i < size; i++)
        ParseByte(data[i]);
}
} // namespace

// Public API ------------------------------------------------------------------
void BTLink::Init()
{
    UartHandler::Config c;
    c.periph        = UartHandler::Config::Peripheral::USART_1;
    c.mode          = UartHandler::Config::Mode::RX;
    c.pin_config.tx = Pin(PORTB, 6); // D13 / pin 14 (unused in RX mode)
    c.pin_config.rx = Pin(PORTB, 7); // D14 / pin 15  <- ESP_TX
    c.baudrate      = 115200;        // must match the ESP32

    uart.Init(c);
    uart.DmaListenStart(dma_buf, sizeof(dma_buf), RxCallback, nullptr);
}

uint16_t BTLink::Knob1()
{ return vals.knob1; }
uint16_t BTLink::Knob2()
{ return vals.knob2; }
uint16_t BTLink::Ldr()
{ return vals.ldr; }
uint16_t BTLink::JoyX()
{ return vals.joyX; }
uint16_t BTLink::JoyY()
{ return vals.joyY; }

// knobs are reversed because of the orientation of them on the device
float BTLink::Knob1f()
{ return 1.0f - vals.knob1 / 4095.0f; }
float BTLink::Knob2f()
{ return 1.0f - vals.knob2 / 4095.0f; }
float BTLink::Ldrf()
{ return 1.0f - vals.ldr / 4095.0f; } // reversed so it's 1 when covered
float BTLink::JoyXf()
{ return vals.joyX / 4095.0f; }
float BTLink::JoyYf()
{ return vals.joyY / 4095.0f; }

bool BTLink::Switch()
{ return vals.sw != 0; }

bool BTLink::Connected()
{ return (System::GetNow() - last_rx_ms) < 500; }

uint32_t BTLink::RxBytes()
{ return rx_bytes; }
uint32_t BTLink::RxFrames()
{ return rx_frames; }
