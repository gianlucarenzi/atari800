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
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
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

/* FX coprocessor modes */
#define FX_MODE_NORMAL     0
#define FX_MODE_LINE_DRAW  1
#define FX_MODE_POLY_FILL  2
#define FX_MODE_AFFINE     3

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

/* CTRL: bit[0]=ADDRSEL, bits[6:1]=DCSEL, bit[7]=RESET (self-clearing) */
static UBYTE vera_ctrl    = 0;
static UBYTE vera_ien     = 0;   /* interrupt enable  */
static UBYTE vera_isr     = 0;   /* interrupt status  */
static UBYTE vera_irqline = 0;   /* raster IRQ line (bits 7:0) */

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
#define VERA_PSG_MIX_SCALE     2048.0

static UBYTE vera_pcm_fifo[VERA_AUDIO_FIFO_SIZE];
static unsigned int vera_pcm_fifo_read = 0;
static unsigned int vera_pcm_fifo_write = 0;
static unsigned int vera_pcm_fifo_count = 0;
static int vera_pcm_loop = FALSE;
static unsigned int vera_pcm_loop_base = 0;
static unsigned int vera_pcm_loop_length = 0;
static unsigned int vera_pcm_loop_pos = 0;
static double vera_pcm_phase = 0.0;
static int vera_pcm_current_left = 0;
static int vera_pcm_current_right = 0;
static int vera_pcm_current_valid = FALSE;

static unsigned int vera_host_playback_freq = 0;
static unsigned int vera_host_channels = 1;
static int vera_host_sample_size = 2;

static double vera_psg_phase[VERA_PSG_VOICE_COUNT];
static UWORD vera_psg_noise_lfsr[VERA_PSG_VOICE_COUNT];

/* SPI (stub — not used on the PBI card variant) */
static UBYTE vera_spi_data = 0xFF;
static UBYTE vera_spi_ctrl = 0;

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

