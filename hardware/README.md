# Hardware

Made with **KiCad 10.0**. Older versions won't open these files.

- [`BluetoothPedal_Main/`](BluetoothPedal_Main/): the main pedal board. Includes the Daisy, XIAO ESP32-C3, input/output buffers, DC power, footswitch and mix knob.
- [`BluetoothPedal_Server/`](BluetoothPedal_Server/): the guitar-mounted board. XIAO ESP32-C3, MCP3208 ADC, knobs, LDR, joystick, battery.
- [`libs/`](libs/): custom symbols, footprints and 3D models used by both projects
- [`PCBWayFiles/`](PCBWayFiles/): Gerbers, BOM and pick-and-place (`-all-pos.csv`) for each board. If you just want boards, these are all you need.

## Opening the projects

I think both projects should open and look right without any setup? But if you want to edit or update parts you may need to do these steps below:

- **Footprints:** each project has an `fp-lib-table` pointing to `../libs/footprints/Custom_Footprints.pretty`. This originally was an absolute path that I changed, but I'm not totally positive that works so you may need to change it, idk.
- **Symbols:** `custom_symbols` and `Seeed_Studio_XIAO_Series` aren't registered in a project symbol table. If you need them, add `libs/symbols/*.kicad_sym` under _Preferences → Manage Symbol Libraries → Project Specific Libraries_.
- **3D models:** some models seem to be absolute paths from my computer, so you may need to configure them in each footprint's settings.

None of this affects the Gerbers, those should all work fine.
