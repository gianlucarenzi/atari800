/*
 * pbi_verax16.c - VeraX16 FPGA video card emulation for Atari 8-bit PBI
 *
 * Emulates a VERA (Video Enhanced Retro Adapter) chip from Commander X16,
 * interfaced as an Atari Parallel Bus Interface (PBI) device.
 *
 * The card asserts EXSEL (External Select) and MPD (Math Pack Disable) when
 * active, mapping VERA registers to $D100-$D11F and the OS handler ROM to
 * $D800-$DFFF, displacing the Atari floating-point math package.
 *
 * Register base $D100 (PBI_ADDR in the ca65 driver source).
 * CC65 / Commander X16 drivers need only change their base constant from
 * $9F20 to $D100 to target the PBI card.
 *
 * VERA register map at $D100-$D11F (offsets relative to $D100):
 *   0x00 ADDR_L      VRAM address bits  7:0 (active address port)
 *   0x01 ADDR_M      VRAM address bits 15:8
 *   0x02 ADDR_H      bit[0]=A16; bits[7:4]=auto-increment selector
 *   0x03 DATA0       VRAM access via address port 0 (auto-advances)
 *   0x04 DATA1       VRAM access via address port 1 (auto-advances)
 *   0x05 CTRL        bit[0]=ADDRSEL, bit[1]=DCSEL, bit[7]=RESET
 *   0x06 IEN         interrupt enable (bits: VSYNC RASTER SPRCOL AFLOW)
 *   0x07 ISR         interrupt status (R); write 1 to clear bits (W)
 *   0x08 IRQLINE_L   raster interrupt scanline, bits 7:0
 *
 *   DCSEL-muxed (offsets 0x09-0x0C, bit[1] of CTRL selects bank):
 *     DCSEL=0: DC_VIDEO, DC_HSCALE, DC_VSCALE, DC_BORDER
 *     DCSEL=1: DC_HSTART, DC_HSTOP,  DC_VSTART, DC_VSTOP
 *
 *   0x0D-0x13  Layer 0 registers (L0_CONFIG .. L0_VSCROLL_H, fixed)
 *   0x14  L1_CONFIG       Layer 1 config
 *   0x15  L1_MAPBASE      Layer 1 map base address
 *   0x16  L1_TILEBASE     Layer 1 tile base address
 *   0x17  L1_HSCROLL_L    Layer 1 horizontal scroll, bits 7:0
 *   0x18  L1_HSCROLL_H    Layer 1 horizontal scroll, bit  8
 *   0x19  L1_VSCROLL_L    Layer 1 vertical scroll, bits 7:0
 *   0x1A  L1_VSCROLL_H    Layer 1 vertical scroll, bit  8
 *   0x1B  AUDIO_CTRL      PCM audio control
 *   0x1C  AUDIO_RATE      PCM sample rate
 *   0x1D  AUDIO_DATA      PCM FIFO write port (write-only)
 *   0x1E  SPI_DATA        SPI data (stub)
 *   0x1F  SPI_CTRL        SPI control (stub)
 *
 * VERA VRAM layout (128 KB, $00000-$1FFFF):
 *   $00000-$1F9BF : tile/bitmap/map data
 *   $1F9C0-$1F9FF : PSG voice registers (16 voices × 4 bytes)
 *   $1FA00-$1FBFF : palette (256 entries × 2 bytes, 12-bit RGB)
 *   $1FC00-$1FFFF : sprite attributes (128 sprites × 8 bytes)
 *
 * Default PBI device ID bit: 7 (mask=$80), matching the ROM header at $D803.
 *
 * Copyright (C) 2024-2026 Gianluca Renzi <gianlucarenzi@eurek.it>
 * Copyright (C) 2002-2026 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "atari.h"
#include "pbi.h"
#include "pbi_verax16.h"
#include "vera_video.h"
#include "util.h"
#include "log.h"
#include "memory.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

/* PBI device bit number (0-7) and mask (1 << num).
 * Default bit 7 (mask=$80) matches the device ID byte in the ROM at $D803. */