/* Advance address port p by its configured step after a DATA access */
static void vera_advance(int p)
{
    ULONG addr = VERA_FULL_ADDR(p);
    int step_idx = (vera_addr_h[p] >> 4) & 0x0F;
    int decr = (vera_addr_h[p] >> 3) & 1;
    
    if (fx_4bit_mode) {
        /* 4-bit mode: nibble increment logic */
        int nibble = (vera_addr_h[p] >> 2) & 1;
        if (nibble) {
            /* Flip nibble */
            vera_addr_h[p] ^= 0x04;
        } else {
            /* Advance address */
            ULONG step = (ULONG)vera_step_lut[step_idx];
            if (decr) addr = (addr - step) & 0x1FFFFu;
            else addr = (addr + step) & 0x1FFFFu;
            vera_addr_l[p] = (UBYTE)(addr & 0xFFu);
            vera_addr_m[p] = (UBYTE)((addr >> 8) & 0xFFu);
            vera_addr_h[p] = (vera_addr_h[p] & 0xF0u) | (UBYTE)((addr >> 16) & 0x01u);
        }
    } else {
        /* Standard 8-bit mode */
        ULONG step = (ULONG)vera_step_lut[step_idx];
        if (step != 0u) {
            if (decr) addr = (addr - step) & 0x1FFFFu;
            else addr = (addr + step) & 0x1FFFFu;
            vera_addr_l[p] = (UBYTE)(addr & 0xFFu);
            vera_addr_m[p] = (UBYTE)((addr >> 8) & 0xFFu);
            vera_addr_h[p] = (vera_addr_h[p] & 0xF0u) | (UBYTE)((addr >> 16) & 0x01u);
        }
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
    if (vera_audio_rate != 0 && vera_pcm_fifo_count < (VERA_AUDIO_FIFO_SIZE / 4u))
        vera_isr |= VERA_AUDIO_AFLOW_MASK;
    else
        vera_isr &= ~VERA_AUDIO_AFLOW_MASK;
    vera_update_irq();
}

static void vera_audio_reset_state(void)
{
    int i;

    vera_pcm_fifo_read = 0;
    vera_pcm_fifo_write = 0;
    vera_pcm_fifo_count = 0;
    vera_pcm_loop = FALSE;
    vera_pcm_loop_base = 0;
    vera_pcm_loop_length = 0;
    vera_pcm_loop_pos = 0;
    vera_pcm_phase = 0.0;
    vera_pcm_current_left = 0;
    vera_pcm_current_right = 0;
    vera_pcm_current_valid = FALSE;
    memset(vera_pcm_fifo, 0, sizeof(vera_pcm_fifo));

    for (i = 0; i < VERA_PSG_VOICE_COUNT; i++) {
        vera_psg_phase[i] = 0.0;
        vera_psg_noise_lfsr[i] = (UWORD)(0xACE1u ^ (UWORD)(i * 0x1111u));
        if (vera_psg_noise_lfsr[i] == 0)
            vera_psg_noise_lfsr[i] = 1;
    }
}

static int vera_pcm_frame_bytes(void)
{
    int bytes = (vera_audio_ctrl & 0x20u) ? 2 : 1;
    if (vera_audio_ctrl & 0x10u)
        bytes *= 2;
    return bytes;
}

static int vera_pcm_can_read_frame(void)
{
    int frame_bytes = vera_pcm_frame_bytes();

    if (vera_pcm_loop)
        return vera_pcm_loop_length >= (unsigned int)frame_bytes;
    return vera_pcm_fifo_count >= (unsigned int)frame_bytes;
}

static int vera_pcm_read_byte(UBYTE *byte)
{
    if (vera_pcm_loop) {
        if (vera_pcm_loop_length == 0u)
            return FALSE;
        *byte = vera_pcm_fifo[(vera_pcm_loop_base + vera_pcm_loop_pos) & VERA_AUDIO_FIFO_MASK];
        vera_pcm_loop_pos++;
        if (vera_pcm_loop_pos >= vera_pcm_loop_length)
            vera_pcm_loop_pos = 0;
        return TRUE;
    }

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
    vera_pcm_loop_base = 0;
    vera_pcm_loop_length = 0;
    vera_pcm_loop_pos = 0;
    vera_pcm_phase = 0.0;
    vera_pcm_current_left = 0;
    vera_pcm_current_right = 0;
    vera_pcm_current_valid = FALSE;
    memset(vera_pcm_fifo, 0, sizeof(vera_pcm_fifo));
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

    if ((byte & 0xC0u) == 0xC0u && vera_pcm_fifo_count > 0u) {
        vera_pcm_loop = TRUE;
        vera_pcm_loop_base = vera_pcm_fifo_read;
        vera_pcm_loop_length = vera_pcm_fifo_count;
        vera_pcm_loop_pos = 0;
    }
    else {
        vera_pcm_loop = FALSE;
        if (byte & 0x80u)
            vera_pcm_reset_fifo();
    }

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

    if (!vera_pcm_can_read_frame())
        return FALSE;

    if (vera_audio_ctrl & 0x20u) {
        if (vera_audio_ctrl & 0x10u) {
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]) ||
                !vera_pcm_read_byte(&raw[2]) || !vera_pcm_read_byte(&raw[3]))
                return FALSE;
            vera_pcm_current_left = (int)(SWORD)((UWORD)raw[0] | ((UWORD)raw[1] << 8));
            vera_pcm_current_right = (int)(SWORD)((UWORD)raw[2] | ((UWORD)raw[3] << 8));
        }
        else {
            SWORD mono;
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]))
                return FALSE;
            mono = (SWORD)((UWORD)raw[0] | ((UWORD)raw[1] << 8));
            vera_pcm_current_left = (int)mono;
            vera_pcm_current_right = (int)mono;
        }
    }
    else {
        if (vera_audio_ctrl & 0x10u) {
            if (!vera_pcm_read_byte(&raw[0]) || !vera_pcm_read_byte(&raw[1]))
                return FALSE;
            vera_pcm_current_left = ((int)(signed char)raw[0]) << 8;
            vera_pcm_current_right = ((int)(signed char)raw[1]) << 8;
        }
        else {
            int mono;
            if (!vera_pcm_read_byte(&raw[0]))
                return FALSE;
            mono = ((int)(signed char)raw[0]) << 8;
            vera_pcm_current_left = mono;
            vera_pcm_current_right = mono;
        }
    }

    vera_pcm_current_valid = TRUE;
    return TRUE;
}

