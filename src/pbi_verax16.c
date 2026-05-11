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
 *   0x1E  SPI_DATA        SPI data
 *   0x1F  SPI_CTRL        SPI control
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
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#if defined(HAVE_WINDOWS_H)
#define fseeko fseeko64
#define ftello ftello64
#elif defined(__BEOS__)
#define fseeko _fseek
#define ftello _ftell
#elif defined(__DJGPP__)
#define fseeko fseek
#define ftello ftell
#endif

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
static char   verax16_sdcard_filename[FILENAME_MAX] = "";
static FILE  *verax16_sdcard_file       = NULL;
static off_t  verax16_sdcard_size       = 0;
static int    verax16_sdcard_writable   = FALSE;

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
#define VERA_VERSION_MAJOR 47u
#define VERA_VERSION_MINOR 0u
#define VERA_VERSION_PATCH 2u

/* FX coprocessor modes */
#define FX_MODE_NORMAL     0
#define FX_MODE_LINE_DRAW  1
#define FX_MODE_POLY_FILL  2
#define FX_MODE_AFFINE     3

/* VERA ADDR_H auto-increment step lookup (index = ADDR_H bits[7:3]) */
static const int vera_step_lut[32] = {
    0,   0,
    1,   -1,
    2,   -2,
    4,   -4,
    8,   -8,
    16,  -16,
    32,  -32,
    64,  -64,
    128, -128,
    256, -256,
    512, -512,
    40,  -40,
    80,  -80,
    160, -160,
    320, -320,
    640, -640
};

