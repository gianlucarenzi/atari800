/*
 * vera_video.c - SDL2 display output for the VeraX16 FPGA PBI video card
 *
 * Renders the VERA chip's video output into a dedicated SDL2 window.
 * Supports tile and bitmap layers with dynamic scaling and color depths.
 *
 * Copyright (C) 2024-2026 Gianluca Renzi <icjtqr@gmail.com>
 * Copyright (C) 2002-2026 Atari800 development team (see DOC/CREDITS)
 */

#include "config.h"
#include "atari.h"
#include "antic.h"
#include "log.h"
#include "pbi_verax16.h"
#include "vera_video.h"
#include <stdio.h>
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
static UBYTE         vera_layer_fb[2][VERA_W * VERA_H];
static UBYTE         vera_sprite_col_fb[VERA_W * VERA_H];
static UBYTE         vera_sprite_z_fb[VERA_W * VERA_H];
/* PBI_VERAX16_GetVideoVersion() value used by the last full render of each line */
static unsigned int  vera_line_version[VERA_H];
/* VRAM each layer read on each line at its last render (one map row or one
 * bitmap row): [lo, hi], lo > hi = nothing. A VRAM write outside the tile
 * data, palette and sprites only re-renders the lines that read it. */
static ULONG         vera_line_lo[2][VERA_H];
static ULONG         vera_line_hi[2][VERA_H];
/* VRAM in 512-byte buckets: for each bucket the lines whose ranges above
 * touch it, so a VRAM write only checks those lines (a stale bit costs a
 * range check, never a missed line) */
#define VERA_BUCKET_SHIFT 9
#define VERA_BUCKETS      (0x20000u >> VERA_BUCKET_SHIFT)
#define VERA_LINE_WORDS   ((VERA_H + 31) / 32)
static Uint32        vera_bucket_lines[VERA_BUCKETS][VERA_LINE_WORDS];
/* VRAM writes seen (VERA_VIDEO_VramWrite): a line copied from the one above
 * needs nothing written since that one was rendered */
static unsigned int  vera_vram_wcount = 0;

/* -DVERA_VIDEO_CHECK: every line is rendered again with the reference code
 * (no fast tile path, no line copy, palette decoded per pixel) and the two
 * are compared; without it vera_ref is the constant 0 */
#ifdef VERA_VIDEO_CHECK
static int           vera_ref = 0;
static unsigned long vera_check_lines = 0, vera_check_bad = 0;
#else
#define vera_ref 0
#endif

/* 0 = not yet tried, 1 = open, -1 = permanently disabled */
static int           vera_open = 0;
/* integer window size multiplier (-verax16-scale), for high-DPI screens */
static int           vera_scale = 1;

/* ------------------------------------------------------------------
 * Public interface.
 * ------------------------------------------------------------------ */
SDL_Window *VERA_VIDEO_GetWindow(void)
{
    return vera_win;
}

/* Largest multiplier <= vera_scale whose window fits the usable area of
 * the display the main window is on. */
static int vera_fit_scale(void)
{
    SDL_Rect r;
    int disp = SDL_VIDEO_wnd != NULL ? SDL_GetWindowDisplayIndex(SDL_VIDEO_wnd) : 0;
    int scale = vera_scale;

    if (disp < 0 || SDL_GetDisplayUsableBounds(disp, &r) != 0)
        return scale;
    while (scale > 1 && (VERA_W * scale > r.w || VERA_H * scale > r.h))
        scale--;
    if (scale != vera_scale)
        Log_print("VERA_VIDEO: scale %d does not fit the %dx%d display, using %d",
                  vera_scale, r.w, r.h, scale);
    return scale;
}

void VERA_VIDEO_SetScale(int scale)
{
    if (scale < VERA_VIDEO_SCALE_MIN || scale > VERA_VIDEO_SCALE_MAX)
        return;
    vera_scale = scale;
    if (vera_win != NULL) {
        int s = vera_fit_scale();
        SDL_SetWindowSize(vera_win, VERA_W * s, VERA_H * s);
    }
}

int VERA_VIDEO_GetScale(void)
{
    return vera_scale;
}

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

    int scale = vera_fit_scale();

    vera_win = SDL_CreateWindow("VeraX16 Video",
                                wx, wy,
                                VERA_W * scale, VERA_H * scale,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!vera_win) {
        Log_print("VERA_VIDEO: SDL_CreateWindow: %s", SDL_GetError());
        vera_open = -1;
        return 0;
    }

    /* no PRESENTVSYNC: the pictures are timed by VERA_VIDEO_VSync(), a wait
     * for the host refresh would stall the emulation in the middle of an
     * Atari frame (the main window already waits for it once a frame) */
    vera_renderer = SDL_CreateRenderer(vera_win, -1, SDL_RENDERER_ACCELERATED);
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

    Log_print("VERA_VIDEO: window opened %dx%d (logical %dx%d, scale %d)",
              VERA_W * scale, VERA_H * scale, VERA_W, VERA_H, scale);
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

static inline Uint32 get_output_color(const UBYTE *vram, UBYTE dc_video, int idx)
{
    Uint32 color;

    if ((dc_video & 0x03u) == 0)
        return 0xFF0000FFu;

    color = get_pal_color(vram, idx);
    if ((dc_video & 0x07u) == 0x06u) {
        Uint32 r = (color >> 16) & 0xFFu;
        Uint32 g = (color >> 8) & 0xFFu;
        Uint32 b = color & 0xFFu;
        Uint32 y = (r + g + b) / 3u;

        return 0xFF000000u | (y << 16) | (y << 8) | y;
    }

    return color;
}

/* colour of each index for the current palette and output mode: rebuilt
 * when the video version (palette writes included) or the mode changed
 * (256 entries instead of one palette decode per output pixel) */
