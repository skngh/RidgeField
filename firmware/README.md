# Firmware

- [`daisy/`](daisy/): the audio side. Runs on the Daisy Seed inside the pedal and does all the effects.
- [`ESP32C3/`](ESP32C3/): the Bluetooth side. One sketch for the guitar mounted box (server), one for the pedal (client).

The client ESP32 forwards control data to the Daisy over UART at 115200 baud as a 14-byte frame. The frame format is documented at the top of `pedal_client.ino` and in `daisy/bt_link.h`.
