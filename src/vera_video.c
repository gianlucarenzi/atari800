/*
 * vera_video.c - SDL2 display output for the VeraX16 FPGA PBI video card
 *
 * Renders the VERA chip's video output into a dedicated SDL2 window.
 * Supports tile and bitmap layers with dynamic scaling and color depths.
 *
 * Copyright (C) 2024-2026 Gianluca Renzi <gianlucarenzi@eurek.it>
 * Copyright (C) 2002-2026 Atari800 development team (see DOC/CREDITS)
 */

#include "config.h"
#include "atari.h"
#include "log.h"
#include "pbi_verax16.h"
#include "vera_video.h"
#include <string.h>

#ifdef SDL2

#include <SDL.h>
#include "sdl/video.h"

#define VERA_W 640
#define VERA_H 480

static SDL_Window   *vera_win        = NULL;
static SDL_Renderer *vera_renderer   = NULL;
static SDL_Texture  *vera_tex        = NULL;
static Uint32        vera_fb[VERA_W * VERA_H];
/* 0 = not yet tried, 1 = open, -1 = permanently disabled */
static int           vera_open = 0;

/* ------------------------------------------------------------------
 * Lazy window creation.
 * ------------------------------------------------------------------ */
static int vera_open_window(void)
{
    if (vera_open == 1) return 1;
    if (vera_open == -1) return 0;

    const char *drv = SDL_GetCurrentVideoDriver();
    if (drv && strncmp(drv, "KMSDRM", 6) == 0) {
        Log_print("VERA_VIDEO: %s driver - VERA display window disabled", drv);
        vera_open = -1;
        return 0;
    }

    int wx = SDL_WINDOWPOS_UNDEFINED;
    int wy = SDL_WINDOWPOS_UNDEFINED;

    /* Try to position the VERA window to the right of the main window */
    if (SDL_VIDEO_wnd != NULL) {
        int main_x, main_y, main_w, main_h;
        SDL_GetWindowPosition(SDL_VIDEO_wnd, &main_x, &main_y);
        SDL_GetWindowSize(SDL_VIDEO_wnd, &main_w, &main_h);
        wx = main_x + main_w + 10;
        wy = main_y;
    }

    vera_win = SDL_CreateWindow("VeraX16 Video",
                                wx, wy,
                                VERA_W, VERA_H,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!vera_win) {
        Log_print("VERA_VIDEO: SDL_CreateWindow: %s", SDL_GetError());
        vera_open = -1;
        return 0;
    }

    vera_renderer = SDL_CreateRenderer(vera_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!vera_renderer) {
        Log_print("VERA_VIDEO: SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(vera_win);
        vera_win = NULL;
        vera_open = -1;
        return 0;
    }

    SDL_RenderSetLogicalSize(vera_renderer, VERA_W, VERA_H);

    vera_tex = SDL_CreateTexture(vera_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, VERA_W, VERA_H);
    if (!vera_tex) {
        Log_print("VERA_VIDEO: SDL_CreateTexture: %s", SDL_GetError());
        SDL_DestroyRenderer(vera_renderer);
        SDL_DestroyWindow(vera_win);
        vera_win = NULL;
        vera_renderer = NULL;
        vera_open = -1;
        return 0;
    }

    Log_print("VERA_VIDEO: window opened %dx%d (logical)", VERA_W, VERA_H);
    vera_open = 1;
    return 1;
}

/* ------------------------------------------------------------------
 * Palette conversion.
 * ------------------------------------------------------------------ */
static inline Uint32 get_pal_color(const UBYTE *vram, int idx)
{
    ULONG addr = 0x1FA00u + (ULONG)(idx & 0xFF) * 2u;
    UBYTE g4b4 = vram[addr];
    UBYTE r4 = vram[addr + 1] & 0x0F;
    UBYTE g4 = (g4b4 >> 4) & 0x0F;
    UBYTE b4 = g4b4 & 0x0F;
    return 0xFF000000u |
           ((Uint32)(r4 | (r4 << 4)) << 16) |
           ((Uint32)(g4 | (g4 << 4)) << 8) |
           (Uint32)(b4 | (b4 << 4));
}

/* ------------------------------------------------------------------
 * Render a tile layer.
 * ------------------------------------------------------------------ */
static void render_tile_layer(const UBYTE *vram, int layer, const VERA_RegSnap *rs, int ax0, int ay0, int ax1, int ay1)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_mapbase = l[1];
    UBYTE l_tilebase = l[2];
    int hscroll = ((int)(l[4] & 0x0F) << 8) | (int)l[3];
    int vscroll = ((int)(l[6] & 0x0F) << 8) | (int)l[5];
    int hscale = rs->dc0[1];
    int vscale = rs->dc0[2];

    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int map_w = 32 << ((l_config >> 4) & 0x03);
    int map_h = 32 << ((l_config >> 6) & 0x03);
    int tile_w = (l_tilebase & 1) ? 16 : 8;
    int tile_h = (l_tilebase & 2) ? 16 : 8;
    ULONG tile_base = (ULONG)(l_tilebase & 0xFCu) * 512u;
    ULONG map_base = (ULONG)l_mapbase * 512u;

    for (int py = ay0; py < ay1; py++) {
        int ly_raw = ((py - ay0) * vscale) >> 7;
        int ly = (ly_raw + vscroll) & 0xFFF;
        int tile_row = (ly / tile_h) % map_h;
        int tile_fy = ly % tile_h;
        Uint32 *row = vera_fb + py * VERA_W;

        for (int px = ax0; px < ax1; px++) {
            int lx_raw = ((px - ax0) * hscale) >> 7;
            int lx = (lx_raw + hscroll) & 0xFFF;
            int tile_col = (lx / tile_w) % map_w;
            int tile_fx = lx % tile_w;

            ULONG map_addr = map_base + (ULONG)(tile_row * map_w + tile_col) * 2u;
            UBYTE ch_l = vram[map_addr & 0x1FFFFu];
            UBYTE attr = vram[(map_addr + 1) & 0x1FFFFu];
            
            int vflip = 0;
            int hflip = 0;
            int tile_idx = ch_l;
            int color_idx = 0;

            if (color_depth == 0) { // 1 bpp
                // 1bpp: Bits 7:4 BG Color, Bits 3:0 FG Color. No flipping.
                int tfy = tile_fy;
                int tfx = tile_fx;
                ULONG glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * (tile_w / 8)) + (tfy * (tile_w / 8)) + (tfx / 8);
                int pixel_bit = (vram[glyph_addr & 0x1FFFFu] >> (7 - (tfx % 8))) & 1;
                color_idx = pixel_bit ? (attr & 0x0F) : (attr >> 4);
            } else {
                // 2/4/8bpp: Bits 7:4 Palette offset, Bit 3 V-Flip, Bit 2 H-Flip, Bits 1:0 Tile Index 9:8.
                vflip = (attr >> 3) & 1;
                hflip = (attr >> 2) & 1;
                tile_idx |= (int)(attr & 0x03) << 8;
                
                int tfy = vflip ? (tile_h - 1 - tile_fy) : tile_fy;
                int tfx = hflip ? (tile_w - 1 - tile_fx) : tile_fx;
                
                int bpp = 1 << color_depth;
                int pixels_per_byte = 8 / bpp;
                ULONG glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * tile_w * bpp / 8) + (tfy * tile_w * bpp / 8) + (tfx * bpp / 8);
                UBYTE b = vram[glyph_addr & 0x1FFFFu];
                int shift = (pixels_per_byte - 1 - (tfx % pixels_per_byte)) * bpp;
                color_idx = (b >> shift) & ((1 << bpp) - 1);
                
                if (color_idx != 0 && color_depth != 3) { // not 8bpp
                    color_idx += (attr >> 4) << 4;
                }
            }

            if (color_idx == 0) continue;
            row[px] = get_pal_color(vram, color_idx);
        }
    }
}