static int   verax16_pbi_num  = 7;
static UBYTE verax16_pbi_mask = 0x80;

/* OS handler ROM (2 KB, copied to $D800-$DFFF when device is selected) */
static UBYTE *verax16_rom               = NULL;
static char   verax16_rom_filename[FILENAME_MAX] = "";

int PBI_VERAX16_enabled = FALSE;
static int verax16_cs    = FALSE;  /* chip select: TRUE after $D1FF written with device mask */

/* ------------------------------------------------------------------ */
/* VERA chip constants                                                  */
/* ------------------------------------------------------------------ */

#define VERA_VRAM_SIZE   0x20000u   /* 128 KB */

/* VERA register window: $D100-$D11F (32 registers, offset 0x00-0x1F).
 * PBI_ADDR constant in ca65 source = $D100.                          */
#define VERA_REG_BASE    0xD100u
#define VERA_REG_COUNT   32u

/* VERA ADDR_H auto-increment step lookup (index = ADDR_H bits[7:4]) */
static const int vera_step_lut[16] = {
    0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 40, 80, 160, 320, 640
};

/* ------------------------------------------------------------------ */
/* VERA chip state                                                      */
/* ------------------------------------------------------------------ */

/* 128 KB VRAM — not cleared on soft chip reset, only on power-on */
static UBYTE vera_vram[VERA_VRAM_SIZE];

/* Two independent address ports.
 * ADDRx registers address the port selected by CTRL.ADDRSEL.
 * DATA0 always uses port 0; DATA1 always uses port 1.              */
static UBYTE vera_addr_l[2];    /* VRAM address bits  7:0  */
static UBYTE vera_addr_m[2];    /* VRAM address bits 15:8  */
static UBYTE vera_addr_h[2];    /* bit[0]=A16; bits[7:4]=INCR index */

/* CTRL: bit[0]=ADDRSEL, bit[1]=DCSEL, bit[7]=RESET (self-clearing) */
static UBYTE vera_ctrl    = 0;
static UBYTE vera_ien     = 0;   /* interrupt enable  */
static UBYTE vera_isr     = 0;   /* interrupt status  */
static UBYTE vera_irqline = 0;   /* raster IRQ line (bits 7:0) */

/* DCSEL-muxed registers at offsets 0x09-0x0C (4 regs × 2 banks):
 *   vera_dc[0]: DC_VIDEO, DC_HSCALE, DC_VSCALE, DC_BORDER  (DCSEL=0)
 *   vera_dc[1]: DC_HSTART, DC_HSTOP, DC_VSTART, DC_VSTOP   (DCSEL=1) */
static UBYTE vera_dc[2][4];

/* Layer 0 — fixed at offsets 0x0D-0x13 (7 registers) */
static UBYTE vera_l0[7];        /* CONFIG, MAPBASE, TILEBASE, HSCROLL_L/H, VSCROLL_L/H */

/* Layer 1 — fixed at offsets 0x14-0x1A (7 registers) */
static UBYTE vera_l1[7];        /* CONFIG, MAPBASE, TILEBASE, HSCROLL_L/H, VSCROLL_L/H */

/* Audio */
static UBYTE vera_audio_ctrl = 0;
static UBYTE vera_audio_rate = 0;

/* SPI (stub — not used on the PBI card variant) */
static UBYTE vera_spi_data = 0xFF;
static UBYTE vera_spi_ctrl = 0;

#ifdef PBI_DEBUG
#define D(a) a
#else
#define D(a) do{}while(0)
#endif

/* ------------------------------------------------------------------ */
/* VERA internal helpers                                                */
/* ------------------------------------------------------------------ */

/* Reconstruct full 17-bit VRAM address from the three address bytes */
#define VERA_FULL_ADDR(p) \
    (((ULONG)(vera_addr_h[(p)] & 0x01u) << 16) | \
     ((ULONG)vera_addr_m[(p)] << 8) | (ULONG)vera_addr_l[(p)])