static const UBYTE vera_version_string[4] = {
    (UBYTE)'V',
    (UBYTE)VERA_VERSION_MAJOR,
    (UBYTE)VERA_VERSION_MINOR,
    (UBYTE)VERA_VERSION_PATCH
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
static UBYTE vera_addr_h[2];    /* bit[0]=A16; bit[1]=nibble; bit[2]=nibble incr; bits[7:3]=INCR index */
static UBYTE vera_rddata[2];    /* DATA port read-ahead latch */

/* CTRL: bit[0]=ADDRSEL, bits[6:1]=DCSEL, bit[7]=RESET (self-clearing) */
static UBYTE vera_ctrl    = 0;
static UBYTE vera_ien     = 0;   /* interrupt enable (bits 3:0) */
static UBYTE vera_isr     = 0;   /* interrupt status  */
static UWORD vera_irqline = 0;   /* raster IRQ line (bits 8:0) */
static UWORD vera_scanline_raw = 0;  /* current raster line before 9-bit clamp */
static UBYTE vera_current_field = 0;
static unsigned int vera_scanline_accum = 0;
static int vera_midline_x_fp = 0;
static UBYTE vera_sprite_frame_collisions = 0;

/* DCSEL-muxed registers at offsets 0x09-0x0C (4 regs × 64 banks):
 *   vera_dc[0]: DC_VIDEO, DC_HSCALE, DC_VSCALE, DC_BORDER  (DCSEL=0)
 *   vera_dc[1]: DC_HSTART, DC_HSTOP, DC_VSTART, DC_VSTOP   (DCSEL=1)
 *   vera_dc[2]: FX_CTRL, FX_TILEBASE, FX_MAPBASE, FX_ACCUM (DCSEL=2)
 *   ... and so on.                                                 */
static UBYTE vera_dc[64][4];

/* FX coprocessor state */
static int32_t fx_pixel_pos_x = 0; /* 11.9 fixed point (20 bits) */
static int32_t fx_pixel_pos_y = 0; /* 11.9 fixed point (20 bits) */
static int16_t fx_pixel_incr_x = 0; /* 6.9 fixed point (15 bits) */
static int16_t fx_pixel_incr_y = 0; /* 6.9 fixed point (15 bits) */
static int fx_pixel_incr_x_times_32 = FALSE;
static int fx_pixel_incr_y_times_32 = FALSE;
static UBYTE fx_addr1_mode = 0;
static int fx_4bit_mode = FALSE;
static int fx_16bit_hop = FALSE;
static int fx_transparency_enabled = FALSE;
static int fx_cache_write_enabled = FALSE;
static int fx_cache_fill_enabled = FALSE;
static int fx_one_byte_cache_cycling = FALSE;
static UBYTE fx_tiledata_base_address = 0;
static UBYTE fx_map_base_address = 0;
static UBYTE fx_map_size = 0;
static int fx_apply_clip = FALSE;
static int fx_2bit_polygon_pixels = FALSE;
static uint32_t ib_cache32 = 0;
static UBYTE fx_cache_byte_index = 0;
static UBYTE fx_cache_nibble_index = 0;
static int fx_cache_increment_mode = 0;
static int fx_mult_enabled = FALSE;
static int fx_add_or_sub = 0;
static int fx_accumulate = FALSE;
static int32_t fx_mult_accumulator = 0;

static int fx_2bit_poke_mode = FALSE;
static UBYTE fx_16bit_hop_start_index = 0;

/* Scratchpad RAM for PBI handler variables ($D120 - $D19F, 128 bytes) */
static UBYTE vera_pbi_scratchpad[0x80];

/* Layer 0 — fixed at offsets 0x0D-0x13 (7 registers) */
static UBYTE vera_l0[7];        /* CONFIG, MAPBASE, TILEBASE, HSCROLL_L/H, VSCROLL_L/H */

/* Layer 1 — fixed at offsets 0x14-0x1A (7 registers) */
static UBYTE vera_l1[7];        /* CONFIG, MAPBASE, TILEBASE, HSCROLL_L/H, VSCROLL_L/H */

/* Audio */
static UBYTE vera_audio_ctrl = 0;
static UBYTE vera_audio_rate = 0;

#define VERA_AUDIO_FIFO_SIZE   4096u
#define VERA_AUDIO_FIFO_MASK   (VERA_AUDIO_FIFO_SIZE - 1u)
#define VERA_AUDIO_FIFO_CAPACITY (VERA_AUDIO_FIFO_SIZE - 1u)
#define VERA_AUDIO_AFLOW_MASK  0x08u
#define VERA_PSG_VOICE_COUNT   16
#define VERA_PSG_REG_BASE      0x1F9C0u
#define VERA_DAC_RATE          (25000000.0 / 512.0)
#define VERA_PCM_MIX_SCALE     0.50

static UBYTE vera_pcm_fifo[VERA_AUDIO_FIFO_SIZE];
static unsigned int vera_pcm_fifo_read = 0;
static unsigned int vera_pcm_fifo_write = 0;
static unsigned int vera_pcm_fifo_count = 0;
static int vera_pcm_loop = FALSE;
static double vera_pcm_phase = 0.0;
static int vera_pcm_current_left = 0;
static int vera_pcm_current_right = 0;
static int vera_pcm_current_valid = FALSE;

static const UBYTE vera_pcm_volume_lut[16] = {
    0, 1, 2, 3, 4, 5, 6, 8, 11, 14, 18, 23, 30, 38, 49, 64
};

static unsigned int vera_host_playback_freq = 0;
static unsigned int vera_host_channels = 1;
static int vera_host_sample_size = 2;

typedef struct {
    UWORD noiseval;
    uint32_t phase;
} VERA_PSGState;

static VERA_PSGState vera_psg_state[VERA_PSG_VOICE_COUNT];
static UWORD vera_psg_noise_state = 1;

static const UWORD vera_psg_volume_lut[64] = {
      0,                                           4,   8,  12,
     16,  17,  18,  20,  21,  22,  23,  25,  26,  28,  30,  31,
     33,  35,  37,  40,  42,  45,  47,  50,  53,  56,  60,  63,
     67,  71,  75,  80,  85,  90,  95, 101, 107, 113, 120, 127,
    135, 143, 151, 160, 170, 180, 191, 202, 214, 227, 241, 255,
    270, 286, 303, 321, 341, 361, 382, 405, 429, 455, 482, 511
};

/* SPI */
static UBYTE vera_spi_data = 0xFF;
static UBYTE vera_spi_ctrl = 0;
static UBYTE vera_spi_tx = 0xFF;
static int vera_spi_ss = FALSE;
static int vera_spi_busy = FALSE;
static int vera_spi_autotx = FALSE;
static int vera_spi_cycles_until_done = 0;

enum {
    VERA_SD_CMD0   = 0,
    VERA_SD_CMD8   = 8,
    VERA_SD_CMD9   = 9,
    VERA_SD_CMD12  = 12,
    VERA_SD_CMD13  = 13,
    VERA_SD_CMD16  = 16,
    VERA_SD_CMD17  = 17,
    VERA_SD_CMD18  = 18,
    VERA_SD_CMD24  = 24,
    VERA_SD_CMD55  = 55,
    VERA_SD_CMD58  = 58,
    VERA_SD_ACMD13 = 0x80 | 13,
    VERA_SD_ACMD41 = 0x80 | 41
};

static UBYTE vera_sd_rxbuf[515];
static int vera_sd_rxbuf_idx = 0;
static ULONG vera_sd_lba = 0;
static UBYTE vera_sd_last_cmd = 0;
static int vera_sd_is_acmd = FALSE;
static int vera_sd_is_idle = TRUE;
static int vera_sd_is_initialized = FALSE;
static int vera_sd_ongoing_multiblock_read = FALSE;
static int vera_sd_selected = FALSE;
static const UBYTE *vera_sd_response = NULL;
static int vera_sd_response_length = 0;
static int vera_sd_response_counter = 0;

#ifdef PBI_DEBUG
#define D(a) a
#else
#define D(a) if (PBI_debug) a
#endif

#define Log_D(format, ...) D(Log_print(format, ##__VA_ARGS__))

/* ------------------------------------------------------------------ */
/* VERA internal helpers                                                */
/* ------------------------------------------------------------------ */

/* Reconstruct full 17-bit VRAM address from the three address bytes */
#define VERA_FULL_ADDR(p) \
    (((ULONG)(vera_addr_h[(p)] & 0x01u) << 16) | \
     ((ULONG)vera_addr_m[(p)] << 8) | (ULONG)vera_addr_l[(p)])

static void vera_set_full_addr(int p, ULONG addr)
{
    vera_addr_l[p] = (UBYTE)(addr & 0xFFu);
    vera_addr_m[p] = (UBYTE)((addr >> 8) & 0xFFu);
    vera_addr_h[p] = (vera_addr_h[p] & 0xFEu) | (UBYTE)((addr >> 16) & 0x01u);
}

static int vera_fx_increment_index(int p)
{
    return (vera_addr_h[p] >> 3) & 0x1F;
}

static int vera_fx_addr_nibble(int p)
{
    return (vera_addr_h[p] >> 1) & 1;
}

static void vera_fx_set_addr_nibble(int p, int nibble)
{
    vera_addr_h[p] = (vera_addr_h[p] & (UBYTE)~0x02u) | (UBYTE)(nibble ? 0x02u : 0x00u);
}

static int vera_fx_nibble_increment(int p)
{
    return (vera_addr_h[p] >> 2) & 1;
}

static int vera_adjust_fx_hop_step(int p, ULONG addr, int step)
{
    if (p == 1 && fx_16bit_hop) {
        int align = (int)(fx_16bit_hop_start_index & 0x03u);
        int lane = (int)(addr & 0x03u);

        if (step == 4)
            return (align == lane) ? 1 : 3;
        if (step == -4)
            return (align == lane) ? -1 : -3;
        if (step == 320)
            return (align == lane) ? 1 : 319;
        if (step == -320)
            return (align == lane) ? -1 : -319;
    }

    return step;
}

static void vera_advance_custom(int p, int step)
{
    ULONG addr = VERA_FULL_ADDR(p);

    if (fx_4bit_mode && vera_fx_nibble_increment(p) && step == 0) {
        if (vera_fx_addr_nibble(p)) {
            if ((vera_fx_increment_index(p) & 1) == 0)
                addr = (addr + 1u) & 0x1FFFFu;
            vera_fx_set_addr_nibble(p, 0);
        } else {
            if (vera_fx_increment_index(p) & 1)
                addr = (addr - 1u) & 0x1FFFFu;
            vera_fx_set_addr_nibble(p, 1);
        }
        vera_set_full_addr(p, addr);
        return;
    }

    if (step != 0) {
        step = vera_adjust_fx_hop_step(p, addr, step);
        addr = (addr + step) & 0x1FFFFu;
        vera_set_full_addr(p, addr);
    }
}

/* Advance address port p by its configured step after a DATA access */
static void vera_advance(int p)
{
    vera_advance_custom(p, vera_step_lut[vera_fx_increment_index(p)]);
}

static void vera_refresh_prefetch(int p)
{
    vera_rddata[p] = vera_vram[VERA_FULL_ADDR(p)];
}

static UBYTE vera_fx_cache_get_byte(int index)
{
    return (UBYTE)((ib_cache32 >> (index * 8)) & 0xFFu);
}

static void vera_fx_cache_set_byte(int index, UBYTE value)
{
    uint32_t mask = ~(0xFFu << (index * 8));

    ib_cache32 = (ib_cache32 & mask) | ((uint32_t)value << (index * 8));
}

static void vera_fx_cache_fill_push(UBYTE value, int addr_nibble)
{
    if (!fx_cache_fill_enabled)
        return;

    if (fx_4bit_mode) {
        UBYTE nibble_read = addr_nibble ? (UBYTE)((value & 0x0Fu) << 4) : (UBYTE)(value & 0xF0u);

        if (fx_cache_nibble_index) {
            UBYTE current = vera_fx_cache_get_byte(fx_cache_byte_index);
            vera_fx_cache_set_byte(fx_cache_byte_index, (UBYTE)((current & 0xF0u) | (nibble_read >> 4)));
            fx_cache_nibble_index = 0;
            fx_cache_byte_index = (fx_cache_byte_index + 1u) & 0x03u;
        } else {
            UBYTE current = vera_fx_cache_get_byte(fx_cache_byte_index);
            vera_fx_cache_set_byte(fx_cache_byte_index, (UBYTE)((current & 0x0Fu) | nibble_read));
            fx_cache_nibble_index = 1;
        }
    } else {
        vera_fx_cache_set_byte(fx_cache_byte_index, value);
        if (fx_cache_increment_mode)
            fx_cache_byte_index = (fx_cache_byte_index & 0x02u) | ((fx_cache_byte_index + 1u) & 0x01u);
        else
            fx_cache_byte_index = (fx_cache_byte_index + 1u) & 0x03u;
    }
}

static void vera_fx_vram_cache_write(ULONG address, UBYTE data, UBYTE nibble_mask)
{
    UBYTE old = vera_vram[address & 0x1FFFFu];
    UBYTE result = data;

    if (nibble_mask & 0x02u)
        result = (UBYTE)((result & 0x0Fu) | (old & 0xF0u));
    if (nibble_mask & 0x01u)
        result = (UBYTE)((result & 0xF0u) | (old & 0x0Fu));

    vera_vram[address & 0x1FFFFu] = result;
}

static void vera_fx_write_data(ULONG address, int addr_nibble, UBYTE value)
{
    if (fx_cache_write_enabled) {
        UBYTE cache_to_use[4];
        UBYTE wrdata_to_use;
        UBYTE ram_wrdata[4];
        UBYTE nibble_mask[4];
        int i;

        if (fx_mult_enabled) {
            int32_t m_result = (int16_t)((vera_fx_cache_get_byte(1) << 8) | vera_fx_cache_get_byte(0)) *
                               (int16_t)((vera_fx_cache_get_byte(3) << 8) | vera_fx_cache_get_byte(2));

            if (fx_add_or_sub)
                m_result = fx_mult_accumulator - m_result;
            else
                m_result = fx_mult_accumulator + m_result;

            cache_to_use[0] = (UBYTE)(m_result & 0xFF);
            cache_to_use[1] = (UBYTE)((m_result >> 8) & 0xFF);
            cache_to_use[2] = (UBYTE)((m_result >> 16) & 0xFF);
            cache_to_use[3] = (UBYTE)((m_result >> 24) & 0xFF);
        } else {
            for (i = 0; i < 4; i++)
                cache_to_use[i] = vera_fx_cache_get_byte(i);
        }

        if (fx_one_byte_cache_cycling)
            wrdata_to_use = vera_fx_cache_get_byte(fx_cache_byte_index);
        else
            wrdata_to_use = value;

        if (!fx_one_byte_cache_cycling) {
            for (i = 0; i < 4; i++)
                ram_wrdata[i] = cache_to_use[i];
        } else {
            for (i = 0; i < 4; i++)
                ram_wrdata[i] = wrdata_to_use;
        }

        if (fx_transparency_enabled) {
            if (fx_4bit_mode) {
                for (i = 0; i < 4; i++) {
                    nibble_mask[i] = (UBYTE)((((ram_wrdata[i] & 0xF0u) == 0u) << 1) |
                                             ((ram_wrdata[i] & 0x0Fu) == 0u));
                }
            } else {
                for (i = 0; i < 4; i++)
                    nibble_mask[i] = (ram_wrdata[i] != 0u) ? 0u : 3u;
            }
        } else {
            nibble_mask[0] = value & 0x03u;
            nibble_mask[1] = (value >> 2) & 0x03u;
            nibble_mask[2] = (value >> 4) & 0x03u;
            nibble_mask[3] = (value >> 6) & 0x03u;
        }

        address &= 0x1FFFCu;
        for (i = 0; i < 4; i++)
            vera_fx_vram_cache_write(address + (ULONG)i, ram_wrdata[i], nibble_mask[i]);
        return;
    }

    if (fx_4bit_mode) {
        UBYTE old = vera_vram[address & 0x1FFFFu];
        UBYTE result;

        if (addr_nibble)
            result = (UBYTE)((old & 0x0Fu) | (value << 4));
        else
            result = (UBYTE)((old & 0xF0u) | (value & 0x0Fu));
        vera_vram[address & 0x1FFFFu] = result;
    } else {
        vera_vram[address & 0x1FFFFu] = value;
    }
}

static void vera_fx_affine_prefetch(void)
{
    ULONG address;
    UBYTE affine_x_tile = (UBYTE)((fx_pixel_pos_x >> 10) & 0xFFu);
    UBYTE affine_y_tile = (UBYTE)((fx_pixel_pos_y >> 10) & 0xFFu);
    UBYTE affine_x_sub_tile = (UBYTE)((fx_pixel_pos_x >> 7) & 0x07u);
    UBYTE affine_y_sub_tile = (UBYTE)((fx_pixel_pos_y >> 7) & 0x07u);
    UBYTE affine_map_size = (UBYTE)(2u << (fx_map_size << 1));
    ULONG affine_tile_base = (ULONG)fx_tiledata_base_address << 9;
    ULONG affine_map_base = (ULONG)fx_map_base_address << 9;

    if (fx_addr1_mode != FX_MODE_AFFINE)
        return;

    if (!fx_apply_clip) {
        affine_x_tile &= (UBYTE)(affine_map_size - 1u);
        affine_y_tile &= (UBYTE)(affine_map_size - 1u);
    }

    if (affine_x_tile >= affine_map_size || affine_y_tile >= affine_map_size) {
        address = affine_tile_base +
                  ((ULONG)affine_y_sub_tile << (3 - fx_4bit_mode)) +
                  ((ULONG)affine_x_sub_tile >> fx_4bit_mode);
        if (fx_4bit_mode)
            vera_fx_set_addr_nibble(1, affine_x_sub_tile & 1u);
    } else {
        UBYTE affine_tile_idx;

        address = affine_map_base + (ULONG)affine_y_tile * (ULONG)affine_map_size + (ULONG)affine_x_tile;
        affine_tile_idx = vera_vram[address & 0x1FFFFu];
        address = affine_tile_base + ((ULONG)affine_tile_idx << (6 - fx_4bit_mode));
        address += ((ULONG)affine_y_sub_tile << (3 - fx_4bit_mode)) +
                   ((ULONG)affine_x_sub_tile >> fx_4bit_mode);
        if (fx_4bit_mode)
            vera_fx_set_addr_nibble(1, affine_x_sub_tile & 1u);
    }

    vera_set_full_addr(1, address & 0x1FFFFu);
    vera_refresh_prefetch(1);
}

static void vera_fx_2bit_poke(UBYTE value)
{
    UBYTE cache = vera_fx_cache_get_byte(fx_cache_byte_index);
    ULONG address = VERA_FULL_ADDR(1);
    UBYTE current = vera_rddata[1];
    UBYTE result = current;

    switch (value >> 6) {
    case 0x00:
        result = (UBYTE)((cache & 0xC0u) | (current & 0x3Fu));
        break;
    case 0x01:
        result = (UBYTE)((cache & 0x30u) | (current & 0xCFu));
        break;
    case 0x02:
        result = (UBYTE)((cache & 0x0Cu) | (current & 0xF3u));
        break;
    default:
        result = (UBYTE)((cache & 0x03u) | (current & 0xFCu));
        break;
    }

    vera_vram[address & 0x1FFFFu] = result;
    vera_rddata[1] = result;
    fx_2bit_poke_mode = FALSE;
}

static UBYTE vera_version_byte_for_offset(int offset)
{
    return vera_version_string[(offset - 0x09) & 0x03];
}

static void vera_sd_clear_response(void)
{
    vera_sd_response = NULL;
    vera_sd_response_length = 0;
    vera_sd_response_counter = 0;
}

static void vera_sd_reset_protocol(void)
{
    vera_sd_rxbuf_idx = 0;
    vera_sd_lba = 0;
    vera_sd_last_cmd = 0;
    vera_sd_is_acmd = FALSE;
    vera_sd_is_idle = TRUE;
    vera_sd_is_initialized = FALSE;
    vera_sd_ongoing_multiblock_read = FALSE;
    vera_sd_selected = FALSE;
    vera_sd_clear_response();
}

static void vera_sd_set_response_bytes(const UBYTE *bytes, int length)
{
    vera_sd_response = bytes;
    vera_sd_response_length = length;
    vera_sd_response_counter = 0;
}

static void vera_sd_set_response_r1(void)
{
    static UBYTE response[1];

    response[0] = (UBYTE)(vera_sd_is_idle ? 0x01u : 0x00u);
    vera_sd_set_response_bytes(response, 1);
}

static void vera_sd_set_response_r2(void)
{
    static const UBYTE not_ready[2] = { 0x1Fu, 0xFFu };
    static const UBYTE ready[2]     = { 0x00u, 0x00u };

    if (vera_sd_is_initialized)
        vera_sd_set_response_bytes(ready, 2);
    else
        vera_sd_set_response_bytes(not_ready, 2);
}

static void vera_sd_set_response_r3(void)
{
    static const UBYTE response[4] = { 0xC0u, 0xFFu, 0x80u, 0x00u };

    vera_sd_set_response_bytes(response, 4);
}

static void vera_sd_set_response_r7(void)
{
    static const UBYTE response[5] = { 0x01u, 0x00u, 0x00u, 0x01u, 0xAAu };

    vera_sd_set_response_bytes(response, 5);
}

static void vera_sd_set_response_csd(void)
{
    static UBYTE response[21] = {
        0xFFu, 0xFFu, 0x00u, 0xFFu, 0xFEu,
        0x40u, 0x0Eu, 0x00u, 0x32u, 0x5Bu, 0x59u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x7Fu, 0x80u,
        0x0Au, 0x40u, 0x00u, 0x01u
    };
    uint64_t c_size = 0;

    if (verax16_sdcard_size >= (off_t)(512u * 1024u))
        c_size = ((uint64_t)verax16_sdcard_size >> 19) - 1u;

    response[12] = (UBYTE)((response[12] & 0xC0u) | ((c_size >> 16) & 0x3Fu));
    response[13] = (UBYTE)((c_size >> 8) & 0xFFu);
    response[14] = (UBYTE)(c_size & 0xFFu);
    vera_sd_set_response_bytes(response, (int)sizeof(response));
}

static int vera_sd_seek_lba(ULONG lba)
{
    off_t offset = (off_t)lba * 512;

    if (verax16_sdcard_file == NULL)
        return FALSE;
    if (offset < 0 || offset >= verax16_sdcard_size)
        return FALSE;
    return fseeko(verax16_sdcard_file, offset, SEEK_SET) == 0;
}

static int vera_sd_load_block(UBYTE *dest)
{
    size_t bytes_read;

    dest[0] = 0xFEu;
    if (!vera_sd_seek_lba(vera_sd_lba)) {
        dest[0] = 0x08u;
        return 1;
    }

    bytes_read = fread(dest + 1, 1, 512, verax16_sdcard_file);
    if (bytes_read != 512u) {
        Log_print("VeraX16: short SD read at LBA %lu", (unsigned long)vera_sd_lba);
        memset(dest + 1 + bytes_read, 0xFF, 512u - bytes_read);
    }
    dest[513] = 0xFFu;
    dest[514] = 0xFFu;
    return 515;
}

static void vera_sd_store_block(const UBYTE *src)
{
    size_t bytes_written;

    if (!verax16_sdcard_writable)
        return;
    if (!vera_sd_seek_lba(vera_sd_lba))
        return;

    bytes_written = fwrite(src, 1, 512, verax16_sdcard_file);
    if (bytes_written != 512u)
        Log_print("VeraX16: short SD write at LBA %lu", (unsigned long)vera_sd_lba);
    fflush(verax16_sdcard_file);
}

static void vera_sd_select(int selected)
{
    vera_sd_selected = selected;
    vera_sd_rxbuf_idx = 0;
    if (!selected) {
        vera_sd_ongoing_multiblock_read = FALSE;
        vera_sd_clear_response();
    }
}

static UBYTE vera_sd_handle_command(UBYTE cmd, const UBYTE *packet)
{
    static UBYTE read_block_response[516];
    static UBYTE write_error_response[2];
    ULONG arg = ((ULONG)packet[1] << 24) | ((ULONG)packet[2] << 16) |
                ((ULONG)packet[3] << 8) | (ULONG)packet[4];
    int response_length;

    vera_sd_last_cmd = cmd;

    switch (cmd) {
    case VERA_SD_CMD0:
        vera_sd_is_idle = TRUE;
        vera_sd_set_response_r1();
        break;
    case VERA_SD_CMD8:
        vera_sd_set_response_r7();
        break;
    case VERA_SD_CMD9:
        vera_sd_set_response_csd();
        break;
    case VERA_SD_ACMD41:
        vera_sd_is_idle = FALSE;
        vera_sd_is_initialized = TRUE;
        vera_sd_set_response_r1();
        break;
    case VERA_SD_CMD12:
        vera_sd_ongoing_multiblock_read = FALSE;
        vera_sd_set_response_r1();
        break;
    case VERA_SD_CMD13:
    case VERA_SD_ACMD13:
        vera_sd_set_response_r2();
        break;
    case VERA_SD_CMD16:
        vera_sd_set_response_r1();
        break;
    case VERA_SD_CMD18:
        vera_sd_ongoing_multiblock_read = TRUE;
        /* fall through */
    case VERA_SD_CMD17:
        vera_sd_lba = arg;
        read_block_response[0] = 0x00u;
        response_length = 1 + vera_sd_load_block(&read_block_response[1]);
        if (response_length == 2)
            vera_sd_ongoing_multiblock_read = FALSE;
        vera_sd_set_response_bytes(read_block_response, response_length);
        break;
    case VERA_SD_CMD24:
        vera_sd_lba = arg;
        if ((off_t)vera_sd_lba * 512 >= verax16_sdcard_size) {
            write_error_response[0] = 0x00u;
            write_error_response[1] = 0x08u;
            vera_sd_set_response_bytes(write_error_response, 2);
        }
        else {
            vera_sd_set_response_r1();
        }
        break;
    case VERA_SD_CMD55:
        vera_sd_is_acmd = TRUE;
        vera_sd_set_response_r1();
        break;
    case VERA_SD_CMD58:
        vera_sd_set_response_r3();
        break;
    default:
        vera_sd_set_response_r1();
        break;
    }

    return 0xFFu;
}

static UBYTE vera_sd_handle(UBYTE inbyte)
{
    UBYTE outbyte = 0xFFu;

    if (!vera_sd_selected || verax16_sdcard_file == NULL)
        return 0xFFu;

    if (vera_sd_rxbuf_idx == 0 && inbyte == 0xFFu) {
        if (vera_sd_response != NULL) {
            outbyte = vera_sd_response[vera_sd_response_counter++];
            if (vera_sd_response_counter >= vera_sd_response_length) {
                if (vera_sd_ongoing_multiblock_read) {
                    static UBYTE read_multiblock_response[515];
                    int response_length;

                    vera_sd_lba++;
                    response_length = vera_sd_load_block(read_multiblock_response);
                    if (response_length == 1)
                        vera_sd_ongoing_multiblock_read = FALSE;
                    vera_sd_set_response_bytes(read_multiblock_response, response_length);
                }
                else {
                    vera_sd_clear_response();
                }
            }
        }
        return outbyte;
    }

    vera_sd_rxbuf[vera_sd_rxbuf_idx++] = inbyte;

    if ((vera_sd_rxbuf[0] & 0xC0u) == 0x40u && vera_sd_rxbuf_idx == 6) {
        UBYTE cmd = vera_sd_rxbuf[0] & 0x3Fu;

        vera_sd_rxbuf_idx = 0;
        if (vera_sd_is_acmd) {
            cmd |= 0x80u;
            vera_sd_is_acmd = FALSE;
        }
        return vera_sd_handle_command(cmd, vera_sd_rxbuf);
    }

    if (vera_sd_rxbuf_idx == 515) {
        vera_sd_rxbuf_idx = 0;
        if (vera_sd_last_cmd == VERA_SD_CMD24 && vera_sd_rxbuf[0] == 0xFEu) {
            static const UBYTE data_response_ok[1] = { 0x05u };
            static const UBYTE data_response_fail[1] = { 0x0Du };

            if (verax16_sdcard_writable && vera_sd_seek_lba(vera_sd_lba)) {
                vera_sd_store_block(vera_sd_rxbuf + 1);
                vera_sd_set_response_bytes(data_response_ok, 1);
            }
            else {
                vera_sd_set_response_bytes(data_response_fail, 1);
            }
        }
    }

    return outbyte;
}

static int vera_sdcard_attach(void)
{
    off_t size;

    if (verax16_sdcard_filename[0] == '\0')
        return TRUE;
    if (verax16_sdcard_file != NULL)
        return TRUE;

    verax16_sdcard_file = fopen(verax16_sdcard_filename, "rb+");
    verax16_sdcard_writable = TRUE;
    if (verax16_sdcard_file == NULL) {
        verax16_sdcard_file = fopen(verax16_sdcard_filename, "rb");
        verax16_sdcard_writable = FALSE;
    }
    if (verax16_sdcard_file == NULL) {
        Log_print("VeraX16: WARNING - cannot open SD image %s", verax16_sdcard_filename);
        return FALSE;
    }

    if (fseeko(verax16_sdcard_file, 0, SEEK_END) != 0) {
        fclose(verax16_sdcard_file);
        verax16_sdcard_file = NULL;
        verax16_sdcard_writable = FALSE;
        Log_print("VeraX16: WARNING - cannot size SD image %s", verax16_sdcard_filename);
        return FALSE;
    }

    size = ftello(verax16_sdcard_file);
    if (size < 0) {
        fclose(verax16_sdcard_file);
        verax16_sdcard_file = NULL;
        verax16_sdcard_writable = FALSE;
        Log_print("VeraX16: WARNING - invalid SD image size for %s", verax16_sdcard_filename);
        return FALSE;
    }

    verax16_sdcard_size = size;
    if (fseeko(verax16_sdcard_file, 0, SEEK_SET) != 0)
        Log_print("VeraX16: WARNING - cannot rewind SD image %s", verax16_sdcard_filename);

    vera_sd_reset_protocol();
    Log_print("VeraX16: SD image attached from %s (%s, %lu bytes)",
              verax16_sdcard_filename,
              verax16_sdcard_writable ? "read-write" : "read-only",
              (unsigned long)verax16_sdcard_size);
    return TRUE;
}

static void vera_sdcard_detach(void)
{
    if (verax16_sdcard_file != NULL) {
        fclose(verax16_sdcard_file);
        verax16_sdcard_file = NULL;
    }
    verax16_sdcard_size = 0;
    verax16_sdcard_writable = FALSE;
    vera_sd_reset_protocol();
}

static void vera_spi_begin_transfer(UBYTE byte)
{
    if (!vera_spi_ss || vera_spi_busy)
        return;

    vera_spi_tx = byte;
    vera_spi_busy = TRUE;
    vera_spi_cycles_until_done = 10;
}

static void vera_spi_step(int cycles)
{
    if (!vera_spi_busy)
        return;

    vera_spi_cycles_until_done -= cycles;
    if (vera_spi_cycles_until_done <= 0) {
        vera_spi_busy = FALSE;
        vera_spi_cycles_until_done = 0;
        vera_spi_data = vera_sd_handle(vera_spi_tx);
    }
}

static void vera_update_irq(void)
{
    if (vera_isr & vera_ien)
        PBI_IRQ |= verax16_pbi_mask;
    else
        PBI_IRQ &= ~verax16_pbi_mask;
}

static void vera_audio_update_aflow(void)
{
    if (vera_pcm_fifo_count < (VERA_AUDIO_FIFO_SIZE / 4u))
        vera_isr |= VERA_AUDIO_AFLOW_MASK;
    else
        vera_isr &= ~VERA_AUDIO_AFLOW_MASK;
    vera_update_irq();
}

#define VERA_SCANLINES_PER_FRAME 525u
#define VERA_SCANLINE_CLAMP_START 512u
#define VERA_SCANLINE_CLAMP_VALUE 0x01FFu
#define VERA_MIDLINE_X_SCALE 256
#define VERA_MIDLINE_DATA_STEP_8BPP (4 * VERA_MIDLINE_X_SCALE)
#define VERA_MIDLINE_DATA_STEP_4BPP (3 * VERA_MIDLINE_X_SCALE)
#define VERA_MIDLINE_LAYER_STEP (9 * VERA_MIDLINE_X_SCALE)
#define VERA_MIDLINE_DC_STEP (12 * VERA_MIDLINE_X_SCALE)
#define VERA_MIDLINE_FX_STEP (10 * VERA_MIDLINE_X_SCALE)

static UWORD vera_scanline_read_value(void)
{
    if (vera_scanline_raw >= VERA_SCANLINE_CLAMP_START)
        return VERA_SCANLINE_CLAMP_VALUE;
    return vera_scanline_raw;
}

static int vera_midline_write_cost(int offset, int dcsel)
{
    if (offset == 0x03 || offset == 0x04) {
        if (fx_addr1_mode == FX_MODE_LINE_DRAW ||
            fx_addr1_mode == FX_MODE_POLY_FILL ||
            fx_addr1_mode == FX_MODE_AFFINE ||
            fx_cache_write_enabled || fx_cache_fill_enabled)
            return VERA_MIDLINE_FX_STEP;
        return fx_4bit_mode ? VERA_MIDLINE_DATA_STEP_4BPP : VERA_MIDLINE_DATA_STEP_8BPP;
    }

    if (offset >= 0x09 && offset <= 0x0C)
        return (dcsel >= 0x02 && dcsel <= 0x06) ? VERA_MIDLINE_FX_STEP : VERA_MIDLINE_DC_STEP;
    if (offset >= 0x0D && offset <= 0x1A)
        return VERA_MIDLINE_LAYER_STEP;

    return VERA_MIDLINE_DATA_STEP_8BPP;
}

static int vera_midline_affects_video(int offset, int dcsel)
{
    if (offset == 0x03 || offset == 0x04)
        return TRUE;
    if (offset >= 0x0D && offset <= 0x1A)
        return TRUE;
    if (offset >= 0x09 && offset <= 0x0C)
        return (dcsel == 0x00 || dcsel == 0x01);
    return FALSE;
}

static void vera_midline_sync_cursor(void)
{
    int base_x_fp;

    if (Atari800_tv_mode <= 0)
        return;

    base_x_fp = (int)(((unsigned long)vera_scanline_accum * 640u * VERA_MIDLINE_X_SCALE) /
                      (unsigned int)Atari800_tv_mode);
    if (base_x_fp < 0)
        base_x_fp = 0;
    if (base_x_fp > 640 * VERA_MIDLINE_X_SCALE)
        base_x_fp = 640 * VERA_MIDLINE_X_SCALE;
    if (vera_midline_x_fp < base_x_fp)
        vera_midline_x_fp = base_x_fp;
}

static void vera_midline_rerender_tail(int offset, int dcsel)
{
    int cost;
    int x;

    if (vera_scanline_raw >= 480u)
        return;

    vera_midline_sync_cursor();

    if (vera_midline_x_fp < 0)
        vera_midline_x_fp = 0;
    if (vera_midline_x_fp > 640 * VERA_MIDLINE_X_SCALE)
        vera_midline_x_fp = 640 * VERA_MIDLINE_X_SCALE;

    x = (vera_midline_x_fp + (VERA_MIDLINE_X_SCALE / 2)) / VERA_MIDLINE_X_SCALE;
    if (x < 0)
        x = 0;
    if (x > 640)
        x = 640;

    VERA_VIDEO_Midline(vera_scanline_raw, (UWORD)x);

    cost = vera_midline_write_cost(offset, dcsel);
    vera_midline_x_fp += cost;
    if (vera_midline_x_fp > 640 * VERA_MIDLINE_X_SCALE)
        vera_midline_x_fp = 640 * VERA_MIDLINE_X_SCALE;
}

static void vera_maybe_trigger_line_irq(void)
{
    if (vera_scanline_raw > 0x01FFu)
        return;

    if ((vera_dc[0][0] & 0x08u) != 0) {
        if ((vera_scanline_raw >> 1) == (vera_irqline >> 1))
            vera_isr |= 0x02u;
    }
    else if (vera_scanline_raw == vera_irqline) {
        vera_isr |= 0x02u;
    }
}

static UBYTE vera_compute_sprite_line_collisions(UWORD py)
{
    UBYTE sprite_line_mask[640u];
    UBYTE collisions = 0;
    ULONG i;
    int line_y = (int)py;

    if (py >= 480u)
        return 0;
    if ((vera_dc[0][0] & 0x40u) == 0)
        return 0;

    memset(sprite_line_mask, 0, sizeof(sprite_line_mask));
    for (i = 0; i < 128u; i++) {
        ULONG base = 0x1FC00u + i * 8u;
        UBYTE attr0 = vera_vram[base];
        UBYTE attr1 = vera_vram[base + 1u];
        int x = (int)vera_vram[base + 2u] | (((int)vera_vram[base + 3u] & 0x03) << 8);
        int y = (int)vera_vram[base + 4u] | (((int)vera_vram[base + 5u] & 0x03) << 8);
        UBYTE attr6 = vera_vram[base + 6u];
        UBYTE attr7 = vera_vram[base + 7u];
        UBYTE coll_mask = attr6 & 0xF0u;
        int z_depth = (attr6 >> 2) & 0x03;
        int width_log2 = ((attr7 >> 4) & 0x03) + 3;
        int height_log2 = (attr7 >> 6) + 3;
        int width = 1 << width_log2;
        int height = 1 << height_log2;
        int hflip = attr6 & 0x01;
        int vflip = (attr6 >> 1) & 0x01;
        int color_mode = (attr1 >> 7) & 0x01;
        ULONG sprite_addr = ((ULONG)attr0 << 5) | ((ULONG)(attr1 & 0x0Fu) << 13);
        int sy;
        int eff_sy;
        ULONG row_addr;
        int sx;

        if (z_depth == 0 || coll_mask == 0u)
            continue;
        if (x >= 0x400 - width) x -= 0x400;
        if (y >= 0x400 - height) y -= 0x400;
        if (line_y < y || line_y >= y + height)
            continue;
        if (x >= 640 || x + width <= 0)
            continue;

        sy = line_y - y;
        eff_sy = vflip ? (height - 1 - sy) : sy;
        row_addr = sprite_addr + ((ULONG)eff_sy << (width_log2 - (1 - color_mode)));

        for (sx = 0; sx < width; sx++) {
            int px = x + sx;
            int eff_sx;
            UBYTE color_idx;
            ULONG pixel_addr;

            if (px < 0 || px >= 640)
                continue;

            eff_sx = hflip ? (width - 1 - sx) : sx;
            if (color_mode == 0) {
                UBYTE packed;

                pixel_addr = row_addr + ((ULONG)eff_sx >> 1);
                packed = vera_vram[pixel_addr & 0x1FFFFu];
                color_idx = (eff_sx & 1) ? (packed & 0x0Fu) : (packed >> 4);
            }
            else {
                pixel_addr = row_addr + (ULONG)eff_sx;
                color_idx = vera_vram[pixel_addr & 0x1FFFFu];
            }

            if (color_idx == 0u)
                continue;

            collisions |= sprite_line_mask[px] & coll_mask;
            sprite_line_mask[px] |= coll_mask;
        }
    }

    return collisions;
}

static void vera_update_isr_and_coll(UWORD y)
{
    if (y == 480u) {
        if (vera_sprite_frame_collisions != 0u)
            vera_isr |= 0x04u;
        vera_isr = (vera_isr & 0x0Fu) | vera_sprite_frame_collisions;
        vera_sprite_frame_collisions = 0u;
        vera_isr |= 0x01u;
    }
}

static void vera_advance_scanline_once(void)
{
    if (vera_scanline_raw < 480u)
        vera_sprite_frame_collisions |= vera_compute_sprite_line_collisions(vera_scanline_raw);

    if (vera_scanline_raw + 1u >= VERA_SCANLINES_PER_FRAME) {
        vera_scanline_raw = 0;
        vera_current_field ^= 1u;
    }
    else {
        vera_scanline_raw++;
    }

    vera_update_isr_and_coll(vera_scanline_raw);
    vera_maybe_trigger_line_irq();
    vera_update_irq();
    vera_spi_step(16);
    vera_midline_x_fp = 0;
    VERA_VIDEO_Scanline(vera_scanline_raw);
}

static void vera_audio_reset_state(void)
{
    int i;

    vera_pcm_fifo_read = 0;
    vera_pcm_fifo_write = 0;
    vera_pcm_fifo_count = 0;
    vera_pcm_loop = FALSE;
    vera_pcm_phase = 0.0;
    vera_pcm_current_left = 0;
    vera_pcm_current_right = 0;
    vera_pcm_current_valid = FALSE;
    memset(vera_pcm_fifo, 0, sizeof(vera_pcm_fifo));

    memset(vera_psg_state, 0, sizeof(vera_psg_state));
    for (i = 0; i < VERA_PSG_VOICE_COUNT; i++)
        vera_psg_state[i].noiseval = 0;
    vera_psg_noise_state = 1;
}

static int vera_pcm_read_byte(UBYTE *byte)
{
    if (vera_pcm_fifo_count == 0u)
        return FALSE;

    *byte = vera_pcm_fifo[vera_pcm_fifo_read];
    vera_pcm_fifo_read = (vera_pcm_fifo_read + 1u) & VERA_AUDIO_FIFO_MASK;
    vera_pcm_fifo_count--;
    vera_audio_update_aflow();
    return TRUE;
}

static void vera_pcm_reset_fifo(void)
{
    vera_pcm_fifo_read = 0;
    vera_pcm_fifo_write = 0;
    vera_pcm_fifo_count = 0;
    vera_pcm_loop = FALSE;
    vera_pcm_phase = 0.0;
    vera_pcm_current_left = 0;
    vera_pcm_current_right = 0;
    vera_pcm_current_valid = FALSE;
    memset(vera_pcm_fifo, 0, sizeof(vera_pcm_fifo));
    vera_audio_update_aflow();
}

static void vera_pcm_restart_fifo(void)
{
    vera_pcm_fifo_read = 0;
    vera_pcm_fifo_count = vera_pcm_fifo_write;
    vera_audio_update_aflow();
}

static UBYTE vera_audio_ctrl_read(void)
{
    UBYTE ctrl = vera_audio_ctrl & 0x3Fu;

    if (vera_pcm_fifo_count == 0u)
        ctrl |= 0x40u;
    if (vera_pcm_fifo_count >= VERA_AUDIO_FIFO_CAPACITY)
        ctrl |= 0x80u;
    return ctrl;
}

static void vera_audio_ctrl_write(UBYTE byte)
{
    int old_format = vera_audio_ctrl & 0x30u;

    if ((byte & 0xC0u) == 0xC0u)
        vera_pcm_loop = TRUE;
    else {
        vera_pcm_loop = FALSE;
        if (byte & 0x80u)
            vera_pcm_reset_fifo();
    }
    if (byte & 0x40u)
        vera_pcm_restart_fifo();

    vera_audio_ctrl = byte & 0x3Fu;
    if ((old_format ^ vera_audio_ctrl) & 0x30u)
        vera_pcm_current_valid = FALSE;
}

static void vera_audio_data_write(UBYTE byte)
{
    if (vera_pcm_fifo_count >= VERA_AUDIO_FIFO_CAPACITY)
        return;

    vera_pcm_fifo[vera_pcm_fifo_write] = byte;
    vera_pcm_fifo_write = (vera_pcm_fifo_write + 1u) & VERA_AUDIO_FIFO_MASK;
    vera_pcm_fifo_count++;
    vera_audio_update_aflow();
}

static int vera_pcm_load_current_sample(void)
{
    UBYTE raw[4];

    if (vera_audio_ctrl & 0x20u) {
        if (vera_audio_ctrl & 0x10u) {
            if (vera_pcm_fifo_count < 4u) {
                if (vera_pcm_loop)
                    vera_pcm_restart_fifo();
                if (vera_pcm_fifo_count < 4u) {
                    vera_pcm_fifo_count = 0;
                    vera_pcm_fifo_read = vera_pcm_fifo_write;
                    vera_audio_update_aflow();
                    return FALSE;
                }
            }
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]) ||
                !vera_pcm_read_byte(&raw[2]) || !vera_pcm_read_byte(&raw[3]))
                return FALSE;
            vera_pcm_current_left = (int)(SWORD)((UWORD)raw[0] | ((UWORD)raw[1] << 8));
            vera_pcm_current_right = (int)(SWORD)((UWORD)raw[2] | ((UWORD)raw[3] << 8));
        }
        else {
            SWORD mono;
            if (vera_pcm_fifo_count < 2u) {
                if (vera_pcm_loop)
                    vera_pcm_restart_fifo();
                if (vera_pcm_fifo_count < 2u) {
                    vera_pcm_fifo_count = 0;
                    vera_pcm_fifo_read = vera_pcm_fifo_write;
                    vera_audio_update_aflow();
                    return FALSE;
                }
            }
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]))
                return FALSE;
            mono = (SWORD)((UWORD)raw[0] | ((UWORD)raw[1] << 8));
            vera_pcm_current_left = (int)mono;
            vera_pcm_current_right = (int)mono;
        }
    }
    else {
        if (vera_audio_ctrl & 0x10u) {
            if (vera_pcm_fifo_count < 2u) {
                if (vera_pcm_loop)
                    vera_pcm_restart_fifo();
                if (vera_pcm_fifo_count < 2u) {
                    vera_pcm_fifo_count = 0;
                    vera_pcm_fifo_read = vera_pcm_fifo_write;
                    vera_audio_update_aflow();
                    return FALSE;
                }
            }
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]))
                return FALSE;
            vera_pcm_current_left = (int)((UWORD)raw[0] << 8);
            vera_pcm_current_right = (int)((UWORD)raw[1] << 8);
        }
        else {
            int mono;
            if (vera_pcm_fifo_count < 1u) {
                if (vera_pcm_loop)
                    vera_pcm_restart_fifo();
                if (vera_pcm_fifo_count < 1u)
                    return FALSE;
            }
            if (!vera_pcm_read_byte(&raw[0]))
                return FALSE;
            mono = (int)((UWORD)raw[0] << 8);
            vera_pcm_current_left = mono;
            vera_pcm_current_right = mono;
        }
    }

    if (vera_pcm_loop && vera_pcm_fifo_count == 0u)
        vera_pcm_restart_fifo();

    vera_pcm_current_valid = TRUE;
    return TRUE;
}