static Uint32       vera_pal_cache[256];
static unsigned int vera_pal_cache_vver = 0;
static UBYTE        vera_pal_cache_mode = 0xFF;

static const Uint32 *vera_palette(const UBYTE *vram, UBYTE dc_video)
{
    unsigned int vver = PBI_VERAX16_GetVideoVersion();
    UBYTE mode = dc_video & 0x07u;

    if (vera_pal_cache_vver != vver || vera_pal_cache_mode != mode) {
        vera_pal_cache_vver = vver;
        vera_pal_cache_mode = mode;
        for (int i = 0; i < 256; i++)
            vera_pal_cache[i] = get_output_color(vram, dc_video, i);
    }
    return vera_pal_cache;
}

static inline UBYTE compose_pixel_index(UBYTE sprite_z, UBYTE sprite_col,
                                        UBYTE layer0_col, UBYTE layer1_col)
{
    switch (sprite_z & 0x03u) {
    case 3:
        return sprite_col ? sprite_col : (layer1_col ? layer1_col : layer0_col);
    case 2:
        return layer1_col ? layer1_col : (sprite_col ? sprite_col : layer0_col);
    case 1:
        return layer1_col ? layer1_col : (layer0_col ? layer0_col : sprite_col);
    default:
        return layer1_col ? layer1_col : layer0_col;
    }
}

/* ------------------------------------------------------------------
 * composer.v's scaled vertical counter, line by line: it restarts at 0 on the
 * first line of the frame >= VSTART and steps (by VSCALE/128) on line y only if
 * line y-1 was inside VSTART..VSTOP, while the index is below 480.  With
 * constant registers this is ((py - VSTART*2) * VSCALE) >> 7; mid-frame
 * changes of VSTART, VSTOP or VSCALE accumulate as on the chip.
 * ------------------------------------------------------------------ */
static int vera_src_fp = 0, vera_src_started = 0, vera_src_upto = -1;
static int vera_src_idx[VERA_H];

int VERA_VIDEO_SourceLine(int py)
{
    VERA_RegSnap rs;

    if (py < 0 || py >= VERA_H)
        return 0;
    if (py < vera_src_upto)
        vera_src_upto = -1;                     /* a new frame */
    if (py > vera_src_upto) {
        PBI_VERAX16_GetRegSnap(&rs);
        while (vera_src_upto < py) {
            int y = ++vera_src_upto;
            int vstart = (int)rs.dc[1][2] * 2, vstop = (int)rs.dc[1][3] * 2;

            if (y == 0)
                vera_src_started = 0;
            if (!vera_src_started && y >= vstart) {
                vera_src_started = 1;
                vera_src_fp = 0;
            } else if (vera_src_started && (vera_src_fp >> 7) < 480
                       && y - 1 >= vstart && y - 1 < vstop) {
                vera_src_fp += rs.dc[0][2];
            }
            vera_src_idx[y] = vera_src_fp >> 7;
        }
    }
    return vera_src_idx[py];
}

/* ------------------------------------------------------------------
 * Render a tile layer.
 * ------------------------------------------------------------------ */