/* ------------------------------------------------------------------
 * Render a bitmap layer.
 * ------------------------------------------------------------------ */
static void render_bitmap_layer(const UBYTE *vram, int layer, const VERA_RegSnap *rs, int ax0, int ay0, int ax1, int ay1)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_mapbase = l[1];
    UBYTE l_tilebase = l[2];
    
    int hscale = rs->dc0[1];
    int vscale = rs->dc0[2];
    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int bpp = 1 << color_depth;
    int bitmap_w = (l_tilebase & 1) ? 640 : 320;
    ULONG bitmap_base = (ULONG)l_mapbase * 512u;
    int palette_offset = (l_tilebase & 0x0F) << 4;

    for (int py = ay0; py < ay1; py++) {
        int ly = ((py - ay0) * vscale) >> 7;
        Uint32 *row = vera_fb + py * VERA_W;
        for (int px = ax0; px < ax1; px++) {
            int lx = ((px - ax0) * hscale) >> 7;
            if (lx >= bitmap_w) continue;
            
            ULONG pixel_offset_bits = ((ULONG)ly * bitmap_w + lx) * bpp;
            ULONG byte_offset = pixel_offset_bits / 8;
            int bit_offset = 7 - (pixel_offset_bits % 8) - (bpp - 1);
            if (bit_offset < 0) bit_offset = 0;
            
            UBYTE b = vram[(bitmap_base + byte_offset) & 0x1FFFFu];
            int color_idx = (b >> bit_offset) & ((1 << bpp) - 1);
            if (color_idx == 0) continue;
            
            if (color_depth != 3) color_idx += palette_offset;
            row[px] = get_pal_color(vram, color_idx);
        }
    }
}

