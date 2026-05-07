/*
 * vera_video.c - SDL2 display output for the VeraX16 FPGA PBI video card
 *
 * Renders the VERA chip's video output into a dedicated 640x480 SDL2 window.
 * Supports text mode (1bpp tile layers) using VERA VRAM and palette data.
 * The window is created lazily on the first call to VERA_VIDEO_Frame().
 *
 * VERA display geometry (VGA 640x480):
 *   Active area: HSTART*4 .. HSTOP*4 (x), VSTART*2 .. VSTOP*2 (y)
 *   Text mode: 8px wide × 16px tall tiles, 80 cols × 25 rows
 *   Tilemap: VRAM[L1_MAPBASE*512], 2 bytes/cell (char, color)
 *   Charset: VRAM[(L1_TILEBASE&0xFC)*512], 16 bytes/glyph
 *   Palette: VRAM[$1FA00], 2 bytes/entry, format: byte0=GGGGBBBB, byte1=0000RRRR
 *
 * NOTE: SDL2's KMSDRM backend does not support multiple windows; on such
 * systems VERA_VIDEO_Frame() is silently skipped.  X11 and Wayland are
 * fully supported.
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
 */

#include "config.h"
#include "atari.h"
#include "log.h"
#include "pbi_verax16.h"
#include "vera_video.h"
#include <string.h>

#ifdef SDL2

#include <SDL.h>

#define VERA_W 640
#define VERA_H 480

static SDL_Window   *vera_win  = NULL;
static SDL_Renderer *vera_ren  = NULL;
static SDL_Texture  *vera_tex  = NULL;
static Uint32        vera_fb[VERA_W * VERA_H];
/* 0 = not yet tried, 1 = open, -1 = permanently disabled */
static int           vera_open = 0;

/* ------------------------------------------------------------------
 * Lazy window creation.
 * Returns 1 if the window is ready, 0 otherwise.
 * Sets vera_open = -1 to permanently disable on unsupported backends.
 * ------------------------------------------------------------------ */