static void vera_pcm_render_sample(int *left, int *right)
{
    double step;
    UBYTE rate = vera_audio_rate;
    UBYTE volume = vera_pcm_volume_lut[vera_audio_ctrl & 0x0Fu];

    *left = 0;
    *right = 0;

    if (vera_host_playback_freq == 0u || rate == 0u)
        return;

    if (!vera_pcm_current_valid && !vera_pcm_load_current_sample())
        return;

    *left = (int)(((double)((long)vera_pcm_current_left * (long)volume) / 64.0) * VERA_PCM_MIX_SCALE);
    *right = (int)(((double)((long)vera_pcm_current_right * (long)volume) / 64.0) * VERA_PCM_MIX_SCALE);

    step = (VERA_DAC_RATE * (double)rate) / (128.0 * (double)vera_host_playback_freq);
    vera_pcm_phase += step;
    while (vera_pcm_phase >= 1.0) {
        vera_pcm_phase -= 1.0;
        if (!vera_pcm_load_current_sample()) {
            vera_pcm_current_valid = FALSE;
            vera_pcm_current_left = 0;
            vera_pcm_current_right = 0;
            break;
        }
    }
}

/* Soft reset: registers to defaults, VRAM contents preserved */
static void vera_chip_reset(const char *caller)
{
    Log_print("VeraX16: VERA chip soft-reset called by: %s", caller);
    memset(vera_addr_l, 0, sizeof(vera_addr_l));
    memset(vera_addr_m, 0, sizeof(vera_addr_m));
    memset(vera_addr_h, 0, sizeof(vera_addr_h));
    memset(vera_rddata, 0, sizeof(vera_rddata));
    vera_ctrl     = 0;
    vera_ien      = 0;
    vera_isr      = 0;
    vera_irqline  = 0;
    vera_scanline_raw = 0;
    vera_current_field = 0;
    vera_scanline_accum = 0;
    vera_midline_x_fp = 0;
    vera_sprite_frame_collisions = 0;

    /* DC defaults */
    memset(vera_dc, 0, sizeof(vera_dc));
    vera_dc[0][1] = 128;    /* DC_HSCALE = 128 (1:1) */
    vera_dc[0][2] = 128;    /* DC_VSCALE = 128 (1:1) */
    /* DC secondary defaults (DCSEL=1 bank) */
    vera_dc[1][1] = 160;    /* DC_HSTOP  = 160 (640 / 4) */
    vera_dc[1][3] = 240;    /* DC_VSTOP  = 240 (480 / 2) */

    /* FX state reset */
    fx_pixel_pos_x = 0;
    fx_pixel_pos_y = 0;
    fx_pixel_incr_x = 0;
    fx_pixel_incr_y = 0;
    fx_pixel_incr_x_times_32 = FALSE;
    fx_pixel_incr_y_times_32 = FALSE;
    fx_addr1_mode = 0;
    fx_4bit_mode = FALSE;
    fx_16bit_hop = FALSE;
    fx_transparency_enabled = FALSE;
    fx_cache_write_enabled = FALSE;
    fx_cache_fill_enabled = FALSE;
    fx_one_byte_cache_cycling = FALSE;
    fx_tiledata_base_address = 0;
    fx_map_base_address = 0;
    fx_map_size = 0;
    fx_apply_clip = FALSE;
    fx_2bit_polygon_pixels = FALSE;
    ib_cache32 = 0;
    fx_cache_byte_index = 0;
    fx_cache_nibble_index = 0;
    fx_cache_increment_mode = 0;
    fx_mult_enabled = FALSE;
    fx_add_or_sub = 0;
    fx_accumulate = FALSE;
    fx_mult_accumulator = 0;

    memset(vera_l0, 0, sizeof(vera_l0));
    memset(vera_l1, 0, sizeof(vera_l1));

    vera_audio_ctrl = 0;
    vera_audio_rate = 0;
    vera_audio_reset_state();
    vera_spi_data   = 0xFF;
    vera_spi_ctrl   = 0;
    vera_spi_tx     = 0xFF;
    vera_spi_ss     = FALSE;
    vera_spi_busy   = FALSE;
    vera_spi_autotx = FALSE;
    vera_spi_cycles_until_done = 0;
    vera_sd_reset_protocol();

    VERA_VIDEO_Reset();
    vera_refresh_prefetch(0);
    vera_refresh_prefetch(1);
    VERA_VIDEO_Scanline(0);
    vera_update_irq();

    Log_print("VeraX16: VERA chip soft-reset");
}