static void vera_pcm_render_sample(int *left, int *right)
{
    double step;
    double volume;
    UBYTE rate = vera_audio_rate > 128u ? 128u : vera_audio_rate;

    *left = 0;
    *right = 0;

    if (vera_host_playback_freq == 0u || rate == 0u)
        return;

    if (!vera_pcm_current_valid && !vera_pcm_load_current_sample())
        return;

    volume = (double)(vera_audio_ctrl & 0x0Fu) / 15.0;
    *left = (int)((double)vera_pcm_current_left * volume * VERA_PCM_MIX_SCALE);
    *right = (int)((double)vera_pcm_current_right * volume * VERA_PCM_MIX_SCALE);

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

static void vera_psg_clock_noise(int voice, int steps)
{
    while (steps-- > 0) {
        UWORD lfsr = vera_psg_noise_lfsr[voice];
        UWORD feedback = (UWORD)(((lfsr >> 0) ^ (lfsr >> 1)) & 1u);
        lfsr = (UWORD)((lfsr >> 1) | (feedback << 15));
        if (lfsr == 0)
            lfsr = 1;
        vera_psg_noise_lfsr[voice] = lfsr;
    }
}

static double vera_psg_wave_sample(int voice, UBYTE waveform, UBYTE pulse_width)
{
    double phase = vera_psg_phase[voice];

    switch (waveform & 0x03u) {
    case 0:
        {
            double duty = (double)(pulse_width + 1u) / 128.0;
            return phase < duty ? 1.0 : -1.0;
        }
    case 1:
        return phase * 2.0 - 1.0;
    case 2:
        return 1.0 - 4.0 * fabs(phase - 0.5);
    default:
        return (vera_psg_noise_lfsr[voice] & 1u) ? 1.0 : -1.0;
    }
}

/* Soft reset: registers to defaults, VRAM contents preserved */
static void vera_chip_reset(const char *caller)
{
    Log_print("VeraX16: VERA chip soft-reset called by: %s", caller);
    memset(vera_addr_l, 0, sizeof(vera_addr_l));
    memset(vera_addr_m, 0, sizeof(vera_addr_m));
    memset(vera_addr_h, 0, sizeof(vera_addr_h));
    vera_ctrl     = 0;
    vera_ien      = 0;
    vera_isr      = 0;
    vera_irqline  = 0;

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

    memset(vera_l0, 0, sizeof(vera_l0));
    memset(vera_l1, 0, sizeof(vera_l1));

    vera_audio_ctrl = 0;
    vera_audio_rate = 0;
    vera_audio_reset_state();
    vera_spi_data   = 0xFF;
    vera_spi_ctrl   = 0;

    vera_update_irq();

    Log_print("VeraX16: VERA chip soft-reset");
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
            if (!no_side_effects)
                vera_advance(0);
            return vera_vram[VERA_FULL_ADDR(0)];
        }
    case 0x04:  /* DATA1 — VRAM read through port 1 */
        {
            if (!no_side_effects)
                vera_advance(1);
            return vera_vram[VERA_FULL_ADDR(1)];
        }
    case 0x05:  /* CTRL — RESET bit always reads 0 */
        return vera_ctrl & 0x7Fu;
    case 0x06:
        return vera_ien;
    case 0x07:
        return vera_isr;
    case 0x08:
        return vera_irqline; /* SCANLINE_L dummy */
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C) {
            switch (dcsel) {
            case 0x00:
                if (offset == 0x09) {
                    /* Current Field (bit 7), etc. */
                    return vera_dc[0][0];
                }
                break;
            case 0x02:
                if (offset == 0x09)
                    return (fx_transparency_enabled << 7) |
                           (fx_cache_write_enabled << 6) |
                           (fx_cache_fill_enabled << 5) |
                           (fx_one_byte_cache_cycling << 4) |
                           (fx_16bit_hop << 3) |
                           (fx_4bit_mode << 2) |
                           (fx_addr1_mode & 0x03);
                break;
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
                break;
            case 0x06:
                /* Accumulator triggers on read */
                if (!no_side_effects) {
                    if (offset == 0x09) { /* reset accum */ }
                    if (offset == 0x0A) { /* accumulate */ }
                }
                break;
            }
            return vera_dc[dcsel][offset - 0x09];
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
        if (offset == 0x1E) return vera_spi_data;
        if (offset == 0x1F) return vera_spi_ctrl;
        return 0xFF;
    }
}

