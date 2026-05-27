# VERA PBI Specification and System Integration

---

## 1. Overview
The VERA (Video Enhanced Retro Adapter) PBI integration provides high-resolution video capabilities to the Atari 8-bit computer series by bypassing the constraints of the original ANTIC and GTIA controllers. This document details the architectural specifications, the Parallel Bus Interface (PBI) protocol, and the requirements for the PBI System Handler ROMs.

---

## 2. The Atari Parallel Bus Interface (PBI)
The PBI allows external devices to become part of the Atari system, sharing the address and data buses. Key signals involved:

*   **EXSEL (External Select):** Triggered by the device, informing the CPU that the bus cycle is directed toward the external device instead of internal hardware or memory.
*   **MPD (Math Pack Disable):** Disables the internal floating-point ROM, allowing the external device to map its own handlers into the `$D800-$DFFF` address space.
*   **Device Identification:** During cold boot, the system scans the PBI chain. Each device MUST provide a valid header at its ROM entry point to be detected.

---

## 3. PBI System Handler ROMs and Initialization
The Atari system requires specialized firmware to correctly integrate PBI peripherals. 

### The Mandatory Handler ROM
Each peripheral attached to the PBI must have an associated ROM handler mapped into the `$D800` space. This ROM is essential for:
1.  **System Detection:** The Atari OS scans this address space during the boot sequence to identify available devices.
2.  **Vector Registration:** The ROM contains the `INIT` vectors required by the OS to initialize the hardware, register device handlers in the `HATABS` (Handler Address Table), and configure the PBI mapping.

> **CRITICAL:** If a PBI peripheral is connected but its required handler ROM is missing, corrupted, or incompatible, the Atari OS scanning routine will fail to register the device properly. This often results in a boot hang, incorrect system configuration, or the peripheral remaining entirely invisible to the software.

### Example: PBI Header Structure
The PBI handler must start with a standard header for the Atari OS to recognize it:

```asm
; PBI Handler Header example
    .byte $40           ; PBI ID marker
    .byte $80           ; Device ID bit mask
    .word PBI_INIT      ; Pointer to Initialization routine
    .word PBI_POLL      ; Pointer to Polling routine
    .word PBI_NMI       ; Pointer to NMI routine
```

---

## 4. Vera Architecture Integration
The VERA chip exposes its functionality through a register window mapped to the PBI address space. On the Atari, these are mapped to `$D100-$D11F`.

### Accessing VERA Registers
The communication involves manipulating the register bank base (`$D100`).

```c
/* Register Access Example */
#define VERA_BASE_ADDR 0xD100
#define VERA_CTRL_OFFSET 0x05

/* Function to set a VERA register */
void vera_write_reg(unsigned char offset, unsigned char value)
{
    unsigned int reg_addr = VERA_BASE_ADDR + offset;
    *(volatile unsigned char *)(reg_addr) = value;
}
```

---

## 5. Verification and Test Bench
To ensure stability and compatibility, the project includes a comprehensive suite of tests. Before deployment, all modifications must pass these benchmarks.

### Test Structure
Tests are located in `vera_pbi_rom/vera-tests/`. Each test is designed to probe specific functional areas of the VERA hardware (e.g., raster interrupts, VRAM access, SPI/SD interface).

### Integration Test Workflow
The test suite utilizes the `vera_detect.h` helper to ensure the hardware is initialized before execution.

```c
#include "vera_detect.h"

int main(void)
{
    /* Require VERA presence; halts if not found */
    unsigned int id = vera_require();
    
    /* Proceed with tests */
    printf("VERA ID 0x%04X successfully detected.
", id);
    run_video_mode_test();
    return 0;
}
```

### Running the Test Bench
The build system orchestrates the compilation of tests into `.COM` binaries and packages them into a single `vera_pbi.atr` disk image, enabling automated validation within the emulator.

---

## 6. Build and Configuration
The project uses a `Makefile` to generate the firmware and drivers.

### Resolution Modes
Three primary display modes are supported:
1.  **80x30 (Default):** 640x480 VGA, 8x16 font.
2.  **80x60:** 640x480 VGA, 8x8 font.
3.  **40x30:** 320x240 (upscaled to 640x480 via 2x hardware scaling), 8x8 font.

### Build Commands
To build the PBI ROM and all driver variants:
```bash
make -C vera_pbi_rom cleanall all atr
```

This generates:
*   `vera_pbi_handler.rom`: The PBI handler (always initializes in 80x60 mode).
*   `VERA8030.SYS`: 80x30 driver.
*   `VERA8060.SYS`: 80x60 driver.
*   `VERA4030.SYS`: 40x30 driver.
*   `vera_pbi.atr`: Disk image containing all drivers and test utilities.

Individual variants can be built using the `SCREEN` variable:
```bash
make SCREEN=40x30
```

---
*Technical Documentation - Project Fork Atari800*
