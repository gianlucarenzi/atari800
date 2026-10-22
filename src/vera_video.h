/*
 * vera_video.h - SDL2 display output for the VeraX16 FPGA PBI video card
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

#ifndef VERA_VIDEO_H_
#define VERA_VIDEO_H_

/* Called once after SDL is initialised (lazy — first frame also works). */
int  VERA_VIDEO_Init(void);

struct SDL_Window;
struct SDL_Window *VERA_VIDEO_GetWindow(void);

/* Shows the VERA picture now. */
void VERA_VIDEO_Frame(void);

/* The VERA reached its VSYNC (line 480, lines 0-479 rendered): shows the
 * picture at the real time of this point of the Atari frame.  Showing it at
 * the end of the Atari frame instead tears it where the VERA beam is. */
void VERA_VIDEO_VSync(void);

/* A VRAM write to [lo, hi] is about to happen. Returns 1 when every line
 * has to be rendered again (tile data, palette, sprites); otherwise it marks
 * only the lines that read those bytes (map or bitmap rows) and returns 0. */
int  VERA_VIDEO_VramWrite(ULONG lo, ULONG hi);

/* Integer window size multiplier, for high-DPI screens.  Out-of-range
 * values are ignored; an open window is resized at once. */
#define VERA_VIDEO_SCALE_MIN 1
#define VERA_VIDEO_SCALE_MAX 4
void VERA_VIDEO_SetScale(int scale);
int  VERA_VIDEO_GetScale(void);

/* Reset the cached VERA output buffers. */
void VERA_VIDEO_Reset(void);

/* Render one VERA scanline into the cached output buffer. */
void VERA_VIDEO_Scanline(UWORD scanline);

/* Re-render the tail of a VERA scanline after a mid-line register/VRAM change. */
void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart);

/* Redo only the colour lookup of a line tail (palette/border change). */
void VERA_VIDEO_Recolor(UWORD scanline, UWORD xstart);

/* Release all SDL resources. */
void VERA_VIDEO_Exit(void);

/* sprite_renderer.v render time: how many pixels (in screen order) of each of the
 * 128 sprites get drawn on scaled sprite line ly (0 = none). */
void VERA_VIDEO_SpriteDrawCounts(const UBYTE *vram, int ly, UBYTE draw_n[128]);

/* composer.v's scaled vertical counter: the source (layer/sprite) line shown on
 * output line py of the current frame, accumulated line by line. */
int VERA_VIDEO_SourceLine(int py);

#endif /* VERA_VIDEO_H_ */
