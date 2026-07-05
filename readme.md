# Simple GB Game Cartridge Explorer

A simple GameBoy / GameBoy Color / MegaDuck / CougarBoy game cartridge explorer and programmer built around the venerable **Arduino Mega 2560**.

This project provides both an interactive serial monitor for manual hardware control and a standalone SD-card programming mode for quickly programming ROM images.

**Copyright © 2026 NR RetroWorks**  
**License:** MIT

---

# Features

- Direct cartridge bus control and status analyzer
- ROM/RAM erasing, programming-from/dumping-to SD card, and verification
- RAM blanking (useful for FRAM chips)
- Interactive serial command interpreter (when Serial Monitor attached)
- Automatic standalone programming of `/rom.bin` from SD card (when no Serial Monitor attached), with LED status feedback
- Supports banked Game Boy cartridges (original MBC and compatible, including MBC5)

---

# Pin Setup

Address and data buses are mapped directly to the ATMega2560's GPIO pins for straightforward wiring and debugging. Adapt as needed, all values can be easily accessed at the top of the explorer sketch.

## Address Bus

| Signal | Default Arduino Pin (ATMega2560) |
|---------|------------:|
| A0  | 22 |
| A1  | 24 |
| A2  | 26 |
| A3  | 28 |
| A4  | 30 |
| A5  | 32 |
| A6  | 34 |
| A7  | 36 |
| A8  | 38 |
| A9  | 40 |
| A10 | 42 |
| A11 | 44 |
| A12 | 46 |
| A13 | 48 |
| A14 | 39 |
| A15 | 41 |

---

## Data Bus

| Signal | Default Arduino Pin (ATMega2560) |
|---------|------------:|
| D0 | 23 |
| D1 | 25 |
| D2 | 27 |
| D3 | 29 |
| D4 | 31 |
| D5 | 33 |
| D6 | 35 |
| D7 | 37 |

---

## Control Signals

| Signal | Default Arduino Pin (ATMega2560) | Description |
|---------|------------:|-------------|
| `CLK` | 43 | Bus clock / register synchronization |
| `/WR` | 45 | Write strobe / flash synchronization |
| `/RD` | 47 | Read strobe / flash synchronization |
| `/CS` | 49 | RAM chip select override |
| `RST` | 2 | Cartridge reset |
| `SD_CS` | 53 | SD card chip select |
| `LED` | 13 | Status LED |

---

## Notes

- `CLK` is active on the **falling edge** due to cartridge being a peripheral device.
- `/WR` and `/RD` are active-low control signals.
- `/CS` is seemingly used only for overriding external RAM chip select during RAM operations.
- Pin 53 is reserved for the SD card chip select while using an SD card module.

# Operating Modes

## Interactive Mode

When a serial terminal is connected, the explorer enters an interactive command interpreter. Commands can be typed into the monitor to expose/affect hardware state.

This mode allows complete manual control over the cartridge bus, ROM, and RAM (if installed).

## Standalone Mode

If no serial monitor is connected, the explorer attempts to automatically program the cartridge using:

```
/rom.bin
```

located on the SD card.

LED status indicates programming progress and completion.

---

# Command Reference

## Basic Commands

| Command | Description |
|----------|-------------|
| `HELP` | Display help menu |
| `STATUS` | Display current device status |
| `RESET` | Reset attached cartridge/device |

---

## Pin Commands

| Command | Description |
|----------|-------------|
| `CLK <0\|1>` | Assert/deassert CLK (falling edge synchronizes registers) |
| `WR <0\|1>` | Assert/deassert `/WE` |
| `RD <0\|1>` | Assert/deassert `/RD` |
| `CS <0\|1>` | Assert/deassert `/CS` (RAM override only?) |
| `DEASSERT` | Deassert all control lines |
| `ADDR` | Display current address bus |
| `ADDR <addr>` | Set address bus |
| `DATA` | Display current data bus |
| `DATA <data>` | Drive data bus (forces output mode) |

---

## Bus Commands

| Command | Description |
|----------|-------------|
| `READ <addr>` | Read byte from address |
| `WRITE <addr> <data>` | Write byte to address |

---

## ROM Commands

| Command | Description |
|----------|-------------|
| `BANK <hex>` | Selects ROM bank |
| `SIZE <bytes>` | Sets ROM size used for dumping |
| `ID` | Read ROM Manufacturer / Device ID |
| `ERASE CHIP` | Erase entire ROM |
| `ERASE <sector>` | Erase ROM sector |
| `PROG <addr> <data>` | Program one byte |
| `PROG <file>` | Program ROM from SD card |
| `DUMP <file>` | Dump ROM to SD card |

---

## RAM Commands

| Command | Description |
|----------|-------------|
| `RAM <ON\|OFF>` | Enable or disable cartridge RAM |
| `RAMBANK <hex>` | Selects RAM bank |
| `RAMSIZE <bytes>` | Sets RAM size used for dumps/blanks |
| `RAMBLANK` | Fills cartridge RAM with `0x00` |
| `RAMPROG <file>` | Program RAM from SD card |
| `RAMDUMP <file>` | Dump RAM to SD card |

---

# Address Map

| Region | Address |
|---------|---------|
| ROM Bank 0 | `0000h - 3FFFh` (fixed) |
| ROM Bank 1/X | `4000h - 7FFFh` (banked) |
| External RAM | `A000h - BFFFh` (banked) |

---

# LED Status

| State | Meaning |
|-------|---------|
| Solid On | Programming "/rom.bin" completed (standalone mode) |
| Fast Blink | Failure programming "/rom.bin" (standalone mode) |
| Very Fast Blink | Programming/erasing chip activity |
| Slow Blink | Ready for commands (interactive mode) |
| Solid Off | Not yet initialized |

---

# ROM Size Shortcuts

The following shorthand values may be used with the `SIZE` command.

| Value | Size |
|------:|------|
| `256` | 256 Kbit |
| `512` | 512 Kbit |
| `1` | 1 Mbit |
| `2` | 2 Mbit |
| `4` | 4 Mbit |
| `8` | 8 Mbit |
| `16` | 16 Mbit |
| `32` | 32 Mbit |
| `64` | 64 Mbit |
| `128` | 128 Mbit |

---

# RAM Size Shortcuts

The following shorthand values may be used with the `RAMSIZE` command.

| Value | Size |
|------:|------|
| `2` | 2 Kbit |
| `4` | 4 Kbit |
| `8` | 8 Kbit |
| `16` | 16 Kbit |
| `32` | 32 Kbit |
| `64` | 64 Kbit |
| `128` | 128 Kbit |
| `256` | 256 Kbit |
| `512` | 512 Kbit |
| `1` | 1 Mbit |

---

# Notes

- ROM banking follows the standard Game Boy memory map.
  - Fixed ROM bank 0 occupies `0000h-3FFFh`.
  - Switchable ROM bank occupies `4000h-7FFFh`.
  - Switchable RAM bank occupies `A000h-BFFFh`.
- Bank translation is handled by their respective mappers.
- The command interpreter intentionally exposes low-level bus control for development and debugging.
- Uses AM/SST style Flash unlock sequence (`5555h`/`2AAAh`) - may need adjusted to match your ROM chip.