/* Advance address port p by its configured step after a DATA access */
static void vera_advance(int p)
{
    ULONG addr = VERA_FULL_ADDR(p);
    ULONG step = (ULONG)vera_step_lut[vera_addr_h[p] >> 4];
    if (step != 0u) {
        addr = (addr + step) & 0x1FFFFu;
        vera_addr_l[p] = (UBYTE)(addr & 0xFFu);
        vera_addr_m[p] = (UBYTE)((addr >> 8) & 0xFFu);
        vera_addr_h[p] = (vera_addr_h[p] & 0xF0u) | (UBYTE)((addr >> 16) & 0x01u);
    }
}

/* Soft reset: registers to defaults, VRAM contents preserved */
static void vera_chip_reset(void)
{
    memset(vera_addr_l, 0, sizeof(vera_addr_l));
    memset(vera_addr_m, 0, sizeof(vera_addr_m));
    memset(vera_addr_h, 0, sizeof(vera_addr_h));
    vera_ctrl     = 0;
    vera_ien      = 0;
    vera_isr      = 0;
    vera_irqline  = 0;

    /* DC defaults (DCSEL=0 bank) */
    memset(vera_dc, 0, sizeof(vera_dc));
    vera_dc[0][1] = 128;    /* DC_HSCALE = 128 (1:1) */
    vera_dc[0][2] = 128;    /* DC_VSCALE = 128 (1:1) */
    /* DC secondary defaults (DCSEL=1 bank) */
    vera_dc[1][1] = 160;    /* DC_HSTOP  = 160 (640 / 4) */
    vera_dc[1][3] = 240;    /* DC_VSTOP  = 240 (480 / 2) */

    memset(vera_l0, 0, sizeof(vera_l0));
    memset(vera_l1, 0, sizeof(vera_l1));

    vera_audio_ctrl = 0;
    vera_audio_rate = 0;
    vera_spi_data   = 0xFF;
    vera_spi_ctrl   = 0;

    PBI_IRQ &= ~verax16_pbi_mask;

    Log_print("VeraX16: VERA chip soft-reset");
}

/* ------------------------------------------------------------------ */
/* VERA register read/write                                             */
/* ------------------------------------------------------------------ */

static UBYTE vera_read_reg(int offset, int no_side_effects)
{
    int addrsel = vera_ctrl & 0x01;
    int dcsel   = (vera_ctrl >> 1) & 0x01;     /* bit 1 only */

    switch (offset) {
    case 0x00:
        return vera_addr_l[addrsel];
    case 0x01:
        return vera_addr_m[addrsel];
    case 0x02:
        return vera_addr_h[addrsel];
    case 0x03:  /* DATA0 — VRAM read through port 0 */
        {
            ULONG a   = VERA_FULL_ADDR(0);
            UBYTE val = vera_vram[a];
            if (!no_side_effects) {
                vera_advance(0);
                D(printf("VeraX16: VRAM[%05lx] -> %02x\n", (unsigned long)a, val));
            }
            return val;
        }
    case 0x04:  /* DATA1 — VRAM read through port 1 */
        {
            ULONG a   = VERA_FULL_ADDR(1);
            UBYTE val = vera_vram[a];
            if (!no_side_effects)
                vera_advance(1);
            return val;
        }
    case 0x05:  /* CTRL — RESET bit always reads 0 */
        return vera_ctrl & 0x7Fu;
    case 0x06:
        return vera_ien;
    case 0x07:
        return vera_isr;
    case 0x08:
        return vera_irqline;
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C)
            return vera_dc[dcsel][offset - 0x09];
        /* Layer 0 fixed: offsets 0x0D-0x13 */
        if (offset >= 0x0D && offset <= 0x13)
            return vera_l0[offset - 0x0D];
        /* Layer 1 fixed: offsets 0x14-0x1A */
        if (offset >= 0x14 && offset <= 0x1A)
            return vera_l1[offset - 0x14];
        if (offset == 0x1B) return vera_audio_ctrl;
        if (offset == 0x1C) return vera_audio_rate;
        if (offset == 0x1D) return 0x00;    /* AUDIO_DATA write-only */
        if (offset == 0x1E) return vera_spi_data;
        if (offset == 0x1F) return vera_spi_ctrl;
        return 0xFF;
    }
}