static void render_tile_layer(UBYTE *row, const UBYTE *vram, int layer,
                              const VERA_RegSnap *rs, int py, int ax0, int ay0, int start, int end)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_mapbase = l[1];
    UBYTE l_tilebase = l[2];
    int hscroll = ((int)(l[4] & 0x0F) << 8) | (int)l[3];
    int vscroll = ((int)(l[6] & 0x0F) << 8) | (int)l[5];
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];

    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int t256c = (l_config & 0x08) != 0;
    int text_mode_256c = (color_depth == 0) && t256c;
    int map_w = 32 << ((l_config >> 4) & 0x03);
    int map_h = 32 << ((l_config >> 6) & 0x03);
    int tile_w = (l_tilebase & 1) ? 16 : 8;
    int tile_h = (l_tilebase & 2) ? 16 : 8;
    ULONG tile_base = (ULONG)(l_tilebase & 0xFCu) * 512u;
    ULONG map_base = (ULONG)l_mapbase * 512u;

    {
        int ly_raw = VERA_VIDEO_SourceLine(py);    /* composer.v counter */
        int ly = (ly_raw + vscroll) & 0xFFF;
        int tile_row = (ly / tile_h) % map_h;
        int tile_fy = ly % tile_h;
        ULONG row_lo = map_base + (ULONG)tile_row * (ULONG)map_w * 2u;

        /* the whole map row: horizontal scroll wraps around it */
        if (row_lo < vera_line_lo[layer][py])
            vera_line_lo[layer][py] = row_lo;
        if (row_lo + (ULONG)map_w * 2u - 1u > vera_line_hi[layer][py])
            vera_line_hi[layer][py] = row_lo + (ULONG)map_w * 2u - 1u;

        if (!vera_ref && color_depth != 0) {
            /* 2/4/8 bpp: shifts instead of divisions, x in fixed point, the
             * map entry and the tile row fetched once per tile */
            int bpp_log2 = color_depth;             /* 1, 2, 3: 2, 4, 8 bpp */
            int bpp = 1 << bpp_log2;
            int tw_log2 = (tile_w == 16) ? 4 : 3;
            int row_bytes = (tile_w * bpp) >> 3;
            ULONG tile_bytes = (ULONG)tile_h * (ULONG)row_bytes;
            int ppb_mask = (8 >> bpp_log2) - 1;     /* pixels per byte - 1 */
            int byte_sh = 3 - bpp_log2;             /* pixel -> byte in the row */
            int pix_mask = (1 << bpp) - 1;
            UBYTE t256 = t256c ? 0x80u : 0u;
            ULONG map_row = map_base + (ULONG)tile_row * (ULONG)map_w * 2u;
            int lx_fp = (start - ax0) * hscale + (hscroll << 7);
            int cur_col = -1;
            int hflip = 0;
            UBYTE palhi = 0;
            ULONG gaddr = 0;
            int prev_lx = -1;
            int color_idx = 0;

            for (int px = start; px < end; px++, lx_fp += hscale) {
                int lx = (lx_fp >> 7) & 0xFFF;
                int tile_col, tfx;

                /* DC_HSCALE < 128: the same source pixel on several output
                 * pixels, fetched once */
                if (lx == prev_lx) {
                    if (color_idx != 0)
                        row[px] = (UBYTE)color_idx;
                    continue;
                }
                prev_lx = lx;
                tile_col = (lx >> tw_log2) & (map_w - 1);
                tfx = lx & (tile_w - 1);

                if (tile_col != cur_col) {
                    ULONG map_addr = map_row + (ULONG)tile_col * 2u;
                    UBYTE ch_l = vram[map_addr & 0x1FFFFu];
                    UBYTE attr = vram[(map_addr + 1) & 0x1FFFFu];
                    int vflip = (attr >> 3) & 1;
                    int tfy = vflip ? (tile_h - 1 - tile_fy) : tile_fy;
                    int tile_idx = ch_l | ((int)(attr & 0x03) << 8);

                    cur_col = tile_col;
                    hflip = (attr >> 2) & 1;
                    palhi = (UBYTE)((attr & 0xF0u) | t256);
                    gaddr = tile_base + (ULONG)tile_idx * tile_bytes + (ULONG)tfy * (ULONG)row_bytes;
                }
                if (hflip)
                    tfx = tile_w - 1 - tfx;
                color_idx = (vram[(gaddr + (ULONG)(tfx >> byte_sh)) & 0x1FFFFu]
                             >> ((ppb_mask - (tfx & ppb_mask)) << bpp_log2)) & pix_mask;
                if (color_idx >= 1 && color_idx <= 15)
                    color_idx |= palhi;
                if (color_idx != 0)
                    row[px] = (UBYTE)color_idx;
            }
            return;
        }
        for (int px = start; px < end; px++) {
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

            if (color_depth == 0) {
                int tfy = tile_fy;
                int tfx = tile_fx;
                ULONG glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * (tile_w / 8)) + (tfy * (tile_w / 8)) + (tfx / 8);
                int pixel_bit = (vram[glyph_addr & 0x1FFFFu] >> (7 - (tfx % 8))) & 1;
                if (text_mode_256c)
                    color_idx = pixel_bit ? attr : 0;
                else
                    color_idx = pixel_bit ? (attr & 0x0F) : (attr >> 4);
            }
            else {
                int tfy;
                int tfx;
                int bpp = 1 << color_depth;
                int pixels_per_byte = 8 / bpp;
                ULONG glyph_addr;
                UBYTE b;
                int shift;

                vflip = (attr >> 3) & 1;
                hflip = (attr >> 2) & 1;
                tile_idx |= (int)(attr & 0x03) << 8;

                tfy = vflip ? (tile_h - 1 - tile_fy) : tile_fy;
                tfx = hflip ? (tile_w - 1 - tile_fx) : tile_fx;

                glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * tile_w * bpp / 8) + (tfy * tile_w * bpp / 8) + (tfx * bpp / 8);
                b = vram[glyph_addr & 0x1FFFFu];
                shift = (pixels_per_byte - 1 - (tfx % pixels_per_byte)) * bpp;
                color_idx = (b >> shift) & ((1 << bpp) - 1);

                /* layer_renderer.v "Apply palette offset": for 2/4/8 bpp,
                 * pixels 1-15 take attr[7:4] as high nibble, with bit 7
                 * forced by T256C (Lx_CONFIG bit 3); 8 bpp included. */
                if (color_idx >= 1 && color_idx <= 15)
                    color_idx |= (int)((attr & 0xF0u) | (t256c ? 0x80u : 0u));
            }

            if (color_idx != 0)
                row[px] = (UBYTE)color_idx;
        }
    }
}

/* ------------------------------------------------------------------
 * Render a bitmap layer.
 * ------------------------------------------------------------------ */
static void render_bitmap_layer(UBYTE *row, const UBYTE *vram, int layer,
                                const VERA_RegSnap *rs, int py, int ax0, int ay0, int start, int end)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_tilebase = l[2];
    
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];
    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int bpp = 1 << color_depth;
    int bitmap_w = (l_tilebase & 1) ? 640 : 320;
    /* layer_renderer.v: bitmap data from TILEBASE[7:2] (MAPBASE unused);
     * palette offset from HSCROLL_H[3:0], bit 7 forced by T256C */
    ULONG bitmap_base = (ULONG)(l_tilebase & 0xFCu) * 512u;
    int palette_offset = ((l[4] & 0x0F) << 4) | ((l_config & 0x08) ? 0x80 : 0);

    {
        int ly = VERA_VIDEO_SourceLine(py);        /* composer.v counter */
        ULONG row_bytes = (ULONG)bitmap_w * (ULONG)bpp / 8u;
        ULONG row_lo = bitmap_base + (ULONG)ly * row_bytes;

        if (row_lo < vera_line_lo[layer][py])
            vera_line_lo[layer][py] = row_lo;
        if (row_lo + row_bytes - 1u > vera_line_hi[layer][py])
            vera_line_hi[layer][py] = row_lo + row_bytes - 1u;
        for (int px = start; px < end; px++) {
            int lx = ((px - ax0) * hscale) >> 7;
            ULONG pixel_offset_bits;
            ULONG byte_offset;
            int bit_offset;
            UBYTE b;
            int color_idx;

            if (lx >= bitmap_w) continue;

            pixel_offset_bits = ((ULONG)ly * bitmap_w + lx) * bpp;
            byte_offset = pixel_offset_bits / 8;
            bit_offset = 7 - (pixel_offset_bits % 8) - (bpp - 1);
            if (bit_offset < 0) bit_offset = 0;

            b = vram[(bitmap_base + byte_offset) & 0x1FFFFu];
            color_idx = (b >> bit_offset) & ((1 << bpp) - 1);
            if (color_idx == 0) continue;

            /* no offset at 1 bpp; at 2/4/8 bpp only for pixels 1-15 */
            if (color_depth != 0 && color_idx <= 15)
                color_idx = (color_idx & 0x0F) | palette_offset;
            row[px] = (UBYTE)color_idx;
        }
    }
}

