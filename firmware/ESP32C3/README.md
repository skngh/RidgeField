# ESP32-C3 firmware

Here's the 2 Arduino sketches, both for the Seeed XIAO ESP32-C3.

- [`pedal_server/`](pedal_server/): goes on the **guitar unit**. Reads the knobs and LDR (through an MCP3208 ADC), plus the joystick and its button. Advertises over BLE and notifies whenever something changes. Also handles the battery level LED.
- [`pedal_client/`](pedal_client/): goes in the **pedal**. Finds the server, subscribes to every control, and forwards them to the Daisy over UART.

Pin maps are at the top of each sketch.

## Setup

1. Install the ESP32 boards package in Arduino IDE (Espressif, via Boards Manager) and pick **XIAO_ESP32C3**.
2. Install **NimBLE-Arduino** from the Library Manager. The sketches use the 2.x API, so 1.x won't compile.
3. Flash `pedal_server` onto the guitar unit and `pedal_client` onto the pedal.

The two find each other by the service UUID and device name (`"Pedal"`). If you build more than one for some reason lol, or want to avoid interference with someone else's (since everyone and their mom will have one soon), change `SERVICE_UUID` / `DEVICE_NAME` and the characteristic UUIDs to match in **both** sketches.