static void vera_write_reg(int offset, UBYTE byte)
{
    int addrsel = vera_ctrl & 0x01;
    int dcsel   = (vera_ctrl >> 1) & 0x01;

    switch (offset) {
    case 0x00:
        vera_addr_l[addrsel] = byte;
        break;
    case 0x01:
        vera_addr_m[addrsel] = byte;
        break;
    case 0x02:
        vera_addr_h[addrsel] = byte;
        break;
    case 0x03:  /* DATA0 — VRAM write through port 0 */
        {
            ULONG a = VERA_FULL_ADDR(0);
            D(printf("VeraX16: VRAM[%05lx] <- %02x\n", (unsigned long)a, byte));
            vera_vram[a] = byte;
            vera_advance(0);
        }
        break;
    case 0x04:  /* DATA1 — VRAM write through port 1 */
        vera_vram[VERA_FULL_ADDR(1)] = byte;
        vera_advance(1);
        break;
    case 0x05:  /* CTRL */
        if (byte & 0x80u) {
            vera_chip_reset();      /* RESET bit: soft-reset, VRAM intact */
        } else {
            vera_ctrl = byte & 0x03u;   /* keep ADDRSEL + DCSEL */
        }
        break;
    case 0x06:
        vera_ien = byte;
        break;
    case 0x07:  /* ISR — writing 1 clears corresponding status bit */
        vera_isr &= ~byte;
        if (!(vera_isr & vera_ien))
            PBI_IRQ &= ~verax16_pbi_mask;
        break;
    case 0x08:
        vera_irqline = byte;
        break;
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C) {
            vera_dc[dcsel][offset - 0x09] = byte;
        }
        /* Layer 0 fixed: offsets 0x0D-0x13 */
        else if (offset >= 0x0D && offset <= 0x13) {
            vera_l0[offset - 0x0D] = byte;
        }
        /* Layer 1 fixed: offsets 0x14-0x1A */
        else if (offset >= 0x14 && offset <= 0x1A) {
            vera_l1[offset - 0x14] = byte;
        }
        else if (offset == 0x1B) { vera_audio_ctrl = byte; }
        else if (offset == 0x1C) { vera_audio_rate = byte; }
        else if (offset == 0x1D) { /* AUDIO_DATA: PCM FIFO — stub */ }
        else if (offset == 0x1E) { vera_spi_data = byte; }
        else if (offset == 0x1F) { vera_spi_ctrl = byte; }
        break;
    }
}

/* ------------------------------------------------------------------ */
/* PBI device interface                                                 */
/* ------------------------------------------------------------------ */