static UBYTE vera_ien_read(void)
{
    return (UBYTE)(((vera_irqline >> 8) & 0x01u) << 7) |
           (UBYTE)(((vera_scanline_read_value() >> 8) & 0x01u) << 6) |
           (vera_ien & 0x0Fu);
}

static UBYTE vera_isr_read(void)
{
    return vera_isr;
}

static UBYTE vera_dc_video_read(void)
{
    return (UBYTE)((vera_current_field << 7) | (vera_dc[0][0] & 0x7Fu));
}

static UBYTE vera_spi_ctrl_read(void)
{
    return (UBYTE)((vera_spi_busy ? 0x80u : 0x00u) |
                   (vera_spi_autotx ? 0x04u : 0x00u) |
                   (vera_spi_ss ? 0x01u : 0x00u));
}

/* ------------------------------------------------------------------ */
/* VERA register read/write                                             */
/* ------------------------------------------------------------------ */

static UBYTE vera_read_reg(int offset, int no_side_effects)
{
    int addrsel = vera_ctrl & 0x01;
    int dcsel   = (vera_ctrl >> 1) & 0x3F;

    switch (offset) {
    case 0x00:
        return vera_addr_l[addrsel];
    case 0x01:
        return vera_addr_m[addrsel];
    case 0x02:
        return vera_addr_h[addrsel];
    case 0x03:  /* DATA0 — VRAM read through port 0 */
        {
            UBYTE value = vera_rddata[0];
            if (!no_side_effects) {
                int addr_nibble = vera_fx_addr_nibble(0);
                vera_advance(0);
                vera_refresh_prefetch(0);
                vera_fx_cache_fill_push(value, addr_nibble);
            }
            return value;
        }
    case 0x04:  /* DATA1 — VRAM read through port 1 */
        {
            UBYTE value = vera_rddata[1];
            if (!no_side_effects) {
                int addr_nibble = vera_fx_addr_nibble(1);
                if (fx_addr1_mode == FX_MODE_AFFINE) {
                    fx_pixel_pos_x += (int32_t)fx_pixel_incr_x << (fx_pixel_incr_x_times_32 ? 4 : 11);
                    fx_pixel_pos_y += (int32_t)fx_pixel_incr_y << (fx_pixel_incr_y_times_32 ? 4 : 11);
                    vera_fx_affine_prefetch();
                } else if (fx_addr1_mode == FX_MODE_POLY_FILL) {
                    fx_pixel_pos_x += (int32_t)fx_pixel_incr_x << (fx_pixel_incr_x_times_32 ? 4 : 11);
                    fx_pixel_pos_y += (int32_t)fx_pixel_incr_y << (fx_pixel_incr_y_times_32 ? 4 : 11);
                    if (fx_one_byte_cache_cycling && !fx_cache_fill_enabled)
                        fx_cache_byte_index = (fx_cache_byte_index + 1u) & 0x03u;
                    if (fx_4bit_mode) {
                        vera_set_full_addr(1, (VERA_FULL_ADDR(0) + ((ULONG)(fx_pixel_pos_x >> 10) & 0x1FFFFu)) & 0x1FFFFu);
                        vera_fx_set_addr_nibble(1, (fx_pixel_pos_x >> 9) & 1);
                    } else {
                        vera_set_full_addr(1, (VERA_FULL_ADDR(0) + ((ULONG)(fx_pixel_pos_x >> 9) & 0x1FFFFu)) & 0x1FFFFu);
                    }
                    vera_refresh_prefetch(1);
                } else {
                    vera_advance(1);
                    vera_refresh_prefetch(1);
                }
                vera_fx_cache_fill_push(value, addr_nibble);
            }
            return value;
        }
    case 0x05:  /* CTRL — RESET bit always reads 0 */
        return vera_ctrl & 0x7Fu;
    case 0x06:
        return vera_ien_read();
    case 0x07:
        return vera_isr_read();
    case 0x08:
        return (UBYTE)(vera_scanline_read_value() & 0xFFu);
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C) {
            switch (dcsel) {
            case 0x00:
                if (offset == 0x09)
                    return vera_dc_video_read();
                return vera_dc[0][offset - 0x09];
            case 0x01:
                return vera_dc[1][offset - 0x09];
            case 0x02:
                if (offset == 0x09)
                    return (fx_transparency_enabled << 7) |
                           (fx_cache_write_enabled << 6) |
                           (fx_cache_fill_enabled << 5) |
                           (fx_one_byte_cache_cycling << 4) |
                           (fx_16bit_hop << 3) |
                           (fx_4bit_mode << 2) |
                           (fx_addr1_mode & 0x03);
                return vera_version_byte_for_offset(offset);
            case 0x05:
                if (offset == 0x0B) {
                    /* fx_fill_length_low */
                    uint16_t flen = (uint16_t)((fx_pixel_pos_y >> 9) - (fx_pixel_pos_x >> 9)) & 0x3FF;
                    int overflow = (flen >> 8) == 3;
                    int poly_fill_2bit = (fx_addr1_mode == FX_MODE_POLY_FILL) && fx_4bit_mode && fx_2bit_polygon_pixels;
                    UBYTE res = 0;
                    if (poly_fill_2bit && (fx_pixel_pos_x & 0x100)) res |= 0x01;
                    if (!overflow && (flen & 0x01)) res |= 0x02;
                    if (!overflow && (flen & 0x02)) res |= 0x04;
                    if (!overflow && (flen & 0x04)) res |= 0x08;
                    if (!overflow) {
                        if (!fx_4bit_mode && (flen & 0x08)) res |= 0x10;
                        else if (fx_4bit_mode && (fx_pixel_pos_x & 0x800)) res |= 0x10;
                    }
                    if (!overflow && (fx_pixel_pos_x & 0x200)) res |= 0x20;
                    if (!overflow && (fx_pixel_pos_x & 0x400)) res |= 0x40;
                    if ((!fx_4bit_mode && flen > 15) || 
                        (!poly_fill_2bit && fx_4bit_mode && flen > 7) ||
                        (poly_fill_2bit && (fx_pixel_pos_y & 0x100))) res |= 0x80;
                    return res;
                }
                if (offset == 0x0C) {
                    /* fx_fill_length_high */
                    uint16_t flen = (uint16_t)((fx_pixel_pos_y >> 9) - (fx_pixel_pos_x >> 9)) & 0x3FF;
                    return (UBYTE)((flen >> 3) << 1);
                }
                return vera_version_byte_for_offset(offset);
            case 0x06:
                if (offset == 0x09)
                    return (UBYTE)(ib_cache32 & 0xFFu);
                if (offset == 0x0A)
                    return (UBYTE)((ib_cache32 >> 8) & 0xFFu);
                if (offset == 0x0B)
                    return (UBYTE)((ib_cache32 >> 16) & 0xFFu);
                return (UBYTE)((ib_cache32 >> 24) & 0xFFu);
            case 0x3F:
                return vera_version_byte_for_offset(offset);
            }
            return vera_version_byte_for_offset(offset);
        }
        /* Layer 0 fixed: offsets 0x0D-0x13 */
        if (offset >= 0x0D && offset <= 0x13)
            return vera_l0[offset - 0x0D];
        /* Layer 1 fixed: offsets 0x14-0x1A */
        if (offset >= 0x14 && offset <= 0x1A)
            return vera_l1[offset - 0x14];
        if (offset == 0x1B) return vera_audio_ctrl_read();
        if (offset == 0x1C) return vera_audio_rate;
        if (offset == 0x1D) return 0x00;    /* AUDIO_DATA write-only */
        if (offset == 0x1E) {
            UBYTE value = vera_spi_data;

            if (!no_side_effects && vera_spi_autotx && vera_spi_ss && !vera_spi_busy)
                vera_spi_begin_transfer(0xFFu);
            return value;
        }
        if (offset == 0x1F) return vera_spi_ctrl_read();
        return 0xFF;
    }
}

