# Detailed Project Description: Atari800 Fork with VERA PBI Device

## Overview
This project is a specialized fork of the Atari800 emulator, extended to support a custom Parallel Bus Interface (PBI) peripheral based on the VERA chipset (commonly found in the Commander X16). The goal is to provide a modern, high-resolution, VERA-based video subsystem for Atari 8-bit computers, acting as the primary display device while bypassing the limitations of the original ANTIC/GTIA graphics.

## Project Architecture

The project consists of two primary components, both residing in the `vera_pbi_rom` directory and tightly integrated with the Atari OS:

### 1. PBI OS Handler (`vera_pbi_handler.rom`)
*   **Role:** Initializes the card and exposes a standard PBI OS interface.
*   **Location:** Mapped to `$D800-$DFFF` when selected via the `$D1FF` PBI latch.
*   **Functionality:**
    *   Implements the standard Atari PBI ROM header (checksum, device ID, JMP vectors).
    *   Handles PBI Initialization (`INIT` handler) at cold/warm starts.
    *   Provides stub routines for CIO operations, enabling the OS to recognize the card as an active peripheral.
    *   Initializes the VERA hardware registers (IRQ, DC settings, etc.) for a 640x480 VGA-compatible mode using Layer 1, tiled map.

### 2. Relocatable OS Driver (`AUTORUN.SYS`)
*   **Role:** Acts as the primary system driver, installed automatically upon boot.
*   **Functionality:**
    *   **Installation:** Hooks into the HATABS (Handler Address Table) to replace the standard Editor (`E:`) and Screen (`S:`) device handlers with VERA-enabled versions.
    *   **PUTC Management:** Replaces the standard CIO PUT BYTE routines with a custom state machine that renders ATASCII text directly to VERA VRAM (80x60 viewport), bypassing original ANTIC/GTIA screen memory.
    *   **VBI Hooks:** Installs Vertical Blank Interrupt routines to manage the cursor blinking and metronome features.
    *   **Warmstart Resilience:** Hooks into the system reset vectors (`DOSINI`/`CASINI` chain) to ensure the driver remains active and the VERA card is re-initialized after a system reset.

## Key Implementation Modules (vera_pbi_rom/*.s)

The following assembly modules form the core of the implementation:

*   **`vera_pbi_handler.s`**: Handles the low-level PBI protocol, ROM header definition, and initial hardware setup during the cold boot sequence.
*   **`vera_driver.s`**: The core PUT BYTE state machine. Implements a 40x24/80x60 ATASCII viewport, handling control characters (EOL, CLEAR, TAB, etc.) and direct VRAM rendering.
*   **`vera_sys_es_hook.s`**: Installs the replacement handlers for E: and S: devices by patching the HATABS and updating cached IOCB PUT BYTE pointers for open devices. Also manages input buffering and raw POKEY keyboard code translation to ATASCII.
*   **`vera_sys_vbi.s`**: Manages the VBI-driven cursor blinker (snapshotting the cursor position and inverting the background/foreground color nibbles) and ensures background tasks do not conflict with foreground VRAM writes.

## Emulator-Side Implementation (Atari800)

The `atari800` emulator core has been extended to support the VERA PBI peripheral. The emulator-side implementation (`src/pbi_verax16.c`, `src/pbi_verax16.h`) handles the hardware emulation of the VERA chip and its integration into the Atari PBI bus.

### Core Emulation Features:
*   **Memory Mapping:** Intercepts access to the `$D100-$D11F` range to handle VERA register read/writes, and manages the mapping of the handler ROM to `$D800-$DFFF` via the PBI device latch (`$D1FF`).
*   **Hardware Emulation:**
    *   **VERA Registers:** Full emulation of VERA registers (Address Ports, Data Ports, CTRL, IEN, ISR) and DC-muxed registers.
    *   **VRAM:** Emulates the 128KB VRAM memory space.
    *   **FX Coprocessor:** Partial emulation of the VERA FX coprocessor for operations like line drawing, polygon filling, and affine transformations.
    *   **Audio/SPI:** Emulation of VERA PSG/PCM audio channels and SPI interface for SD card emulation.
*   **Bus Integration:**
    *   **IRQ Management:** Handles interrupt requests from VERA to the Atari CPU based on IEN/ISR settings.
    *   **Configuration:** Supports CLI arguments to enable the card (`-verax16`), specify the ROM handler image (`-verax16-rom`), and attach an SD card image for the SPI interface (`-verax16-sdcard`).
*   **Lifecycle Management:** Handles power-on/reset states, ensuring that VRAM is initialized and the card is properly enabled/disabled on the bus.

## Known Issues and Fixes

### Cursor Instability and Visual Corruption
During development, a race condition between the VBI interrupt (managing cursor blinking) and screen manipulation routines (`scroll_up`, `do_delete_line`, `do_insert_line`, `do_delete_char`, `do_insert_char`) caused cursor disappearance and intermittent visual corruption.

**Fix:**
1.  **Cursor Invalidation:** Explicitly added calls to `_vera_cursor_invalidate` at the start of all screen manipulation routines to ensure the cursor is erased before VRAM modifications.
2.  **Register Preservation:** Refactored `_vera_cursor_invalidate` to save and restore all CPU registers (`A`, `X`, `Y`) and the `VERA_CTRL` register, ensuring calling routines maintain their state integrity and VERA controller settings.

## Integration Strategy
The driver effectively makes the VERA card the *primary* display device. The original OS PUT BYTE routines are *not* called; instead, the custom driver redirects all text output directly to the VERA's VRAM. By setting the system margins (`LMARGIN`, `RMARGIN`) to 0/79 during OPEN, the driver ensures that Atari OS software sees a standard 80-column device.
