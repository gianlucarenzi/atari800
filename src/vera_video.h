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

/* Called once per emulated frame to render the VERA display. */
void VERA_VIDEO_Frame(void);

/* Reset the cached VERA output buffers. */
void VERA_VIDEO_Reset(void);

/* Render one VERA scanline into the cached output buffer. */
void VERA_VIDEO_Scanline(UWORD scanline);

/* Re-render the tail of a VERA scanline after a mid-line register/VRAM change. */
void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart);

/* Release all SDL resources. */
void VERA_VIDEO_Exit(void);

#endif /* VERA_VIDEO_H_ */