static void vera_write_reg(int offset, UBYTE byte)
{
    int addrsel = vera_ctrl & 0x01;
    int dcsel   = (vera_ctrl >> 1) & 0x3F;
    int rerender_midline = FALSE;

    switch (offset) {
    case 0x00:
        if (fx_2bit_polygon_pixels && fx_4bit_mode && fx_addr1_mode == FX_MODE_POLY_FILL && addrsel == 1) {
            fx_2bit_poke_mode = TRUE;
            vera_addr_l[1] = (vera_addr_l[1] & 0xFCu) | (byte & 0x03u);
        } else {
            fx_2bit_poke_mode = FALSE;
            vera_addr_l[addrsel] = byte;
            if (addrsel == 1)
                fx_16bit_hop_start_index = byte & 0x03u;
        }
        vera_refresh_prefetch(addrsel);
        break;
    case 0x01:
        vera_addr_m[addrsel] = byte;
        vera_refresh_prefetch(addrsel);
        break;
    case 0x02:
        vera_addr_h[addrsel] = byte;
        vera_refresh_prefetch(addrsel);
        break;
    case 0x03:  /* DATA0 — VRAM write through port 0 */
        if (fx_2bit_poke_mode && fx_addr1_mode != FX_MODE_NORMAL) {
            vera_fx_2bit_poke(byte);
            rerender_midline = TRUE;
            break;
        }
        vera_fx_write_data(VERA_FULL_ADDR(0), vera_fx_addr_nibble(0), byte);
        vera_advance(0);
        vera_refresh_prefetch(0);
        rerender_midline = TRUE;
        break;
    case 0x04:  /* DATA1 — VRAM write through port 1 */
        if (fx_2bit_poke_mode && fx_addr1_mode != FX_MODE_NORMAL) {
            vera_fx_2bit_poke(byte);
            rerender_midline = TRUE;
            break;
        }
        vera_fx_write_data(VERA_FULL_ADDR(1), vera_fx_addr_nibble(1), byte);
        if (fx_addr1_mode == FX_MODE_LINE_DRAW) {
            int step0 = vera_step_lut[vera_fx_increment_index(0)];
            int index0 = vera_fx_increment_index(0);
            int32_t dx = (int32_t)fx_pixel_incr_x << (fx_pixel_incr_x_times_32 ? 4 : 11);

            fx_pixel_pos_x += dx;
            if (fx_pixel_pos_x & 0x100000) { /* overflow */
                if (fx_4bit_mode && vera_fx_nibble_increment(0)) {
                    ULONG addr1 = VERA_FULL_ADDR(1);

                    if (vera_fx_addr_nibble(1)) {
                        if ((index0 & 1) == 0)
                            addr1 = (addr1 + 1u) & 0x1FFFFu;
                        vera_fx_set_addr_nibble(1, 0);
                    } else {
                        if (index0 & 1)
                            addr1 = (addr1 - 1u) & 0x1FFFFu;
                        vera_fx_set_addr_nibble(1, 1);
                    }
                    vera_set_full_addr(1, addr1);
                }
                vera_advance_custom(1, step0);
                fx_pixel_pos_x &= ~0x100000;
            }
        } else if (fx_addr1_mode == FX_MODE_POLY_FILL) {
        } else if (fx_addr1_mode == FX_MODE_AFFINE) {
            /* Implementation of affine tilemap lookup would go here */
        } else {
            vera_advance(1);
        }
        vera_refresh_prefetch(1);
        rerender_midline = TRUE;
        break;
    case 0x05:  /* CTRL */
        if (byte & 0x80u) {
            vera_chip_reset("CTRL_REG_RESET_BIT");      /* RESET bit: soft-reset, VRAM intact */
        } else {
            vera_ctrl = byte & 0x7Fu;   /* keep ADDRSEL + DCSEL (6 bits) */
        }
        break;
    case 0x06:
        vera_irqline = (vera_irqline & 0x00FFu) | (UWORD)((byte & 0x80u) << 1);
        vera_ien = byte & 0x0Fu;
        vera_update_irq();
        break;
    case 0x07:  /* ISR — writing 1 clears corresponding status bit */
        vera_isr &= (UBYTE)~byte;
        vera_update_irq();
        break;
    case 0x08:
        vera_irqline = (vera_irqline & 0x0100u) | byte;
        break;
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C) {
            vera_dc[dcsel][offset - 0x09] = byte;
            switch (dcsel) {
            case 0x00:
                if (offset == 0x09)
                    vera_dc[0][0] = byte & 0x7Fu;
                break;
            case 0x02:
                if (offset == 0x09) {
                    fx_transparency_enabled = (byte >> 7) & 1;
                    fx_cache_write_enabled = (byte >> 6) & 1;
                    fx_cache_fill_enabled = (byte >> 5) & 1;
                    fx_one_byte_cache_cycling = (byte >> 4) & 1;
                    fx_16bit_hop = (byte >> 3) & 1;
                    fx_4bit_mode = (byte >> 2) & 1;
                    fx_addr1_mode = byte & 3;
                } else if (offset == 0x0A) {
                    fx_tiledata_base_address = byte >> 2;
                    fx_apply_clip = (byte >> 1) & 1;
                    fx_2bit_polygon_pixels = byte & 1;
                } else if (offset == 0x0B) {
                    fx_map_base_address = byte >> 2;
                    fx_map_size = byte & 3;
                } else if (offset == 0x0C) {
                    fx_add_or_sub = (byte >> 5) & 1;
                    fx_mult_enabled = (byte >> 4) & 1;
                    fx_accumulate = (byte >> 6) & 1;
                    fx_cache_byte_index = (byte >> 2) & 3;
                    fx_cache_nibble_index = (byte >> 1) & 1;
                    fx_cache_increment_mode = byte & 1;
                    if (byte & 0x40) {
                        int32_t m_result = (int16_t)((vera_fx_cache_get_byte(1) << 8) | vera_fx_cache_get_byte(0)) *
                                           (int16_t)((vera_fx_cache_get_byte(3) << 8) | vera_fx_cache_get_byte(2));
                        if (fx_add_or_sub)
                            fx_mult_accumulator -= m_result;
                        else
                            fx_mult_accumulator += m_result;
                    }
                    if (byte & 0x80)
                        fx_mult_accumulator = 0;
                }
                break;
            case 0x03:
                if (offset == 0x09) fx_pixel_incr_x = (fx_pixel_incr_x & 0x7F00) | byte;
                else if (offset == 0x0A) {
                    fx_pixel_incr_x = (fx_pixel_incr_x & 0x00FF) | ((byte & 0x7F) << 8);
                    fx_pixel_incr_x_times_32 = (byte >> 7) & 1;
                    if (fx_addr1_mode == FX_MODE_LINE_DRAW || fx_addr1_mode == FX_MODE_POLY_FILL)
                        fx_pixel_pos_x = (fx_pixel_pos_x & ~0x1FF) | 256;
                    if (fx_addr1_mode == FX_MODE_LINE_DRAW) fx_pixel_pos_x &= ~(1 << 9);
                } else if (offset == 0x0B) fx_pixel_incr_y = (fx_pixel_incr_y & 0x7F00) | byte;
                else if (offset == 0x0C) {
                    fx_pixel_incr_y = (fx_pixel_incr_y & 0x00FF) | ((byte & 0x7F) << 8);
                    fx_pixel_incr_y_times_32 = (byte >> 7) & 1;
                    if (fx_addr1_mode == FX_MODE_LINE_DRAW || fx_addr1_mode == FX_MODE_POLY_FILL)
                        fx_pixel_pos_y = (fx_pixel_pos_y & ~0x1FF) | 256;
                }
                break;
            case 0x04:
                if (offset == 0x09) fx_pixel_pos_x = (fx_pixel_pos_x & 0x0E01FF) | (byte << 9);
                else if (offset == 0x0A) fx_pixel_pos_x = (fx_pixel_pos_x & 0x01FE00) | ((byte & 7) << 17) | (byte >> 7);
                else if (offset == 0x0B) fx_pixel_pos_y = (fx_pixel_pos_y & 0x0E01FF) | (byte << 9);
                else if (offset == 0x0C) fx_pixel_pos_y = (fx_pixel_pos_y & 0x01FE00) | ((byte & 7) << 17) | (byte >> 7);
                break;
            case 0x05:
                if (offset == 0x09) fx_pixel_pos_x = (fx_pixel_pos_x & ~0x1FE) | (byte << 1);
                else if (offset == 0x0A) fx_pixel_pos_y = (fx_pixel_pos_y & ~0x1FE) | (byte << 1);
                break;
            case 0x06:
                if (offset == 0x09) ib_cache32 = (ib_cache32 & 0xFFFFFF00) | byte;
                else if (offset == 0x0A) ib_cache32 = (ib_cache32 & 0xFFFF00FF) | (byte << 8);
                else if (offset == 0x0B) ib_cache32 = (ib_cache32 & 0xFF00FFFF) | (byte << 16);
                else if (offset == 0x0C) ib_cache32 = (ib_cache32 & 0x00FFFFFF) | (byte << 24);
                break;
            }
        }
        /* Layer 0 fixed: offsets 0x0D-0x13 */
        else if (offset >= 0x0D && offset <= 0x13) {
            vera_l0[offset - 0x0D] = byte;
        }
        /* Layer 1 fixed: offsets 0x14-0x1A */
        else if (offset >= 0x14 && offset <= 0x1A) {
            vera_l1[offset - 0x14] = byte;
        }
        else if (offset == 0x1B) { vera_audio_ctrl_write(byte); }
        else if (offset == 0x1C) {
            vera_audio_rate = (byte > 128u) ? (UBYTE)(256u - (unsigned int)byte) : byte;
            vera_audio_update_aflow();
        }
        else if (offset == 0x1D) { vera_audio_data_write(byte); }
        else if (offset == 0x1E) { vera_spi_begin_transfer(byte); }
        else if (offset == 0x1F) {
            vera_spi_ctrl = byte & 0x07u;
            vera_spi_autotx = (byte & 0x04u) != 0u;
            if (vera_spi_ss != ((byte & 0x01u) != 0u)) {
                vera_spi_ss = (byte & 0x01u) != 0u;
                vera_sd_select(vera_spi_ss);
                if (!vera_spi_ss) {
                    vera_spi_busy = FALSE;
                    vera_spi_cycles_until_done = 0;
                    vera_spi_data = 0xFF;
                }
            }
        }
        break;
    }

    if (vera_midline_affects_video(offset, dcsel))
        rerender_midline = TRUE;

    if (rerender_midline)
        vera_midline_rerender_tail(offset, dcsel);
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
        } else if (strcmp(argv[i], "-verax16-sdcard") == 0 && i + 1 < *argc) {
            Util_strlcpy(verax16_sdcard_filename, argv[i + 1],
                         sizeof(verax16_sdcard_filename));
            i++;
            recognized = TRUE;
        }

        if (!recognized) {
            if (strcmp(argv[i], "-help") == 0) {
                Log_print("\t-verax16           Enable VeraX16 FPGA PBI video card");
                Log_print("\t--use-verax16      Alias for -verax16");
                Log_print("\t-verax16-rom F     OS handler ROM (2KB, $D800-$DFFF)");
                Log_print("\t-verax16-pbi-id N  PBI device bit 0-7 (default: 7, mask=$80)");
                Log_print("\t-verax16-sdcard F  Raw SD image file (for example from dd) exposed through VERA SPI");
                Log_print("\t                    The Atari driver handles MBR/GPT/filesystems on top of the raw 512-byte blocks");
            }
            argv[j++] = argv[i];
        }
    }
    *argc = j;

    if (!PBI_VERAX16_enabled)
        return TRUE;

    /* Power-on: clear VRAM and reset all registers */
    memset(vera_vram, 0, sizeof(vera_vram));

    /* Pre-load the standard Commander X16 default palette.
     * Format: byte0 = GGGGBBBB, byte1 = 0000RRRR  (12-bit colour). */
    {
        static const UWORD default_palette[256] = {
            0x000,0xfff,0x800,0xafe,0xc4c,0x0c5,0x00a,0xee7,0xd85,0x640,0xf77,0x333,0x777,0xaf6,0x08f,0xbbb,
            0x000,0x111,0x222,0x333,0x444,0x555,0x666,0x777,0x888,0x999,0xaaa,0xbbb,0xccc,0xddd,0xeee,0xfff,
            0x211,0x433,0x644,0x866,0xa88,0xc99,0xfbb,0x211,0x422,0x633,0x844,0xa55,0xc66,0xf77,0x200,0x411,
            0x611,0x822,0xa22,0xc33,0xf33,0x200,0x400,0x600,0x800,0xa00,0xc00,0xf00,0x221,0x443,0x664,0x886,
            0xaa8,0xcc9,0xfeb,0x211,0x432,0x653,0x874,0xa95,0xcb6,0xfd7,0x210,0x431,0x651,0x862,0xa82,0xca3,
            0xfc3,0x210,0x430,0x640,0x860,0xa80,0xc90,0xfb0,0x121,0x343,0x564,0x786,0x9a8,0xbc9,0xdfb,0x121,
            0x342,0x463,0x684,0x8a5,0x9c6,0xbf7,0x120,0x241,0x461,0x582,0x6a2,0x8c3,0x9f3,0x120,0x240,0x360,
            0x480,0x5a0,0x6c0,0x7f0,0x121,0x343,0x465,0x686,0x8a8,0x9ca,0xbfc,0x121,0x242,0x364,0x485,0x5a6,
            0x6c8,0x7f9,0x020,0x141,0x162,0x283,0x2a4,0x3c5,0x3f6,0x020,0x041,0x061,0x082,0x0a2,0x0c3,0x0f3,
            0x122,0x344,0x466,0x688,0x8aa,0x9cc,0xbff,0x122,0x244,0x366,0x488,0x5aa,0x6cc,0x7ff,0x022,0x144,
            0x166,0x288,0x2aa,0x3cc,0x3ff,0x022,0x044,0x066,0x088,0x0aa,0x0cc,0x0ff,0x112,0x334,0x456,0x668,
            0x88a,0x9ac,0xbcf,0x112,0x224,0x346,0x458,0x56a,0x68c,0x79f,0x002,0x114,0x126,0x238,0x24a,0x35c,
            0x36f,0x002,0x014,0x016,0x028,0x02a,0x03c,0x03f,0x112,0x334,0x546,0x768,0x98a,0xb9c,0xdbf,0x112,
            0x324,0x436,0x648,0x85a,0x96c,0xb7f,0x102,0x214,0x416,0x528,0x62a,0x83c,0x93f,0x102,0x204,0x306,
            0x408,0x50a,0x60c,0x70f,0x212,0x434,0x646,0x868,0xa8a,0xc9c,0xfbe,0x211,0x423,0x635,0x847,0xa59,
            0xc6b,0xf7d,0x201,0x413,0x615,0x826,0xa28,0xc3a,0xf3c,0x201,0x403,0x604,0x806,0xa08,0xc09,0xf0b
        };
        unsigned int i;

        for (i = 0; i < 256u; i++) {
            UWORD entry = default_palette[i];
            vera_vram[0x1FA00u + i * 2u] = (UBYTE)(entry & 0xFFu);
            vera_vram[0x1FA00u + i * 2u + 1u] = (UBYTE)(entry >> 8);
        }
    }

    vera_chip_reset("INITIAL_SETUP");

    /* Optionally load OS handler ROM */
    if (verax16_rom_filename[0] != '\0') {
        char cwd[FILENAME_MAX];
        const char *cwd_str;

        if (getcwd(cwd, sizeof(cwd)) != NULL)
            cwd_str = cwd;
        else
            cwd_str = "(unknown)";

        verax16_rom = (UBYTE *)Util_malloc(0x800);
        if (!Atari800_LoadImage(verax16_rom_filename, verax16_rom, 0x800)) {
            free(verax16_rom);
            verax16_rom = NULL;
            Log_print("VeraX16: WARNING - ROM not loaded from %s (cwd: %s)",
                      verax16_rom_filename, cwd_str);
            fprintf(stderr,
                    "VeraX16: WARNING - cannot load ROM '%s' (cwd: %s). "
                    "Use an absolute path or a path relative to the launch directory.\n",
                    verax16_rom_filename, cwd_str);
        } else {
            Log_print("VeraX16: ROM loaded from %s", verax16_rom_filename);
        }
    }

    if (verax16_sdcard_filename[0] != '\0')
        vera_sdcard_attach();

    Log_print("VeraX16: PBI video card enabled  device-bit=%d mask=$%02X regs=$D100-$D11F VRAM=128KB",
              verax16_pbi_num, verax16_pbi_mask);
    return TRUE;
}