/* ------------------------------------------------------------------
 * Public: compose and display one VERA frame.
 * ------------------------------------------------------------------ */
void VERA_VIDEO_Frame(void)
{
    if (!PBI_VERAX16_enabled) return;

    VERA_RegSnap rs;
    PBI_VERAX16_GetRegSnap(&rs);
    const UBYTE *vram = PBI_VERAX16_GetVRAMPtr();

    /* Active pixel rectangle */
    int ax0 = (int)rs.dc1[0] * 4;
    int ax1 = (int)rs.dc1[1] * 4;
    int ay0 = (int)rs.dc1[2] * 2;
    int ay1 = (int)rs.dc1[3] * 2;
    if (ax0 < 0) ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0) ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    /* Fill border */
    Uint32 border = get_pal_color(vram, rs.dc0[3]);
    for (int i = 0; i < VERA_W * VERA_H; i++) vera_fb[i] = border;

    /* Layers */
    for (int layer = 0; layer < 2; layer++) {
        UBYTE enabled = (layer == 0) ? (rs.dc0[0] & 0x10) : (rs.dc0[0] & 0x20);
        if (enabled && ax1 > ax0 && ay1 > ay0) {
            UBYTE l_config = (layer == 0) ? rs.l0[0] : rs.l1[0];
            if (l_config & 0x04) render_bitmap_layer(vram, layer, &rs, ax0, ay0, ax1, ay1);
            else render_tile_layer(vram, layer, &rs, ax0, ay0, ax1, ay1);
        }
    }

    if (!vera_open_window()) return;

    SDL_UpdateTexture(vera_tex, NULL, vera_fb, VERA_W * sizeof(Uint32));
    SDL_RenderClear(vera_renderer);
    SDL_RenderCopy(vera_renderer, vera_tex, NULL, NULL);
    SDL_RenderPresent(vera_renderer);
}

int VERA_VIDEO_Init(void) { return 1; }

void VERA_VIDEO_Exit(void)
{
    if (vera_tex) { SDL_DestroyTexture(vera_tex); vera_tex = NULL; }
    if (vera_renderer) { SDL_DestroyRenderer(vera_renderer); vera_renderer = NULL; }
    if (vera_win) { SDL_DestroyWindow(vera_win); vera_win = NULL; }
    vera_open = 0;
}

#else /* !SDL2 */
int  VERA_VIDEO_Init(void) { return 1; }
void VERA_VIDEO_Frame(void) {}
void VERA_VIDEO_Exit(void) {}
#endif