/* ------------------------------------------------------------------
 * Sprite render time (sprite_renderer.v, VERA 48.0.1), timed per clock but
 * event driven.  The finder takes one clock per attribute entry; a sprite on
 * the line waits until the renderer is idle (render_busy = start_render_r ||
 * state != IDLE), then takes 1 clock (SF_START_RENDER).  The renderer takes
 * 1 clock in IDLE, then per 32-bit word 2 clocks of WAIT_FETCH and 1 clock per
 * pixel (pixels past x=640 included).  Everything stops 798 clocks after the
 * line render starts.  VRAM contention (the CPU and the layers have priority)
 * is not modelled: a fetch is taken as granted at once.
 * ------------------------------------------------------------------ */
void VERA_VIDEO_SpriteDrawCounts(const UBYTE *vram, int ly, UBYTE draw_n[128])
{
    enum { SPRITE_CLOCKS = 798 };
    unsigned int tf = 0;                /* finder clock */
    unsigned int rfree = 0;             /* first clock the renderer is free */
    int i;

    memset(draw_n, 0, 128);
    for (i = 0; i < 128 && tf < SPRITE_CLOCKS; i++, tf++) {
        ULONG addr = 0x1FC00u + (ULONG)i * 8u;
        UBYTE attr6 = vram[addr + 6];
        UBYTE attr7 = vram[addr + 7];
        int width = 8 << ((attr7 >> 4) & 0x03);
        int height = 8 << (attr7 >> 6);
        int ppw = (vram[addr + 1] & 0x80) ? 4 : 8;      /* pixels per 32-bit word */
        int y = vram[addr + 4] | ((vram[addr + 5] & 3) << 8);
        unsigned int ts, t;
        int sx, n = 0;

        if (((attr6 >> 2) & 3) == 0) continue;           /* disabled */
        if (((ly - y) & 0x3FF) >= height) continue;      /* sprite_on_line (10-bit ydiff) */
        ts = tf > rfree ? tf : rfree;                    /* SF_START_RENDER */
        if (ts >= SPRITE_CLOCKS) break;
        tf = ts;
        t = ts + 2;                                      /* START, then IDLE */
        for (sx = 0; sx < width; sx++) {
            if (sx % ppw == 0) t += 2;                   /* WAIT_FETCH */
            if (t >= SPRITE_CLOCKS) break;               /* render_time_done */
            t++;
            n++;
        }
        draw_n[i] = (UBYTE)n;
        rfree = t;
    }
}

/* ------------------------------------------------------------------
 * Render sprite layer.
 * ------------------------------------------------------------------ */
static void render_sprites(UBYTE *col_row, UBYTE *z_row, const UBYTE *vram,
                           const VERA_RegSnap *rs, int py, int ax0, int ay0,
                           int xstart, int xend)
{
    /* sprite_renderer.v works in the composer's line buffer, before the
     * output scaler: sprite X / Y are in the same scaled coordinates as the
     * layers (320x240 at DC_HSCALE = DC_VSCALE = 64) */
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];
    int ly;

    UBYTE draw_n[128];

    if (hscale == 0 || vscale == 0)
        return;
    ly = VERA_VIDEO_SourceLine(py);                /* composer.v counter */
    VERA_VIDEO_SpriteDrawCounts(vram, ly, draw_n);

    /* Sprite attributes are at $1FC00-$1FFFF (128 sprites × 8 bytes) */
    for (int i = 0; i < 128; i++) {
        ULONG addr = 0x1FC00u + (ULONG)i * 8u;
        UBYTE attr0 = vram[addr];
        UBYTE attr1 = vram[addr + 1];
        UBYTE x_l = vram[addr + 2];
        UBYTE x_h = vram[addr + 3];
        UBYTE y_l = vram[addr + 4];
        UBYTE y_h = vram[addr + 5];
        UBYTE attr6 = vram[addr + 6];
        UBYTE attr7 = vram[addr + 7];
        int width_log2 = ((attr7 >> 4) & 0x03) + 3;
        int height_log2 = (attr7 >> 6) + 3;
        int width = 1 << width_log2;
        int height = 1 << height_log2;
        int hflip = attr6 & 0x01;
        int vflip = (attr6 >> 1) & 0x01;
        int color_mode = (attr1 >> 7) & 0x01;
        int palette_offset = (attr7 & 0x0F) << 4;
        ULONG sprite_addr = ((ULONG)attr0 << 5) | ((ULONG)(attr1 & 0x0F) << 13);

        int z_depth = (attr6 >> 2) & 3;
        if (z_depth == 0) continue; /* Disabled */

        int x = x_l | ((x_h & 3) << 8);
        int y = y_l | ((y_h & 3) << 8);
        if (x >= 0x400 - width) x -= 0x400;
        if (y >= 0x400 - height) y -= 0x400;
        if (x >= VERA_W || y >= VERA_H || x + width <= 0 || y + height <= 0) continue;
        if (ly < y || ly >= y + height) continue;

        {
            int sy = ly - y;
            int eff_sy = vflip ? (height - 1 - sy) : sy;
            ULONG row_addr = sprite_addr + ((ULONG)eff_sy << (width_log2 - (1 - color_mode)));

            for (int px = xstart; px < xend; px++) {
                int sx = (((px - ax0) * hscale) >> 7) - x;
                int eff_sx;
                ULONG pixel_addr;
                UBYTE color_idx;

                if (sx < 0 || sx >= width || sx >= draw_n[i]) continue;   /* render time */

                eff_sx = hflip ? (width - 1 - sx) : sx;
                if (color_mode == 0) {
                    UBYTE packed;

                    pixel_addr = row_addr + ((ULONG)eff_sx >> 1);
                    packed = vram[pixel_addr & 0x1FFFFu];
                    color_idx = (eff_sx & 1) ? (packed & 0x0Fu) : (packed >> 4);
                    if (color_idx != 0)
                        color_idx = (UBYTE)(color_idx + palette_offset);
                }
                else {
                    pixel_addr = row_addr + (ULONG)eff_sx;
                    color_idx = vram[pixel_addr & 0x1FFFFu];
                    if (color_idx != 0 && color_idx < 16)          /* as at 4 bpp */
                        color_idx = (UBYTE)(color_idx + palette_offset);
                }

                if (color_idx != 0 && z_depth > z_row[px]) {
                    z_row[px] = (UBYTE)z_depth;
                    col_row[px] = color_idx;
                }
            }
        }
    }
}