void PBI_VERAX16_Exit(void)
{
    if (!PBI_VERAX16_enabled)
        return;
    VERA_VIDEO_Exit();
    vera_sdcard_detach();
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
        memcpy(MEMORY_mem + 0xd800, MEMORY_os + 0x1800, 0x800);
        vera_chip_reset("PBI_RESET");
    }
}

int PBI_VERAX16_ReadConfig(char *string, char *ptr)
{
    if (strcmp(string, "VERAX16_ROM") == 0)
        Util_strlcpy(verax16_rom_filename, ptr, sizeof(verax16_rom_filename));
    else if (strcmp(string, "VERAX16_SDCARD") == 0)
        Util_strlcpy(verax16_sdcard_filename, ptr, sizeof(verax16_sdcard_filename));
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
    fprintf(fp, "VERAX16_SDCARD=%s\n", verax16_sdcard_filename);
    fprintf(fp, "VERAX16_PBI_ID=%d\n", verax16_pbi_num);
}

/* ------------------------------------------------------------------ */
/* PBI bus read ($D100-$D1FE)                                          */
/* ------------------------------------------------------------------ */

int PBI_VERAX16_D1GetByte(UWORD addr, int no_side_effects)
{
    int offset = (int)addr - (int)VERA_REG_BASE;
    if (offset >= 0 && offset < (int)VERA_REG_COUNT) {
        UBYTE val = vera_read_reg(offset, no_side_effects);
        Log_D("PBI_GetByte: %04X -> %02X", addr, val);
        return (int)val;
    }
    if (offset >= 0x20 && offset < 0xA0) /* $D120 - $D19F */
        return (int)vera_pbi_scratchpad[offset - 0x20];
    return PBI_NOT_HANDLED;
}

