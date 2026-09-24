# Daisy firmware

The DSP for the pedal, running on a Daisy Seed.

- `BTPedal.cpp`: main program, with the effects chain, controls, bypass, LED and LDR calibration
- `bt_link.cpp` / `bt_link.h`: receives control data from the ESP32 over UART (USART1, DMA)
- `Simple-DSP/`: my DSP library (git submodule), where the reverb, bitcrusher, pitch shifter, delay and distortion come from
- `Moorer Reverb Presentation.pdf`: this is the reverb algorithm I used

Signal chain: delay → pitch shift → bitcrusher → reverb (with soft clip) → dry/wet mix.

## Before you build: set your libDaisy / DaisySP paths

The Makefile expects libDaisy and DaisySP to sit two folders up from this one:

```make
LIBDAISY_DIR = ../../libDaisy/
DAISYSP_DIR = ../../DaisySP/
```

**You must either change those two lines to point to where you have [libDaisy](https://github.com/electro-smith/libDaisy) and [DaisySP](https://github.com/electro-smith/DaisySP), or simply copy this folder to something like a MyProjects folder in your libDaisy folder.** If you end up changing the path then you'll wanna change the paths in `c_cpp_properties.json` and `tasks.json` too if you use VS Code.

Also make sure the submodule is pulled, or you'll get missing `Simple-DSP` headers:

```sh
git submodule update --init
```

## Building and flashing

You'll need the Daisy toolchain. Follow Electro-Smith's [getting started guide](https://daisy.audio/tutorials/cpp-dev-env/) if you haven't set it up.

## Pins

| Daisy pin                | Use               |
| ------------------------ | ----------------- |
| D14 / pin 15 (USART1 RX) | from ESP32 TX     |
| D15                      | status LED        |
| D16                      | bypass footswitch |
| A10                      | mix knob          |