/* set or clear line py in the buckets of the VRAM range [lo, hi] */
static void vera_bucket_mark(int py, ULONG lo, ULONG hi, int set)
{
    Uint32 bit = 1u << (py & 31);
    int word = py >> 5;
    ULONG b, last;

    if (lo > hi || lo > 0x1FFFFu)
        return;
    if (hi > 0x1FFFFu)
        hi = 0x1FFFFu;
    last = hi >> VERA_BUCKET_SHIFT;
    for (b = lo >> VERA_BUCKET_SHIFT; b <= last; b++) {
        if (set)
            vera_bucket_lines[b][word] |= bit;
        else
            vera_bucket_lines[b][word] &= ~bit;
    }
}

static void vera_line_buckets(int py, int set)
{
    vera_bucket_mark(py, vera_line_lo[0][py], vera_line_hi[0][py], set);
    vera_bucket_mark(py, vera_line_lo[1][py], vera_line_hi[1][py], set);
}

/* the last full line rendered (or copied) and what it was rendered from */
static int           vera_dup_py = -1;
static int           vera_dup_src;
static unsigned int  vera_dup_vver, vera_dup_wcount;
static VERA_RegSnap  vera_dup_rs;

/* the line whose buffers hold a full render, and the registers it used: a
 * mid-line DC_VIDEO / HSTOP change recomposes from those buffers (the chip
 * fetched the layers a line earlier) instead of re-reading VRAM */
static int           vera_start_py = -1;
static VERA_RegSnap  vera_start_rs;

static void render_scanline_range_inner(int py, int xstart, int xend)
{
    VERA_RegSnap rs;
    const UBYTE *vram;
    int ax0;
    int ax1;
    int ay0;
    int ay1;
    int start;
    int end;
    UBYTE *layer0_row;
    UBYTE *layer1_row;
    UBYTE *sprite_col_row;
    UBYTE *sprite_z_row;
    Uint32 *fb_row;
    Uint32 border;

    if (!PBI_VERAX16_enabled)
        return;
    if (py < 0 || py >= VERA_H)
        return;

    PBI_VERAX16_GetRegSnap(&rs);
    vram = PBI_VERAX16_GetVRAMPtr();

    ax0 = (int)rs.dc[1][0] * 4;
    ax1 = (int)rs.dc[1][1] * 4;
    ay0 = (int)rs.dc[1][2] * 2;
    ay1 = (int)rs.dc[1][3] * 2;
    if (ax0 < 0) ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0) ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    start = xstart;
    if (start < 0) start = 0;
    end = xend;
    if (end > VERA_W) end = VERA_W;
    if (end <= start)
        return;

    /* a full render records afresh what the layers read; a partial one
     * (mid-line change) adds to it */
    if (start == 0 && end == VERA_W) {
        unsigned int vver = PBI_VERAX16_GetVideoVersion();

        if (!vera_ref) {
            vera_start_py = py;
            vera_start_rs = rs;
        }
        int inside = (py >= ay0 && py < ay1);
        int vscale = rs.dc[0][2];
        int src = inside ? VERA_VIDEO_SourceLine(py) : -1;

        /* DC_VSCALE < 128 shows a source line on several output lines: when
         * the line above shows the same one and nothing was written since it
         * was rendered (palette writes bump the video version), this line is
         * the same, copy it */
        if (!vera_ref && vera_dup_py >= 0 && py == vera_dup_py + 1 && src == vera_dup_src
            && vver == vera_dup_vver && vera_vram_wcount == vera_dup_wcount
            && memcmp(&rs, &vera_dup_rs, sizeof rs) == 0) {
            size_t o = (size_t)py * VERA_W;

            memcpy(vera_layer_fb[0] + o, vera_layer_fb[0] + o - VERA_W, VERA_W);
            memcpy(vera_layer_fb[1] + o, vera_layer_fb[1] + o - VERA_W, VERA_W);
            memcpy(vera_sprite_col_fb + o, vera_sprite_col_fb + o - VERA_W, VERA_W);
            memcpy(vera_sprite_z_fb + o, vera_sprite_z_fb + o - VERA_W, VERA_W);
            memcpy(vera_fb + o, vera_fb + o - VERA_W, VERA_W * sizeof(Uint32));
            vera_line_lo[0][py] = vera_line_lo[0][py - 1];
            vera_line_lo[1][py] = vera_line_lo[1][py - 1];
            vera_line_hi[0][py] = vera_line_hi[0][py - 1];
            vera_line_hi[1][py] = vera_line_hi[1][py - 1];
            vera_dup_py = py;
            return;
        }
        if (!vera_ref) {
            vera_dup_py = py;
            vera_dup_src = src;
            vera_dup_vver = vver;
            vera_dup_wcount = vera_vram_wcount;
            vera_dup_rs = rs;
        }
        vera_line_lo[0][py] = vera_line_lo[1][py] = 0xFFFFFFFFu;
        vera_line_hi[0][py] = vera_line_hi[1][py] = 0;
    } else if (!vera_ref) {
        vera_dup_py = -1;                   /* partial render: no copy from it */
    }

    layer0_row = vera_layer_fb[0] + (size_t)py * VERA_W;
    layer1_row = vera_layer_fb[1] + (size_t)py * VERA_W;
    sprite_col_row = vera_sprite_col_fb + (size_t)py * VERA_W;
    sprite_z_row = vera_sprite_z_fb + (size_t)py * VERA_W;
    fb_row = vera_fb + (size_t)py * VERA_W;

    memset(layer0_row + start, 0, (size_t)(end - start));
    memset(layer1_row + start, 0, (size_t)(end - start));
    memset(sprite_col_row + start, 0, (size_t)(end - start));
    memset(sprite_z_row + start, 0, (size_t)(end - start));

    border = get_output_color(vram, rs.dc[0][0], rs.dc[0][3]);
    for (int px = start; px < end; px++)
        fb_row[px] = border;

    if (py < ay0 || py >= ay1)
        return;
    if (start < ax0) start = ax0;
    if (end > ax1) end = ax1;
    if (end <= start)
        return;

    /* Lx_CONFIG bit 3 is T256C (attr_mode in layer_renderer.v), not an
     * affine mode: VERA has no affine layer.  Bit 2 selects bitmap mode. */
    if (rs.dc[0][0] & 0x10) {
        if (rs.l0[0] & 0x04)
            render_bitmap_layer(layer0_row, vram, 0, &rs, py, ax0, ay0, start, end);
        else
            render_tile_layer(layer0_row, vram, 0, &rs, py, ax0, ay0, start, end);
    }
    if (rs.dc[0][0] & 0x20) {
        if (rs.l1[0] & 0x04)
            render_bitmap_layer(layer1_row, vram, 1, &rs, py, ax0, ay0, start, end);
        else
            render_tile_layer(layer1_row, vram, 1, &rs, py, ax0, ay0, start, end);
    }
    if (rs.dc[0][0] & 0x40)
        render_sprites(sprite_col_row, sprite_z_row, vram, &rs, py, ax0, ay0, start, end);

    {
        const Uint32 *pal = vera_palette(vram, rs.dc[0][0]);
        UBYTE bg = rs.dc[0][3];

        for (int px = start; px < end; px++) {
            UBYTE color_idx = compose_pixel_index(sprite_z_row[px], sprite_col_row[px],
                                                  layer0_row[px], layer1_row[px]);
            fb_row[px] = vera_ref ? get_output_color(vram, rs.dc[0][0], color_idx ? color_idx : bg)
                                  : pal[color_idx ? color_idx : bg];
        }
    }
}