static void vera_write_reg(int offset, UBYTE byte)
{
    int addrsel = vera_ctrl & 0x01;
    int dcsel   = (vera_ctrl >> 1) & 0x3F;

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
        if (fx_4bit_mode) {
            UBYTE val = vera_vram[VERA_FULL_ADDR(0)];
            int nibble = (vera_addr_h[0] >> 2) & 1;
            if (nibble) val = (val & 0x0F) | (byte << 4);
            else        val = (val & 0xF0) | (byte & 0x0F);
            vera_vram[VERA_FULL_ADDR(0)] = val;
        } else {
            vera_vram[VERA_FULL_ADDR(0)] = byte;
        }
        vera_advance(0);
        break;
    case 0x04:  /* DATA1 — VRAM write through port 1 */
        if (fx_addr1_mode == FX_MODE_LINE_DRAW) {
            /* Line Draw: update fixed-point pos, overflow advances ADDR1 */
            int32_t dx = (int32_t)fx_pixel_incr_x << (fx_pixel_incr_x_times_32 ? 4 : 11);
            fx_pixel_pos_x += dx;
            if (fx_pixel_pos_x & 0x100000) { /* overflow */
                vera_advance(1);
                fx_pixel_pos_x &= ~0x100000;
            }
            vera_vram[VERA_FULL_ADDR(1)] = byte;
        } else if (fx_addr1_mode == FX_MODE_POLY_FILL) {
            /* Poly Fill: simple write */
            vera_vram[VERA_FULL_ADDR(1)] = byte;
        } else if (fx_addr1_mode == FX_MODE_AFFINE) {
            /* Affine: advance pos, lookup */
            vera_vram[VERA_FULL_ADDR(1)] = byte;
            /* Implementation of affine tilemap lookup would go here */
        } else {
            vera_vram[VERA_FULL_ADDR(1)] = byte;
            vera_advance(1);
        }
        break;
    case 0x05:  /* CTRL */
        if (byte & 0x80u) {
            vera_chip_reset("CTRL_REG_RESET_BIT");      /* RESET bit: soft-reset, VRAM intact */
        } else {
            vera_ctrl = byte & 0x7Fu;   /* keep ADDRSEL + DCSEL (6 bits) */
        }
        break;
    case 0x06:
        vera_ien = byte;
        vera_update_irq();
        break;
    case 0x07:  /* ISR — writing 1 clears corresponding status bit */
        vera_isr &= ~byte;
        vera_update_irq();
        break;
    case 0x08:
        vera_irqline = byte;
        break;
    default:
        /* DCSEL-muxed: offsets 0x09-0x0C */
        if (offset >= 0x09 && offset <= 0x0C) {
            vera_dc[dcsel][offset - 0x09] = byte;
            switch (dcsel) {
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
                    if (byte & 0x80) { /* reset accum */ }
                    if (byte & 0x40) { /* accumulate */ }
                    fx_add_or_sub = (byte >> 5) & 1;
                    fx_mult_enabled = (byte >> 4) & 1;
                    fx_cache_byte_index = (byte >> 2) & 3;
                    fx_cache_nibble_index = (byte >> 1) & 1;
                    fx_cache_increment_mode = byte & 1;
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
            vera_audio_rate = byte > 128u ? 128u : byte;
            vera_audio_update_aflow();
        }
        else if (offset == 0x1D) { vera_audio_data_write(byte); }
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
        vera_chip_reset("PBI_RESET");
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
    int offset = (int)addr - (int)VERA_REG_BASE;
    if (offset >= 0 && offset < (int)VERA_REG_COUNT) {
        UBYTE val = vera_read_reg(offset, no_side_effects);
        Log_print("PBI_GetByte: %04X -> %02X", addr, val);
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
        Log_print("PBI_PutByte: %04X <- %02X", addr, byte);
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
        vera_update_irq();
        Log_D("VeraX16: VSYNC IRQ");
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
    double psg_step_scale;
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

    psg_step_scale = VERA_DAC_RATE / (131072.0 * (double)vera_host_playback_freq);
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
            UBYTE vol = voices[voice].vol_route & 0x3Fu;
            UBYTE waveform = (UBYTE)(voices[voice].wave_pulse >> 6);
            UBYTE pulse_width = voices[voice].wave_pulse & 0x3Fu;
            double increment;
            int steps;
            double sample;
            int amp;

            if (vol == 0u || voices[voice].freq == 0u)
                continue;
            if ((voices[voice].vol_route & 0xC0u) == 0u)
                continue;

            increment = (double)voices[voice].freq * psg_step_scale;
            vera_psg_phase[voice] += increment;
            steps = (int)vera_psg_phase[voice];
            if (steps > 0) {
                vera_psg_phase[voice] -= (double)steps;
                if (waveform == 3u)
                    vera_psg_clock_noise(voice, steps);
            }

            sample = vera_psg_wave_sample(voice, waveform, pulse_width);
            amp = (int)(sample * ((double)vol / 63.0) * VERA_PSG_MIX_SCALE);

            if (voices[voice].vol_route & 0x40u)
                mix_left += amp;
            if (voices[voice].vol_route & 0x80u)
                mix_right += amp;
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