/* ------------------------------------------------------------------ */
/* PBI bus write ($D100-$D1FE, $D1FF handled separately)               */
/* ------------------------------------------------------------------ */

void PBI_VERAX16_D1PutByte(UWORD addr, UBYTE byte)
{
    int offset = (int)addr - (int)VERA_REG_BASE;
    if (offset >= 0 && offset < (int)VERA_REG_COUNT) {
        Log_D("PBI_PutByte: %04X <- %02X", addr, byte);
        vera_write_reg(offset, byte);
    }
    else if (offset >= 0x20 && offset < 0xA0) /* $D120 - $D19F */
        vera_pbi_scratchpad[offset - 0x20] = byte;
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

    /* Any other D1FF value deselects this device: restore OS math pack */
    verax16_cs = FALSE;
    memcpy(MEMORY_mem + 0xd800, MEMORY_os + 0x1800, 0x800);
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
}

void PBI_VERAX16_Scanline(void)
{
    if (!PBI_VERAX16_enabled)
        return;

    vera_scanline_accum += VERA_SCANLINES_PER_FRAME;
    while (vera_scanline_accum >= (unsigned int)Atari800_tv_mode) {
        vera_advance_scanline_once();
        vera_scanline_accum -= (unsigned int)Atari800_tv_mode;
    }
    vera_midline_sync_cursor();
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
    memcpy(s->dc, vera_dc, sizeof(s->dc));
    memcpy(s->l0,  vera_l0,    7);
    memcpy(s->l1,  vera_l1,    7);
}