/* the line's ranges change while it renders: keep the buckets in step */
static void render_scanline_range(int py, int xstart, int xend)
{
    if (py < 0 || py >= VERA_H)
        return;
    vera_line_buckets(py, 0);
    render_scanline_range_inner(py, xstart, xend);
#ifdef VERA_VIDEO_CHECK
    {
        static UBYTE l0[VERA_W], l1[VERA_W], sc[VERA_W], sz[VERA_W];
        static Uint32 fb[VERA_W];
        size_t o = (size_t)py * VERA_W;

        memcpy(l0, vera_layer_fb[0] + o, VERA_W);
        memcpy(l1, vera_layer_fb[1] + o, VERA_W);
        memcpy(sc, vera_sprite_col_fb + o, VERA_W);
        memcpy(sz, vera_sprite_z_fb + o, VERA_W);
        memcpy(fb, vera_fb + o, sizeof fb);
        vera_ref = 1;
        render_scanline_range_inner(py, xstart, xend);
        vera_ref = 0;
        ++vera_check_lines;
        if (memcmp(l0, vera_layer_fb[0] + o, VERA_W) || memcmp(l1, vera_layer_fb[1] + o, VERA_W)
            || memcmp(sc, vera_sprite_col_fb + o, VERA_W) || memcmp(sz, vera_sprite_z_fb + o, VERA_W)
            || memcmp(fb, vera_fb + o, sizeof fb)) {
            if (++vera_check_bad <= 20)
                fprintf(stderr, "VERA_VIDEO_CHECK: line %d (%d-%d) differs\n", py, xstart, xend);
        }
        if ((vera_check_lines & 0xFFFFu) == 0)
            fprintf(stderr, "VERA_VIDEO_CHECK: %lu lines, %lu different\n", vera_check_lines, vera_check_bad);
    }
#endif
    vera_line_buckets(py, 1);
}

/* ------------------------------------------------------------------
 * VRAM write: which lines does it change?
 * ------------------------------------------------------------------ */
static int vera_ranges_overlap(ULONG lo, ULONG hi, ULONG a_lo, ULONG a_hi)
{
    return lo <= a_hi && a_lo <= hi;
}

/* ------------------------------------------------------------------
 * Sprite attribute writes: only the lines of the sprite, before and after
 * ------------------------------------------------------------------ */
static UBYTE vera_sprite_pending[128 / 8];
static int   vera_sprite_pending_any = 0;

/* lines whose layer line (scaled) crosses sprite i as VRAM holds it now */
static void vera_sprite_invalidate(int i)
{
    VERA_RegSnap rs;
    const UBYTE *vram = PBI_VERAX16_GetVRAMPtr();
    ULONG a = 0x1FC00u + (ULONG)i * 8u;
    int height = 1 << ((vram[a + 7] >> 6) + 3);
    int y = vram[a + 4] | ((vram[a + 5] & 3) << 8);
    int vscale, ay0, py;

    if (((vram[a + 6] >> 2) & 3) == 0)
        return;                                 /* disabled: draws nothing */
    PBI_VERAX16_GetRegSnap(&rs);
    vscale = rs.dc[0][2];
    ay0 = (int)rs.dc[1][2] * 2;
    if (y >= 0x400 - height) y -= 0x400;
    for (py = ay0; py < VERA_H; py++) {
        int ly = ((py - ay0) * vscale) >> 7;
        if (ly >= y + height)
            break;
        if (ly >= y)
            vera_line_version[py] = 0;
    }
}