static int vera_open_window(void)
{
    if (vera_open == 1)
        return 1;
    if (vera_open == -1)
        return 0;

    /* KMSDRM is a single-display backend; creating a second SDL window
     * or renderer on it crashes SDL2 with SIGSEGV.  Detect and skip. */
    {
        const char *drv = SDL_GetCurrentVideoDriver();
        if (drv && strncmp(drv, "KMSDRM", 6) == 0) {
            Log_print("VERA_VIDEO: %s driver - VERA display window disabled", drv);
            vera_open = -1;
            return 0;
        }
    }

    vera_win = SDL_CreateWindow("VeraX16 Video",
                                SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED,
                                VERA_W, VERA_H,
                                SDL_WINDOW_HIDDEN);
    if (!vera_win) {
        Log_print("VERA_VIDEO: SDL_CreateWindow: %s", SDL_GetError());
        vera_open = -1;
        return 0;
    }

    vera_ren = SDL_CreateRenderer(vera_win, -1, 0);
    if (!vera_ren) {
        Log_print("VERA_VIDEO: SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(vera_win);
        vera_win = NULL;
        vera_open = -1;
        return 0;
    }

    vera_tex = SDL_CreateTexture(vera_ren,
                                 SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING,
                                 VERA_W, VERA_H);
    if (!vera_tex) {
        Log_print("VERA_VIDEO: SDL_CreateTexture: %s", SDL_GetError());
        SDL_DestroyRenderer(vera_ren);
        SDL_DestroyWindow(vera_win);
        vera_ren = NULL;
        vera_win = NULL;
        vera_open = -1;
        return 0;
    }

    SDL_ShowWindow(vera_win);
    Log_print("VERA_VIDEO: window opened %dx%d", VERA_W, VERA_H);
    vera_open = 1;
    return 1;
}

/* ------------------------------------------------------------------
 * Convert a 4-bit VERA palette entry to 32-bit ARGB8888.
 * Palette format: byte0 = GGGGBBBB, byte1 = 0000RRRR
 * ------------------------------------------------------------------ */
static inline Uint32 pal_to_argb(const UBYTE *vram, int pal_idx)
{
    ULONG pa = 0x1FA00u + (ULONG)(pal_idx & 0xFF) * 2u;
    UBYTE p0 = vram[pa];          /* GGGGBBBB */
    UBYTE p1 = vram[pa + 1u];     /* 0000RRRR */
    int r4 = p1 & 0x0F;
    int g4 = (p0 >> 4) & 0x0F;
    int b4 = p0 & 0x0F;
    return 0xFF000000u |
           (Uint32)((r4 << 4) | r4) << 16 |
           (Uint32)((g4 << 4) | g4) << 8  |
           (Uint32)((b4 << 4) | b4);
}

/* ------------------------------------------------------------------
 * Render one tile/text layer (1bpp mode only for this revision).
 * ------------------------------------------------------------------ */
static void render_text_layer(const UBYTE *vram,
                               UBYTE l_config,   UBYTE l_mapbase,
                               UBYTE l_tilebase,
                               UBYTE l_hscr_l,   UBYTE l_hscr_h,
                               UBYTE l_vscr_l,   UBYTE l_vscr_h,
                               int ax0, int ay0, int ax1, int ay1)
{
    static const int map_sizes[] = { 32, 64, 128, 256 };

    int color_depth = l_config & 0x03;
    int bitmap_mode = (l_config >> 2) & 0x01;
    int map_w = map_sizes[(l_config >> 4) & 0x03];
    int map_h = map_sizes[(l_config >> 6) & 0x03];

    if (bitmap_mode || color_depth != 0)
        return;   /* TODO: bitmap and higher-bpp tile modes */

    ULONG tile_base  = (ULONG)(l_tilebase & 0xFCu) * 512u;
    ULONG map_base   = (ULONG)l_mapbase * 512u;
    int   tile_w     = (l_tilebase & 0x01u) ? 16 : 8;
    int   tile_h     = (l_tilebase & 0x02u) ? 16 : 8;
    int   hscroll    = ((int)(l_hscr_h & 0x01) << 8) | (int)l_hscr_l;
    int   vscroll    = ((int)(l_vscr_h & 0x01) << 8) | (int)l_vscr_l;
    int   tile_bytes = (tile_w / 8) * tile_h;

    for (int py = ay0; py < ay1; py++) {
        int sy       = ((py - ay0) + vscroll) & 0x3FF;
        int tile_row = (sy / tile_h) % map_h;
        int tile_fy  = sy % tile_h;
        Uint32 *row  = vera_fb + py * VERA_W;

        for (int px = ax0; px < ax1; px++) {
            int sx       = ((px - ax0) + hscroll) & 0x3FF;
            int tile_col = (sx / tile_w) % map_w;
            int tile_fx  = sx % tile_w;

            /* 2-byte tilemap entry: char code + colour attribute */
            ULONG map_addr  = map_base +
                              (ULONG)(tile_row * map_w + tile_col) * 2u;
            UBYTE ch        = vram[ map_addr       & 0x1FFFFu];
            UBYTE attr      = vram[(map_addr + 1u) & 0x1FFFFu];

            /* 1bpp glyph row: 1 byte = 8 horizontal pixels */
            ULONG glyph_addr = tile_base +
                               (ULONG)ch * (ULONG)tile_bytes + (ULONG)tile_fy;
            UBYTE glyph_row  = vram[glyph_addr & 0x1FFFFu];
            int   pixel_bit  = (glyph_row >> (7 - tile_fx)) & 1;

            /* attr bits[3:0]=FG index, bits[7:4]=BG index */
            int pal_idx = pixel_bit ? (attr & 0x0Fu) : ((attr >> 4) & 0x0Fu);
            row[px] = pal_to_argb(vram, pal_idx);
        }
    }
}

/* ------------------------------------------------------------------
 * Public: compose and display one VERA frame.
 * ------------------------------------------------------------------ */
void VERA_VIDEO_Frame(void)
{
    if (!PBI_VERAX16_enabled)
        return;

    VERA_RegSnap rs;
    PBI_VERAX16_GetRegSnap(&rs);
    const UBYTE *vram = PBI_VERAX16_GetVRAMPtr();

    /* Active pixel rectangle from Display Composer window registers */
    int ax0 = (int)rs.dc1[0] * 4;   /* HSTART × 4 */
    int ax1 = (int)rs.dc1[1] * 4;   /* HSTOP  × 4 */
    int ay0 = (int)rs.dc1[2] * 2;   /* VSTART × 2 */
    int ay1 = (int)rs.dc1[3] * 2;   /* VSTOP  × 2 */
    if (ax0 < 0)      ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0)      ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    /* Fill framebuffer with border colour */
    Uint32 border = pal_to_argb(vram, rs.dc0[3]);
    int i;
    for (i = 0; i < VERA_W * VERA_H; i++)
        vera_fb[i] = border;

    /* Layer 0 (bit 4 of DC_VIDEO) */
    if ((rs.dc0[0] & 0x10u) && ax1 > ax0 && ay1 > ay0)
        render_text_layer(vram,
                          rs.l0[0], rs.l0[1], rs.l0[2],
                          rs.l0[3], rs.l0[4], rs.l0[5], rs.l0[6],
                          ax0, ay0, ax1, ay1);

    /* Layer 1 (bit 5 of DC_VIDEO) */
    if ((rs.dc0[0] & 0x20u) && ax1 > ax0 && ay1 > ay0)
        render_text_layer(vram,
                          rs.l1[0], rs.l1[1], rs.l1[2],
                          rs.l1[3], rs.l1[4], rs.l1[5], rs.l1[6],
                          ax0, ay0, ax1, ay1);

    if (!vera_open_window())
        return;

    SDL_UpdateTexture(vera_tex, NULL, vera_fb, VERA_W * (int)sizeof(Uint32));
    SDL_RenderClear(vera_ren);
    SDL_RenderCopy(vera_ren, vera_tex, NULL, NULL);
    SDL_RenderPresent(vera_ren);
}

int VERA_VIDEO_Init(void)
{
    return 1;   /* window created lazily on first VERA_VIDEO_Frame() */
}

void VERA_VIDEO_Exit(void)
{
    if (vera_tex) { SDL_DestroyTexture(vera_tex);  vera_tex = NULL; }
    if (vera_ren) { SDL_DestroyRenderer(vera_ren); vera_ren = NULL; }
    if (vera_win) { SDL_DestroyWindow(vera_win);   vera_win = NULL; }
    vera_open = 0;
}

#else /* !SDL2 */

int  VERA_VIDEO_Init(void)  { return 1; }
void VERA_VIDEO_Frame(void) {}
void VERA_VIDEO_Exit(void)  {}

#endif /* SDL2 */

/*
vim:ts=4:sw=4:
*/
