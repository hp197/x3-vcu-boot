# x3-vcu-boot

An open, from-source replica of the bootloader on the Segway-Ninebot **x3**-platform
VCU (GT3 / G3 / ZT3 / F3 / E3), which runs on an **Artery AT32F415CBT7**
(ARM Cortex-M4, 128 KB flash, 32 KB SRAM). It occupies the first **4 KB** of flash
(`0x08000000`–`0x08000FFF`), brings up the clock, optionally commits a staged OTA,
then validates and launches the application in slot 0.

It builds with the official **Artery AT32F415 BSP** (BSD-3-Clause) + CMSIS via
PlatformIO, and has been confirmed to boot the application on real GT3 VCU hardware.

## Flash layout

| Region | Range | Purpose |
|--------|-------|---------|
| Bootloader | `0x08000000`–`0x08000FFF` | this loader (4 KB) |
| slot 0 | `0x08001000`–`0x0800FFFF` | active application (vectors at `0x08001000`) |
| slot 1 | `0x08010000`–`0x0801EFFF` | OTA staging (A/B mirror) |
| identity / descriptor | `0x0801F000`–`0x0801FFFF` | identity records + boot descriptor (`0x0801F800`) |

## Boot behaviour

1. Configure the clock (HEXT → PLL → 144 MHz) and flash wait states.
2. Read the boot descriptor. If an OTA is flagged, commit it (erase slot 0, copy
   slot 1 → slot 0), then clear the flag.
3. Validate slot 0 structurally (its initial stack pointer must lie in SRAM) and
   launch it (`__set_MSP` + jump to the reset vector). There is **no** comms/DFU
   receive path in the bootloader — the application receives an OTA image itself and
   stages it into slot 1.
4. If nothing valid can be launched, reset and retry.

## Clock / crystal note

The GT3 VCU board uses a **16 MHz** external crystal (`HEXT`), not the 8 MHz the
Artery demos assume. The PLL is therefore configured as `HEXT/2 × 18 = 144 MHz`
(`CRM_PLL_MULT_18`), and `HEXT_VALUE` is set to `16000000`. Using the demo's
`MULT_36` on a 16 MHz board yields 288 MHz — a 2× overclock past the 150 MHz max,
which hangs the core. Adjust these if your board uses a different crystal.

## Build

PlatformIO downloads its own copy of the Artery platform and BSP, so no local
checkout is needed.

```bash
pio run            # build  -> .pio/build/x3-vcu-boot/firmware.bin (a 4 KB image)
pio run -t clean   # clean
```

Flashing and debugging are intentionally disabled in `platformio.ini`; flash the
resulting image to `0x08000000` over SWD with your own tool (e.g. OpenOCD). The size
check is pinned to the 4 KB bootloader region, so the build fails if the image grows
past it.

## Related

The [`gt3fullflash`](https://github.com/hp197/gt3fullflash) packager embeds this
bootloader when it assembles a full 128 KB flash image from a slot-0 application and
a device identity record.

## License

This repository's original work (the bootloader source, linker script, and build
configuration) is licensed under **CC BY-NC-SA 4.0** — see [LICENSE](LICENSE). The
Artery AT32F415 BSP and CMSIS used at build time keep their own licenses.