const UBYTE *PBI_VERAX16_GetVRAMPtr(void)
{
    return vera_vram;
}

void PBI_VERAX16_SoundInit(unsigned int playback_freq, unsigned int channels, int sample_size)
{
    vera_host_playback_freq = playback_freq;
    vera_host_channels = channels;
    vera_host_sample_size = sample_size;
}

void PBI_VERAX16_SoundMix(void *buffer, int sndn, unsigned int channels, int sample_size)
{
    typedef struct {
        UWORD freq;
        UBYTE vol_route;
        UBYTE wave_pulse;
    } VERA_PSGVoice;

    VERA_PSGVoice voices[VERA_PSG_VOICE_COUNT];
    int frames;
    int frame;
    int voice;
    UBYTE *buffer8;
    SWORD *buffer16;

    if (!PBI_VERAX16_enabled || buffer == NULL || sndn <= 0 || channels == 0u ||
        vera_host_playback_freq == 0u || (sample_size != 1 && sample_size != 2))
        return;

    if (vera_host_channels != 0u)
        channels = vera_host_channels;
    if (vera_host_sample_size == 1 || vera_host_sample_size == 2)
        sample_size = vera_host_sample_size;

    frames = sndn / (int)channels;
    if (frames <= 0)
        return;

    for (voice = 0; voice < VERA_PSG_VOICE_COUNT; voice++) {
        ULONG base = VERA_PSG_REG_BASE + (ULONG)voice * 4u;
        voices[voice].freq = (UWORD)((UWORD)vera_vram[base] | ((UWORD)vera_vram[base + 1u] << 8));
        voices[voice].vol_route = vera_vram[base + 2u];
        voices[voice].wave_pulse = vera_vram[base + 3u];
    }

    buffer8 = (UBYTE *)buffer;
    buffer16 = (SWORD *)buffer;

    for (frame = 0; frame < frames; frame++) {
        int pcm_left = 0;
        int pcm_right = 0;
        int mix_left;
        int mix_right;

        vera_pcm_render_sample(&pcm_left, &pcm_right);
        mix_left = pcm_left;
        mix_right = pcm_right;

        for (voice = 0; voice < VERA_PSG_VOICE_COUNT; voice++) {
            VERA_PSGState *state = &vera_psg_state[voice];
            UBYTE vol = voices[voice].vol_route & 0x3Fu;
            UBYTE waveform = (UBYTE)(voices[voice].wave_pulse >> 6);
            UBYTE pulse_width = voices[voice].wave_pulse & 0x3Fu;
            UWORD volume = vera_psg_volume_lut[vol];
            uint32_t new_phase;
            uint32_t v = 0;
            int16_t sv;
            int16_t val;

            vera_psg_noise_state = (UWORD)((vera_psg_noise_state << 1) |
                                 (((vera_psg_noise_state >> 1) ^
                                   (vera_psg_noise_state >> 2) ^
                                   (vera_psg_noise_state >> 4) ^
                                   (vera_psg_noise_state >> 15)) & 1u));

            new_phase = (voices[voice].vol_route & 0xC0u) ?
                        ((state->phase + voices[voice].freq) & 0x1FFFFu) : 0u;
            if ((state->phase & 0x10000u) && !(new_phase & 0x10000u))
                state->noiseval = (UWORD)((vera_psg_noise_state >> 1) & 0x3Fu);
            state->phase = new_phase;

            switch (waveform & 0x03u) {
            case 0:
                v = ((state->phase >> 10) > pulse_width) ? 0u : 0x3Fu;
                break;
            case 1:
                v = (state->phase >> 11) ^ ((pulse_width ^ 0x3Fu) & 0x3Fu);
                break;
            case 2:
                v = ((state->phase & 0x10000u) ?
                     (~(state->phase >> 10) & 0x3Fu) :
                     ((state->phase >> 10) & 0x3Fu)) ^ ((pulse_width ^ 0x3Fu) & 0x3Fu);
                break;
            default:
                v = state->noiseval & 0x3Fu;
                break;
            }

            sv = (int16_t)(v ^ 0x20u);
            if (sv & 0x20)
                sv |= (int16_t)0xFFC0u;
            val = (int16_t)(sv * (int16_t)volume);

            if (voices[voice].vol_route & 0x40u)
                mix_left += val >> 3;
            if (voices[voice].vol_route & 0x80u)
                mix_right += val >> 3;
        }

        if (sample_size == 2) {
            int index = frame * (int)channels;
            if (channels == 1u) {
                int mono = (mix_left + mix_right) / 2;
                int mixed = (int)buffer16[index] + mono;
                if (mixed > 32767) mixed = 32767;
                else if (mixed < -32768) mixed = -32768;
                buffer16[index] = (SWORD)mixed;
            }
            else {
                int mixed_left = (int)buffer16[index] + mix_left;
                int mixed_right = (int)buffer16[index + 1] + mix_right;
                if (mixed_left > 32767) mixed_left = 32767;
                else if (mixed_left < -32768) mixed_left = -32768;
                if (mixed_right > 32767) mixed_right = 32767;
                else if (mixed_right < -32768) mixed_right = -32768;
                buffer16[index] = (SWORD)mixed_left;
                buffer16[index + 1] = (SWORD)mixed_right;
            }
        }
        else {
            int index = frame * (int)channels;
            if (channels == 1u) {
                int mono = (mix_left + mix_right) / 2;
                int mixed = (((int)buffer8[index] - 128) << 8) + mono;
                if (mixed > 32767) mixed = 32767;
                else if (mixed < -32768) mixed = -32768;
                buffer8[index] = (UBYTE)((mixed >> 8) + 128);
            }
            else {
                int mixed_left = (((int)buffer8[index] - 128) << 8) + mix_left;
                int mixed_right = (((int)buffer8[index + 1] - 128) << 8) + mix_right;
                if (mixed_left > 32767) mixed_left = 32767;
                else if (mixed_left < -32768) mixed_left = -32768;
                if (mixed_right > 32767) mixed_right = 32767;
                else if (mixed_right < -32768) mixed_right = -32768;
                buffer8[index] = (UBYTE)((mixed_left >> 8) + 128);
                buffer8[index + 1] = (UBYTE)((mixed_right >> 8) + 128);
            }
        }
    }
}


/*
vim:ts=4:sw=4:
*/
