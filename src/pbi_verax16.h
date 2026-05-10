/*
 * pbi_verax16.h - VeraX16 FPGA video card emulation for Atari 8-bit PBI
 *
 * Emulates a VERA (Video Enhanced Retro Adapter) chip from Commander X16,
 * interfaced as an Atari Parallel Bus Interface (PBI) device.
 *
 * Hardware memory map (when card is active via EXSEL/MPD):
 *   $D100-$D11F : VERA registers (32 registers, base mirrors X16 $9F20)
 *   $D1FF       : PBI device ID latch (shared, bit selected by verax16_pbi_num)
 *   $D800-$DFFF : VERA OS handler ROM (2KB, selected via $D1FF latch)
 *
 * Copyright (C) 2024 Gianluca Renzi <gianlucarenzi@eurek.it>
 * Copyright (C) 2002-2024 Atari800 development team (see DOC/CREDITS)
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

#ifndef PBI_VERAX16_H_
#define PBI_VERAX16_H_

#include "atari.h"
#include <stdio.h>

/* Lifecycle */
int  PBI_VERAX16_Initialise(int *argc, char *argv[]);
void PBI_VERAX16_Exit(void);
void PBI_VERAX16_Reset(void);

/* Config file support */
int  PBI_VERAX16_ReadConfig(char *string, char *ptr);
void PBI_VERAX16_WriteConfig(FILE *fp);

/* PBI bus dispatch */
int  PBI_VERAX16_D1GetByte(UWORD addr, int no_side_effects);
void PBI_VERAX16_D1PutByte(UWORD addr, UBYTE byte);
int  PBI_VERAX16_D1ffPutByte(UBYTE byte);
int  PBI_VERAX16_D1ffGetByte(void);  /* IRQ status bit for $D1FF read */

/* Called once per display frame to generate VSYNC IRQ if enabled */
void PBI_VERAX16_VSync(void);

/* Direct VRAM access for debugging/state inspection */
UBYTE PBI_VERAX16_GetVRAM(ULONG addr);
void  PBI_VERAX16_PutVRAM(ULONG addr, UBYTE byte);

/* Register snapshot for the VERA video renderer */
typedef struct {
    UBYTE dc[8][4]; /* DC banks: 0=Video/Scale, 1=Start/Stop, 2=FX Ctrl, 3=FX Incr, 4=FX Pos, 5=FX Pos, 6=FX Cache */
    UBYTE l0[7];   /* Layer 0: CONFIG, MAPBASE, TILEBASE, HSCR_L/H, VSCR_L/H */
    UBYTE l1[7];   /* Layer 1: same                                         */
} VERA_RegSnap;

void            PBI_VERAX16_GetRegSnap(VERA_RegSnap *s);
const UBYTE    *PBI_VERAX16_GetVRAMPtr(void);

void PBI_VERAX16_SoundInit(unsigned int playback_freq, unsigned int channels, int sample_size);
void PBI_VERAX16_SoundMix(void *buffer, int sndn, unsigned int channels, int sample_size);

extern int PBI_VERAX16_enabled;

#endif /* PBI_VERAX16_H_ */