int PBI_VERAX16_Initialise(int *argc, char *argv[])
{
    int i, j;
    int recognized;

    for (i = j = 1; i < *argc; i++) {
        recognized = FALSE;

        if (strcmp(argv[i], "-verax16") == 0 ||
            strcmp(argv[i], "--use-verax16") == 0) {
            PBI_VERAX16_enabled = TRUE;
            recognized = TRUE;
        } else if (strcmp(argv[i], "-verax16-rom") == 0 && i + 1 < *argc) {
            Util_strlcpy(verax16_rom_filename, argv[i + 1],
                         sizeof(verax16_rom_filename));
            i++;
            recognized = TRUE;
        } else if (strcmp(argv[i], "-verax16-pbi-id") == 0 && i + 1 < *argc) {
            int n = atoi(argv[i + 1]);
            if (n >= 0 && n <= 7) {
                verax16_pbi_num  = n;
                verax16_pbi_mask = (UBYTE)(1u << n);
            } else {
                Log_print("VeraX16: invalid PBI ID %d, keeping default %d",
                          n, verax16_pbi_num);
            }
            i++;
            recognized = TRUE;
        }

        if (!recognized) {
            if (strcmp(argv[i], "-help") == 0) {
                Log_print("\t-verax16           Enable VeraX16 FPGA PBI video card");
                Log_print("\t--use-verax16      Alias for -verax16");
                Log_print("\t-verax16-rom F     OS handler ROM (2KB, $D800-$DFFF)");
                Log_print("\t-verax16-pbi-id N  PBI device bit 0-7 (default: 7, mask=$80)");
            }
            argv[j++] = argv[i];
        }
    }
    *argc = j;

    if (!PBI_VERAX16_enabled)
        return TRUE;

    /* Power-on: clear VRAM and reset all registers */
    memset(vera_vram, 0, sizeof(vera_vram));

    /* Pre-load the standard Commander X16 default 16-colour palette.
     * Format: byte0 = GGGGBBBB, byte1 = 0000RRRR  (12-bit colour) */
    {
        static const UBYTE pal16[32] = {
            0x00, 0x00,   /* 0: Black    #000 */
            0xFF, 0x0F,   /* 1: White    #FFF */
            0x00, 0x08,   /* 2: Red      #800 */
            0xFE, 0x0A,   /* 3: Cyan     #AFE */
            0x4C, 0x0C,   /* 4: Purple   #C4C */
            0xC5, 0x00,   /* 5: Green    #0C5 */
            0x0A, 0x00,   /* 6: Blue     #00A */
            0xE7, 0x0E,   /* 7: Yellow   #EE7 */
            0x85, 0x0D,   /* 8: Orange   #D85 */
            0x40, 0x06,   /* 9: Brown    #640 */
            0x77, 0x0F,   /* A: LtRed    #F77 */
            0x33, 0x03,   /* B: DkGray   #333 */
            0x77, 0x07,   /* C: MdGray   #777 */
            0xF6, 0x0A,   /* D: LtGreen  #AF6 */
            0x8F, 0x00,   /* E: LtBlue   #08F */
            0xBB, 0x0B,   /* F: LtGray   #BBB */
        };
        memcpy(vera_vram + 0x1FA00u, pal16, sizeof(pal16));
    }

    vera_chip_reset();

    /* Optionally load OS handler ROM */
    if (verax16_rom_filename[0] != '\0') {
        verax16_rom = (UBYTE *)Util_malloc(0x800);
        if (!Atari800_LoadImage(verax16_rom_filename, verax16_rom, 0x800)) {
            free(verax16_rom);
            verax16_rom = NULL;
            Log_print("VeraX16: WARNING - ROM not loaded (no $D800 handler)");
        } else {
            Log_print("VeraX16: ROM loaded from %s", verax16_rom_filename);
        }
    }

    Log_print("VeraX16: PBI video card enabled  device-bit=%d mask=$%02X regs=$D100-$D11F VRAM=128KB",
              verax16_pbi_num, verax16_pbi_mask);
    return TRUE;
}

void PBI_VERAX16_Exit(void)
{
    if (!PBI_VERAX16_enabled)
        return;
    VERA_VIDEO_Exit();
    if (verax16_rom != NULL) {
        free(verax16_rom);
        verax16_rom = NULL;
    }
    PBI_VERAX16_enabled = FALSE;
}

void PBI_VERAX16_Reset(void)
{
    if (PBI_VERAX16_enabled) {
        verax16_cs = FALSE;
        vera_chip_reset();
    }
}

int PBI_VERAX16_ReadConfig(char *string, char *ptr)
{
    if (strcmp(string, "VERAX16_ROM") == 0)
        Util_strlcpy(verax16_rom_filename, ptr, sizeof(verax16_rom_filename));
    else if (strcmp(string, "VERAX16_PBI_ID") == 0) {
        int n = atoi(ptr);
        if (n >= 0 && n <= 7) {
            verax16_pbi_num  = n;
            verax16_pbi_mask = (UBYTE)(1u << n);
        }
    }
    else return FALSE;
    return TRUE;
}

