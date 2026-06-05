# Atari800 Emulator with VERA X16 PBI Integration

This project is a fork of the **Atari800** emulator (version 5.2.0), extended with support for the **VERA X16** FPGA video card via the Atari **Parallel Bus Interface (PBI)**.

## Project Overview

- **Atari800 Emulator:** A portable, GPL-licensed emulator for Atari 8-bit computers and the 5200 console.
- **VERA X16 Integration:** Emulates the "Video Enhanced Retro Adapter" (VERA) chip from the Commander X16, interfaced as an Atari PBI device.
- **Enhanced Graphics:** Provides high-resolution VGA-compatible modes (e.g., 640x480) and 80-column text support, bypassing the original ANTIC/GTIA limitations.
- **System Drivers:** Includes custom 6502 firmware and OS drivers that hook into the Atari OS to provide enhanced `E:` (Editor), `S:` (Screen), and `K:` (Keyboard) functionality.

## Key Components

### Emulator Core (`src/`)
- `pbi_verax16.c/h`: Core emulation of the VERA chip registers and PBI bus interface.
- `vera_video.c/h`: VERA video rendering logic, integrated with the emulator's display loop.
- `pbi.c`: Updated to dispatch bus cycles to the VERA PBI handler.

### VERA Drivers & ROM (`vera_pbi_rom/`)
- `vera_pbi_handler.s`: The PBI ROM handler that allows the Atari OS to detect and initialize the VERA card.
- `vera_sys_es_hook.s`: Implementation of OS `E:` and `S:` handler hooks for high-res text and graphics.
- `vera_sys_vbi.s` / `vera_sys_loader.s`: System integration for Vertical Blank Interrupts and driver loading.
- `vera_driver.s`: Low-level VERA driver implementation.

## Building and Running

### Building the Emulator
The project uses the standard GNU Autotools build system.

1.  **Generate configuration:**
    ```bash
    ./autogen.sh
    ```
2.  **Configure with VERA support:**
    ```bash
    ./configure --enable-pbi-verax16
    ```
3.  **Compile:**
    ```bash
    make
    ```

### Building the VERA Drivers
The drivers in `vera_pbi_rom` have their own build system (Makefile and CMake).

1.  **Build ROM and drivers:**
    ```bash
    make -C vera_pbi_rom cleanall all atr
    ```
    This produces `vera_pbi_handler.rom` and a disk image `vera_pbi.atr` containing system drivers (`VERA8030.SYS`, etc.).

## Development Conventions

- **Language:** C (C99/ANSI) for the emulator; 6502 Assembly (CA65) for the drivers.
- **Style:** Follow the existing Atari800 coding standards (see `DOC/PORTING`).
- **PBI Integration:** The VERA registers are mapped to `$D100-$D11F`. The PBI handler ROM is mapped to `$D800-$DFFF` when selected via the PBI device latch at `$D1FF`.
- **Testing:** New features should be validated using the test bench in `vera_pbi_rom/vera-tests/`.

## Important Documentation
- `README.TXT`: General emulator information.
- `DOC/README`: Extensive documentation for the base emulator.
- `vera_pbi_rom/README.md`: Detailed specification of the VERA PBI integration and driver structure.
- `vera_pbi_rom/DETAILED-PROJECT.md`: In-depth project documentation.