/* the new rectangles of the sprites written since the last line: VRAM
 * holds the new attributes by now */
static void vera_sprite_flush(void)
{
    int i;

    vera_sprite_pending_any = 0;
    for (i = 0; i < 128; i++) {
        if (vera_sprite_pending[i >> 3] & (1u << (i & 7))) {
            vera_sprite_pending[i >> 3] &= (UBYTE)~(1u << (i & 7));
            vera_sprite_invalidate(i);
        }
    }
}

int VERA_VIDEO_VramWrite(ULONG lo, ULONG hi)
{
    VERA_RegSnap rs;
    UBYTE video;
    int layer, py, w;
    ULONG b, last;

    vera_vram_wcount++;
    PBI_VERAX16_GetRegSnap(&rs);
    video = rs.dc[0][0];

    /* palette: every pixel */
    if (vera_ranges_overlap(lo, hi, 0x1FA00u, 0x1FBFFu))
        return 1;
    /* sprites: their attributes change the lines of the sprite only (the
     * old rectangle now, the new one once written: vera_sprite_flush);
     * their data can be anywhere */
    if (video & 0x40) {
        if (lo < 0x1FC00u)
            return 1;
        if (lo <= 0x1FFFFu) {
            int i, last_i = (int)(((hi > 0x1FFFFu ? 0x1FFFFu : hi) - 0x1FC00u) >> 3);

            for (i = (int)((lo - 0x1FC00u) >> 3); i <= last_i; i++) {
                vera_sprite_invalidate(i);
                vera_sprite_pending[i >> 3] |= (UBYTE)(1u << (i & 7));
            }
            vera_sprite_pending_any = 1;
        }
    }

    /* tile data of an enabled tile layer: any line can show that tile */
    for (layer = 0; layer < 2; layer++) {
        const UBYTE *l = layer ? rs.l1 : rs.l0;
        int depth, tile_w, tile_h;
        ULONG base, size;

        if (!(video & (0x10 << layer)) || (l[0] & 0x04))
            continue;
        depth = l[0] & 0x03;
        tile_w = (l[2] & 1) ? 16 : 8;
        tile_h = (l[2] & 2) ? 16 : 8;
        base = (ULONG)(l[2] & 0xFCu) * 512u;
        size = (ULONG)(depth ? 1024 : 256) * (ULONG)(tile_w * tile_h * (1 << depth) / 8);
        if (vera_ranges_overlap(lo, hi, base, base + size - 1u))
            return 1;
    }

    /* map or bitmap rows: only the lines that read them (a line that was not
     * rendered since a register change is out of date anyway) */
    if (lo > 0x1FFFFu)
        return 0;
    last = (hi > 0x1FFFFu ? 0x1FFFFu : hi) >> VERA_BUCKET_SHIFT;
    for (b = lo >> VERA_BUCKET_SHIFT; b <= last; b++) {
        for (w = 0; w < VERA_LINE_WORDS; w++) {
            Uint32 m = vera_bucket_lines[b][w];

            while (m != 0u) {
                py = w * 32 + __builtin_ctz(m);
                m &= m - 1u;
                for (layer = 0; layer < 2; layer++) {
                    if (vera_ranges_overlap(lo, hi, vera_line_lo[layer][py], vera_line_hi[layer][py])) {
                        vera_line_version[py] = 0;
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Public: compose and display one VERA frame.
 * ------------------------------------------------------------------ */
void VERA_VIDEO_Reset(void)
{
    vera_src_upto = -1;
    vera_src_started = 0;
    vera_src_fp = 0;
    memset(vera_layer_fb, 0, sizeof(vera_layer_fb));
    memset(vera_sprite_col_fb, 0, sizeof(vera_sprite_col_fb));
    memset(vera_sprite_z_fb, 0, sizeof(vera_sprite_z_fb));
    memset(vera_fb, 0, sizeof(vera_fb));
    memset(vera_line_version, 0, sizeof(vera_line_version));
    memset(vera_line_lo, 0xFF, sizeof(vera_line_lo));
    memset(vera_line_hi, 0, sizeof(vera_line_hi));
    memset(vera_bucket_lines, 0, sizeof(vera_bucket_lines));
    memset(vera_sprite_pending, 0, sizeof(vera_sprite_pending));
    vera_sprite_pending_any = 0;
}

void VERA_VIDEO_Scanline(UWORD scanline)
{
    unsigned int version;

    if (vera_sprite_pending_any)
        vera_sprite_flush();
    version = PBI_VERAX16_GetVideoVersion();

    if (scanline < VERA_H) {
        /* Nothing that affects the picture was written since this line was
         * last rendered: the buffers are still valid. */
        if (vera_line_version[scanline] == version) {
            /* still valid: nothing visible was written, so the registers
               are those it was rendered with */
            vera_start_py = (int)scanline;
            PBI_VERAX16_GetRegSnap(&vera_start_rs);
            return;
        }
        vera_line_version[scanline] = version;
    }
    render_scanline_range((int)scanline, 0, VERA_W);
}

/* Palette/border change mid-line: only the colour lookup is redone from the
 * indices already rendered at line start, as the real chip reads the palette
 * at pixel output time while layer data was fetched a line earlier. */
void VERA_VIDEO_Recolor(UWORD scanline, UWORD xstart)
{
    VERA_RegSnap rs;
    const UBYTE *vram;
    int py = (int)scanline;
    int ax0, ax1, ay0, ay1, start, end;
    size_t row;

    if (!PBI_VERAX16_enabled || py < 0 || py >= VERA_H)
        return;

    PBI_VERAX16_GetRegSnap(&rs);
    vram = PBI_VERAX16_GetVRAMPtr();

    ax0 = (int)rs.dc[1][0] * 4;
    ax1 = (int)rs.dc[1][1] * 4;
    ay0 = (int)rs.dc[1][2] * 2;
    ay1 = (int)rs.dc[1][3] * 2;
    if (ax0 < 0) ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0) ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    start = (int)xstart;
    if (start < 0) start = 0;
    end = VERA_W;
    if (end <= start)
        return;

    row = (size_t)py * VERA_W;
    {
        const Uint32 *pal = vera_palette(vram, rs.dc[0][0]);

        for (int px = start; px < end; px++) {
            int inside = (py >= ay0 && py < ay1 && px >= ax0 && px < ax1);
            UBYTE color_idx = 0;

            if (inside)
                color_idx = compose_pixel_index(vera_sprite_z_fb[row + px],
                                                vera_sprite_col_fb[row + px],
                                                vera_layer_fb[0][row + px],
                                                vera_layer_fb[1][row + px]);
            vera_fb[row + px] = pal[color_idx ? color_idx : rs.dc[0][3]];
        }
    }
}

/* Recompose the tail of line py from its buffers with the composer registers as
 * they are now (composer.v: enables, window, border and output mode act per
 * pixel on the shown line).  Possible when the buffers already hold what the
 * new settings show: same HSCALE and HSTART (the pixel mapping of this
 * renderer), nothing enabled that was off at the line start, the window not
 * wider than then.  Returns FALSE when the tail must be re-rendered. */
static int vera_midline_recompose(int py, int xstart)
{
    VERA_RegSnap rs;
    const VERA_RegSnap *s0 = &vera_start_rs;
    const UBYTE *vram;
    const Uint32 *pal;
    int ax0, ax1, ay0, ay1, oax1, oay0, oay1, inside_now, inside_old;
    UBYTE en, oen;
    size_t row;

    if (py != vera_start_py || py < 0 || py >= VERA_H)
        return FALSE;
    PBI_VERAX16_GetRegSnap(&rs);
    en = rs.dc[0][0] & 0x70u;
    oen = s0->dc[0][0] & 0x70u;
    if (rs.dc[0][1] != s0->dc[0][1] || rs.dc[1][0] != s0->dc[1][0] || (en & ~oen))
        return FALSE;
    ax0 = (int)rs.dc[1][0] * 4;
    ax1 = (int)rs.dc[1][1] * 4;   if (ax1 > VERA_W) ax1 = VERA_W;
    oax1 = (int)s0->dc[1][1] * 4; if (oax1 > VERA_W) oax1 = VERA_W;
    ay0 = (int)rs.dc[1][2] * 2;   ay1 = (int)rs.dc[1][3] * 2;
    oay0 = (int)s0->dc[1][2] * 2; oay1 = (int)s0->dc[1][3] * 2;
    if (ay1 > VERA_H) ay1 = VERA_H;
    if (oay1 > VERA_H) oay1 = VERA_H;
    inside_now = py >= ay0 && py < ay1;
    inside_old = py >= oay0 && py < oay1;
    if (inside_now && (!inside_old || ax1 > oax1))
        return FALSE;

    vram = PBI_VERAX16_GetVRAMPtr();
    pal = vera_palette(vram, rs.dc[0][0]);
    row = (size_t)py * VERA_W;
    for (int px = xstart; px < VERA_W; px++) {
        UBYTE color_idx = 0;

        if (inside_now && px >= ax0 && px < ax1)
            color_idx = compose_pixel_index((en & 0x40u) ? vera_sprite_z_fb[row + px] : 0,
                                            vera_sprite_col_fb[row + px],
                                            (en & 0x10u) ? vera_layer_fb[0][row + px] : 0,
                                            (en & 0x20u) ? vera_layer_fb[1][row + px] : 0);
        vera_fb[row + px] = pal[color_idx ? color_idx : rs.dc[0][3]];
    }
    return TRUE;
}

void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart)
{
    if (vera_midline_recompose((int)scanline, (int)xstart))
        return;
    render_scanline_range((int)scanline, (int)xstart, VERA_W);
}

void VERA_VIDEO_Frame(void)
{
    if (!PBI_VERAX16_enabled) return;

    if (!vera_open_window()) return;

    SDL_UpdateTexture(vera_tex, NULL, vera_fb, VERA_W * sizeof(Uint32));
    SDL_RenderClear(vera_renderer);
    SDL_RenderCopy(vera_renderer, vera_tex, NULL, NULL);
    SDL_RenderPresent(vera_renderer);
}

void VERA_VIDEO_VSync(void)
{
    if (!PBI_VERAX16_enabled) return;

    /* the emulation runs a whole Atari frame in a burst and then sleeps:
     * wait for the real time of this point of the frame, so the VERA
     * pictures come every 1/59.94 s and not in bunches */
    Atari800_SyncPoint((double)ANTIC_ypos / Atari800_tv_mode);
    VERA_VIDEO_Frame();
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
int  VERA_VIDEO_VramWrite(ULONG lo, ULONG hi) { (void)lo; (void)hi; return 1; }
void VERA_VIDEO_SetScale(int scale) { (void)scale; }
int  VERA_VIDEO_GetScale(void) { return 1; }
void VERA_VIDEO_Reset(void) {}
void VERA_VIDEO_Scanline(UWORD scanline) { (void)scanline; }
void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart) { (void)scanline; (void)xstart; }
void VERA_VIDEO_Recolor(UWORD scanline, UWORD xstart) { (void)scanline; (void)xstart; }
void VERA_VIDEO_Frame(void) {}
void VERA_VIDEO_VSync(void) {}
void VERA_VIDEO_Exit(void) {}
#endif