void PBI_VERAX16_WriteConfig(FILE *fp)
{
    fprintf(fp, "VERAX16_ROM=%s\n", verax16_rom_filename);
    fprintf(fp, "VERAX16_PBI_ID=%d\n", verax16_pbi_num);
}

/* ------------------------------------------------------------------ */
/* PBI bus read ($D100-$D1FE)                                          */
/* ------------------------------------------------------------------ */

int PBI_VERAX16_D1GetByte(UWORD addr, int no_side_effects)
{
    if (!verax16_cs)
        return PBI_NOT_HANDLED;
    return (int)vera_read_reg((int)addr & 0x1F, no_side_effects);
}

/* ------------------------------------------------------------------ */
/* PBI bus write ($D100-$D1FE, $D1FF handled separately)               */
/* ------------------------------------------------------------------ */

void PBI_VERAX16_D1PutByte(UWORD addr, UBYTE byte)
{
    if (!verax16_cs)
        return;
    vera_write_reg((int)addr & 0x1F, byte);
}

/* ------------------------------------------------------------------ */
/* $D1FF latch: map/unmap the OS handler ROM at $D800-$DFFF            */
/* When our device is selected: MPD active, EXSEL active, ROM visible. */
/* ------------------------------------------------------------------ */

int PBI_VERAX16_D1ffPutByte(UBYTE byte)
{
    if (!PBI_VERAX16_enabled)
        return PBI_NOT_HANDLED;

    if (byte == verax16_pbi_mask) {
        verax16_cs = TRUE;
        if (verax16_rom != NULL) {
            memcpy(MEMORY_mem + 0xd800, verax16_rom, 0x800);
            Log_print("VeraX16: selected (CS=1), ROM mapped at $D800");
        }
        return 0;
    }

    /* Any other D1FF value deselects this device */
    verax16_cs = FALSE;
    return PBI_NOT_HANDLED;
}

/* Returns this device's IRQ status bit for the $D1FF read */
int PBI_VERAX16_D1ffGetByte(void)
{
    return (PBI_IRQ & verax16_pbi_mask) ? verax16_pbi_mask : 0;
}

/* ------------------------------------------------------------------ */
/* VSYNC IRQ generation — call once per display frame                  */
/* ------------------------------------------------------------------ */

void PBI_VERAX16_VSync(void)
{
    if (!PBI_VERAX16_enabled)
        return;
    if (vera_ien & 0x01u) {     /* VSYNC interrupt enabled */
        vera_isr |= 0x01u;
        PBI_IRQ  |= verax16_pbi_mask;
        D(printf("VeraX16: VSYNC IRQ\n"));
    }
}

/* ------------------------------------------------------------------ */
/* Direct VRAM access for state inspection / save-state                */
/* ------------------------------------------------------------------ */

UBYTE PBI_VERAX16_GetVRAM(ULONG addr)
{
    return vera_vram[addr & 0x1FFFFu];
}

void PBI_VERAX16_PutVRAM(ULONG addr, UBYTE byte)
{
    vera_vram[addr & 0x1FFFFu] = byte;
}

/* ------------------------------------------------------------------ */
/* Register snapshot + VRAM pointer for the SDL video renderer         */
/* ------------------------------------------------------------------ */

void PBI_VERAX16_GetRegSnap(VERA_RegSnap *s)
{
    memcpy(s->dc0, vera_dc[0], 4);
    memcpy(s->dc1, vera_dc[1], 4);
    memcpy(s->l0,  vera_l0,    7);
    memcpy(s->l1,  vera_l1,    7);
}

const UBYTE *PBI_VERAX16_GetVRAMPtr(void)
{
    return vera_vram;
}

/*
vim:ts=4:sw=4:
*/
