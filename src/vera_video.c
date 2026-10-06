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
        int ly_raw = ((py - ay0) * vscale) >> 7;
        int ly = (ly_raw + vscroll) & 0xFFF;
        int tile_row = (ly / tile_h) % map_h;
        int tile_fy = ly % tile_h;
        ULONG row_lo = map_base + (ULONG)tile_row * (ULONG)map_w * 2u;

        /* the whole map row: horizontal scroll wraps around it */
        if (row_lo < vera_line_lo[layer][py])
            vera_line_lo[layer][py] = row_lo;
        if (row_lo + (ULONG)map_w * 2u - 1u > vera_line_hi[layer][py])
            vera_line_hi[layer][py] = row_lo + (ULONG)map_w * 2u - 1u;

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
        int ly = ((py - ay0) * vscale) >> 7;
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
 * Render sprite layer.
 * ------------------------------------------------------------------ */
static void render_sprites(UBYTE *col_row, UBYTE *z_row, const UBYTE *vram,
                           int py, int xstart, int xend)
{
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
        if (py < y || py >= y + height) continue;

        {
            int sy = py - y;
            int eff_sy = vflip ? (height - 1 - sy) : sy;
            ULONG row_addr = sprite_addr + ((ULONG)eff_sy << (width_log2 - (1 - color_mode)));

            for (int sx = 0; sx < width; sx++) {
                int px = x + sx;
                int eff_sx;
                ULONG pixel_addr;
                UBYTE color_idx;

                if (px < xstart || px >= xend) continue;

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
                }

                if (color_idx != 0 && z_depth > z_row[px]) {
                    z_row[px] = (UBYTE)z_depth;
                    col_row[px] = color_idx;
                }
            }
        }
    }
}

static void render_scanline_range(int py, int xstart, int xend)
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
        vera_line_lo[0][py] = vera_line_lo[1][py] = 0xFFFFFFFFu;
        vera_line_hi[0][py] = vera_line_hi[1][py] = 0;
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
        render_sprites(sprite_col_row, sprite_z_row, vram, py, start, end);

    for (int px = start; px < end; px++) {
        UBYTE color_idx = compose_pixel_index(sprite_z_row[px], sprite_col_row[px],
                                              layer0_row[px], layer1_row[px]);
        fb_row[px] = get_output_color(vram, rs.dc[0][0],
                                      color_idx ? color_idx : rs.dc[0][3]);
    }
}

/* ------------------------------------------------------------------
 * VRAM write: which lines does it change?
 * ------------------------------------------------------------------ */
static int vera_ranges_overlap(ULONG lo, ULONG hi, ULONG a_lo, ULONG a_hi)
{
    return lo <= a_hi && a_lo <= hi;
}

int VERA_VIDEO_VramWrite(ULONG lo, ULONG hi)
{
    VERA_RegSnap rs;
    UBYTE video;
    int layer, py;

    PBI_VERAX16_GetRegSnap(&rs);
    video = rs.dc[0][0];

    /* palette: every pixel; sprites: attributes and data can be anywhere */
    if (vera_ranges_overlap(lo, hi, 0x1FA00u, 0x1FBFFu) || (video & 0x40))
        return 1;

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
    for (py = 0; py < VERA_H; py++) {
        for (layer = 0; layer < 2; layer++) {
            if (vera_ranges_overlap(lo, hi, vera_line_lo[layer][py], vera_line_hi[layer][py])) {
                vera_line_version[py] = 0;
                break;
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
    memset(vera_layer_fb, 0, sizeof(vera_layer_fb));
    memset(vera_sprite_col_fb, 0, sizeof(vera_sprite_col_fb));
    memset(vera_sprite_z_fb, 0, sizeof(vera_sprite_z_fb));
    memset(vera_fb, 0, sizeof(vera_fb));
    memset(vera_line_version, 0, sizeof(vera_line_version));
    memset(vera_line_lo, 0xFF, sizeof(vera_line_lo));
    memset(vera_line_hi, 0, sizeof(vera_line_hi));
}

void VERA_VIDEO_Scanline(UWORD scanline)
{
    unsigned int version = PBI_VERAX16_GetVideoVersion();

    if (scanline < VERA_H) {
        /* Nothing that affects the picture was written since this line was
         * last rendered: the buffers are still valid. */
        if (vera_line_version[scanline] == version)
            return;
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
    for (int px = start; px < end; px++) {
        int inside = (py >= ay0 && py < ay1 && px >= ax0 && px < ax1);
        UBYTE color_idx = 0;

        if (inside)
            color_idx = compose_pixel_index(vera_sprite_z_fb[row + px],
                                            vera_sprite_col_fb[row + px],
                                            vera_layer_fb[0][row + px],
                                            vera_layer_fb[1][row + px]);
        vera_fb[row + px] = get_output_color(vram, rs.dc[0][0],
                                             color_idx ? color_idx : rs.dc[0][3]);
    }
}

void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart)
{
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
void VERA_VIDEO_Exit(void) {}
#endif
