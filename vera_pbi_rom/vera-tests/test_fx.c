/* test_fx.c — VERA FX coprocessor register / behaviour test.
 *
 * Standalone binary: does NOT require VERA.SYS.  Output goes through the
 * standard Atari E: handler (40-column TV display), not VERA.
 *
 * Because no driver owns the VERA display, the test issues a VERA chip
 * soft-reset (CTRL bit 7) at startup to guarantee deterministic FX state.
 * DCSEL=0/1 registers (DC_VIDEO, DC_HSCALE…) are not relevant here.
 *
 * Tests:
 *  1. Reset state: X/Y_POS_S == 0x80 after chip reset (bug-9 fix)
 *  2. FX_CTRL (DCSEL=2) write/read roundtrip
 *  3. FX_TILEBASE / FX_MAPBASE (DCSEL=2) roundtrip
 *  4. FX_MULT (DCSEL=2) write/read roundtrip
 *  5. FX_X/Y_INCR (DCSEL=3) roundtrip
 *  6. FX_X/Y_POS integer part (DCSEL=4) roundtrip
 *  7. FX_X/Y_POS_S subpixel (DCSEL=5) roundtrip + cross-check DCSEL=4
 *  8. FX cache bytes (DCSEL=6) write/read
 *  9. Multiplier: A*B result written to VRAM via DATA0
 * 10. Transparency: zero byte skipped, non-zero written
 *
 * Build: cl65 -t atari --start-addr 0x5000 -o TESTFX.COM vera-tests/test_fx.c
 */

#include <stdio.h>
#include <conio.h>
#include <atari.h>
#include "vera_detect.h"

/* ------------------------------------------------------------------ */
/* VERA PBI register block at $D100                                    */
/* ------------------------------------------------------------------ */
#define VERA_ADDR_L  (*(volatile unsigned char *)0xD100)
#define VERA_ADDR_M  (*(volatile unsigned char *)0xD101)
#define VERA_ADDR_H  (*(volatile unsigned char *)0xD102)
#define VERA_DATA0   (*(volatile unsigned char *)0xD103)
#define VERA_CTRL    (*(volatile unsigned char *)0xD105)

/* DCSEL-muxed registers at $D109-$D10C */
#define VERA_REG09   (*(volatile unsigned char *)0xD109)
#define VERA_REG0A   (*(volatile unsigned char *)0xD10A)
#define VERA_REG0B   (*(volatile unsigned char *)0xD10B)
#define VERA_REG0C   (*(volatile unsigned char *)0xD10C)

/* CTRL DCSEL field values (bits [6:1]) */
#define DCSEL_0   0x00   /* DC_VIDEO … DC_BORDER      (owned by VERA.SYS) */
#define DCSEL_2   0x04   /* FX_CTRL, FX_TILEBASE, FX_MAPBASE, FX_MULT     */
#define DCSEL_3   0x06   /* FX_X_INCR_L/H, FX_Y_INCR_L/H                  */
#define DCSEL_4   0x08   /* FX_X/Y_POS_L/H (integer part)                  */
#define DCSEL_5   0x0A   /* FX_X/Y_POS_S (subpixel), FX_POLY_FILL_L/H     */
#define DCSEL_6   0x0C   /* FX cache bytes / accumulator side-effects       */

/* Atari OS: CRITIC flag — while non-zero, deferred VBI is suppressed */
#define CRITIC (*(volatile unsigned char *)0x0042)

/* Safe VRAM window: bank 0, low addresses — clear of tilemap/font/PSG */
#define TEST_VRAM_MULT  0x00100UL   /* 4-byte area for multiplier test    */
#define TEST_VRAM_TRANS 0x00110UL   /* 1-byte area for transparency test  */

/* ------------------------------------------------------------------ */
/* Test scaffolding                                                     */
/* ------------------------------------------------------------------ */
static unsigned char g_pass = 0;
static unsigned char g_fail = 0;

static void check_b(const char *name, unsigned char got, unsigned char expected)
{
    if (got == expected)
    {
        printf("OK   %-24s $%02X\n", name, (unsigned int)got);
        g_pass++;
    }
    else
    {
        printf("FAIL %-24s got $%02X exp $%02X\n",
               name, (unsigned int)got, (unsigned int)expected);
        g_fail++;
    }
}

/* ------------------------------------------------------------------ */
/* VRAM helpers (no FX modes, ADDRSEL=0)                               */
/* ------------------------------------------------------------------ */

/* ADDR_H auto-increment: bits[7:4]; bank in bit[0] */
#define ADDR_H_NOINC_B0  0x00   /* bank=0, no increment    */
#define ADDR_H_INC1_B0   0x10   /* bank=0, +1 per access   */

static void set_addr0(unsigned long addr, unsigned char addr_h)
{
    VERA_CTRL  = DCSEL_0;   /* ADDRSEL=0, DCSEL=0 */
    VERA_ADDR_L = (unsigned char)(addr & 0xFF);
    VERA_ADDR_M = (unsigned char)((addr >> 8) & 0xFF);
    VERA_ADDR_H = addr_h;
}

static void vram_write(unsigned long addr, unsigned char value)
{
    set_addr0(addr, ADDR_H_NOINC_B0);
    VERA_DATA0 = value;
}

static unsigned char vram_read(unsigned long addr)
{
    set_addr0(addr, ADDR_H_NOINC_B0);
    return VERA_DATA0;
}

/* ------------------------------------------------------------------ */
/* Test 2: FX_CTRL (DCSEL=2) write/read roundtrip                      */
/* ------------------------------------------------------------------ */
static void test_fx_ctrl(void)
{
    unsigned char v;

    printf("\n[2] FX_CTRL roundtrip\n");

    VERA_CTRL  = DCSEL_2;
    /* transparency=1, 4bit=1, mode=NORMAL → 0x84 */
    VERA_REG09 = 0x84;
    v = VERA_REG09;
    check_b("FX_CTRL=0x84 r/b", v, 0x84);

    VERA_REG09 = 0x00;
    v = VERA_REG09;
    check_b("FX_CTRL=0x00 r/b", v, 0x00);

    VERA_CTRL = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 3: FX_TILEBASE / FX_MAPBASE (DCSEL=2) roundtrip               */
/* ------------------------------------------------------------------ */
static void test_tilebase_mapbase(void)
{
    unsigned char v;

    printf("\n[3] FX_TILEBASE / FX_MAPBASE\n");

    VERA_CTRL = DCSEL_2;

    /* FX_TILEBASE: tiledata_base=0x3F, clip=0, 2bit_poly=1 → 0xFD */
    VERA_REG0A = 0xFD;
    v = VERA_REG0A;
    check_b("FX_TILEBASE=0xFD", v, 0xFD);

    /* FX_MAPBASE: map_base=0x24, map_size=3 → (0x24<<2)|3 = 0x93 */
    VERA_REG0B = 0x93;
    v = VERA_REG0B;
    check_b("FX_MAPBASE=0x93", v, 0x93);

    /* Restore */
    VERA_REG0A = 0x00;
    VERA_REG0B = 0x00;
    VERA_CTRL  = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 4: FX_MULT (DCSEL=2) write/read roundtrip                      */
/* Bits 7 (ResetAccum) and 6 (AccumTrigger) are write-only → read 0.  */
/* ------------------------------------------------------------------ */
static void test_fx_mult_reg(void)
{
    unsigned char v;

    printf("\n[4] FX_MULT reg roundtrip\n");

    VERA_CTRL = DCSEL_2;
    /* accumulate=0, add_or_sub=1, mult_enabled=1,
     * cache_byte_index=3, nibble_index=0, inc_mode=1 → 0x3D
     * Expected read: (0<<6)|(1<<5)|(1<<4)|(3<<2)|(0<<1)|1 = 0x3D */
    VERA_REG0C = 0x3D;
    v = VERA_REG0C;
    check_b("FX_MULT=0x3D r/b", v, 0x3D);

    VERA_REG0C = 0x00;
    v = VERA_REG0C;
    check_b("FX_MULT=0x00 r/b", v, 0x00);

    VERA_CTRL = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 5: FX_X/Y_INCR (DCSEL=3) roundtrip                            */
/* ------------------------------------------------------------------ */
static void test_incr(void)
{
    unsigned char vl, vh;

    printf("\n[5] FX_X/Y_INCR roundtrip\n");

    VERA_CTRL = DCSEL_3;

    VERA_REG09 = 0x34;
    VERA_REG0A = 0x01;   /* 32x flag=0, upper 7 bits=0x01 */
    vl = VERA_REG09;
    vh = VERA_REG0A;
    check_b("X_INCR_L=0x34", vl, 0x34);
    check_b("X_INCR_H=0x01", vh, 0x01);

    VERA_REG0B = 0xAB;
    VERA_REG0C = 0x82;   /* 32x flag=1, upper 7 bits=0x02 */
    vl = VERA_REG0B;
    vh = VERA_REG0C;
    check_b("Y_INCR_L=0xAB", vl, 0xAB);
    check_b("Y_INCR_H=0x82", vh, 0x82);

    /* Zero incr to avoid unintended position drift */
    VERA_REG09 = 0x00;
    VERA_REG0A = 0x00;
    VERA_REG0B = 0x00;
    VERA_REG0C = 0x00;
    VERA_CTRL  = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 6: FX_X/Y_POS integer part (DCSEL=4) roundtrip                 */
/* ------------------------------------------------------------------ */
static void test_pos_integer(void)
{
    unsigned char vl, vh;

    printf("\n[6] FX_X/Y_POS integer roundtrip\n");

    VERA_CTRL = DCSEL_4;

    /* X: integer=0x250, sign-ext bit=0 → POS_L=0x50, POS_H=0x02 */
    VERA_REG09 = 0x50;
    VERA_REG0A = 0x02;
    vl = VERA_REG09;
    vh = VERA_REG0A;
    check_b("X_POS_L=0x50", vl, 0x50);
    check_b("X_POS_H=0x02", vh, 0x02);

    /* Y: Y_Pos[7:0]=0xC0, Y_Pos[10:8]=5, Y[-9]=1 → POS_H=0x85 */
    VERA_REG0B = 0xC0;
    VERA_REG0C = 0x85;
    vl = VERA_REG0B;
    vh = VERA_REG0C;
    check_b("Y_POS_L=0xC0", vl, 0xC0);
    check_b("Y_POS_H=0x85", vh, 0x85);

    VERA_CTRL = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 7: FX_X_POS_S subpixel (DCSEL=5) roundtrip + cross-check      */
/* Writing POS_S must not disturb the integer part (DCSEL=4).          */
/* ------------------------------------------------------------------ */
static void test_pos_subpixel(void)
{
    unsigned char vs, vl_after;

    printf("\n[7] FX_X_POS_S subpixel roundtrip\n");

    /* Set a known integer part */
    VERA_CTRL  = DCSEL_4;
    VERA_REG09 = 0x50;   /* X_POS_L */
    VERA_REG0A = 0x02;   /* X_POS_H */

    /* Write subpixel */
    VERA_CTRL  = DCSEL_5;
    VERA_REG09 = 0x3C;
    vs = VERA_REG09;
    check_b("X_POS_S=0x3C", vs, 0x3C);

    /* Integer part must be unchanged */
    VERA_CTRL  = DCSEL_4;
    vl_after   = VERA_REG09;
    check_b("X_POS_L still 0x50", vl_after, 0x50);

    /* Restore positions to zero */
    VERA_CTRL  = DCSEL_4;
    VERA_REG09 = 0x00;
    VERA_REG0A = 0x00;
    VERA_REG0B = 0x00;
    VERA_REG0C = 0x00;
    VERA_CTRL  = DCSEL_5;
    VERA_REG09 = 0x00;
    VERA_REG0A = 0x00;
    VERA_CTRL  = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 8: FX cache bytes (DCSEL=6) write/read                         */
/* Side-effects: reading $D109 resets accumulator; $D10A triggers an   */
/* accumulate step.  Neither affects the byte values returned.         */
/* ------------------------------------------------------------------ */
static void test_cache_rw(void)
{
    unsigned char c0, c1, c2, c3;

    printf("\n[8] FX cache bytes roundtrip\n");

    VERA_CTRL  = DCSEL_6;
    VERA_REG09 = 0xAA;
    VERA_REG0A = 0xBB;
    VERA_REG0B = 0xCC;
    VERA_REG0C = 0xDD;

    c0 = VERA_REG09;   /* side-effect: resets accumulator */
    c1 = VERA_REG0A;   /* side-effect: accumulate step    */
    c2 = VERA_REG0B;
    c3 = VERA_REG0C;

    check_b("cache[0]=0xAA", c0, 0xAA);
    check_b("cache[1]=0xBB", c1, 0xBB);
    check_b("cache[2]=0xCC", c2, 0xCC);
    check_b("cache[3]=0xDD", c3, 0xDD);

    VERA_CTRL = DCSEL_0;
}

/* ------------------------------------------------------------------ */
/* Test 9: Multiplier — cache-write + mult writes A*B to VRAM          */
/* A = 3 (cache[1:0]), B = 4 (cache[3:2]) → A*B = 12 = 0x0C           */
/* Result written as 4 bytes little-endian at TEST_VRAM_MULT.          */
/* Uses CRITIC to suppress the deferred VBI during the operation.      */
/* ------------------------------------------------------------------ */
static void test_multiplier(void)
{
    unsigned char r0, r1, r2, r3;

    printf("\n[9] Multiplier result to VRAM\n");

    CRITIC = 1;

    /* Pre-fill target area with 0xFF to make wrong results visible */
    set_addr0(TEST_VRAM_MULT, ADDR_H_INC1_B0);
    VERA_DATA0 = 0xFF;
    VERA_DATA0 = 0xFF;
    VERA_DATA0 = 0xFF;
    VERA_DATA0 = 0xFF;

    /* FX_CTRL: cache_write_enabled=1, all others 0 → 0x40 */
    VERA_CTRL  = DCSEL_2;
    VERA_REG09 = 0x40;

    /* FX_MULT: bit7=ResetAccum resets accumulator to 0; bit4=mult_enabled → 0x90 */
    VERA_REG0C = 0x90;

    /* Cache: A=3 (low word), B=4 (high word) */
    VERA_CTRL  = DCSEL_6;
    VERA_REG09 = 0x03;   /* cache[0] = A_lo */
    VERA_REG0A = 0x00;   /* cache[1] = A_hi */
    VERA_REG0B = 0x04;   /* cache[2] = B_lo */
    VERA_REG0C = 0x00;   /* cache[3] = B_hi */

    /* Write triggers vera_fx_write_data: 3*4=12 stored as 4-byte LE */
    set_addr0(TEST_VRAM_MULT, ADDR_H_NOINC_B0);
    VERA_DATA0 = 0x00;

    /* Disable cache-write mode before reading back */
    VERA_CTRL  = DCSEL_2;
    VERA_REG09 = 0x00;
    VERA_REG0C = 0x00;

    /* Read back the 4 bytes written by the multiplier */
    set_addr0(TEST_VRAM_MULT, ADDR_H_INC1_B0);
    r0 = VERA_DATA0;
    r1 = VERA_DATA0;
    r2 = VERA_DATA0;
    r3 = VERA_DATA0;

    VERA_CTRL    = DCSEL_0;
    CRITIC = 0;

    check_b("VRAM+0 = 0x0C (3*4)", r0, 0x0C);
    check_b("VRAM+1 = 0x00",       r1, 0x00);
    check_b("VRAM+2 = 0x00",       r2, 0x00);
    check_b("VRAM+3 = 0x00",       r3, 0x00);
}

/* ------------------------------------------------------------------ */
/* Test 10: Transparency — zero byte skipped, non-zero written (8-bit) */
/* ------------------------------------------------------------------ */
static void test_transparency(void)
{
    unsigned char v;

    printf("\n[10] Transparency (8-bit mode)\n");

    CRITIC = 1;

    /* Write sentinel 0x55 with transparency disabled */
    VERA_CTRL  = DCSEL_2;
    VERA_REG09 = 0x00;
    vram_write(TEST_VRAM_TRANS, 0x55);
    v = vram_read(TEST_VRAM_TRANS);
    check_b("sentinel 0x55 written", v, 0x55);

    /* Enable transparency: FX_CTRL bit7=1 */
    VERA_CTRL  = DCSEL_2;
    VERA_REG09 = 0x80;

    /* Write 0x00 — must be skipped */
    vram_write(TEST_VRAM_TRANS, 0x00);
    v = vram_read(TEST_VRAM_TRANS);
    check_b("zero skipped (still 0x55)", v, 0x55);

    /* Write 0xAA — non-zero, must pass through */
    vram_write(TEST_VRAM_TRANS, 0xAA);
    v = vram_read(TEST_VRAM_TRANS);
    check_b("non-zero 0xAA written", v, 0xAA);

    /* Restore */
    VERA_CTRL    = DCSEL_2;
    VERA_REG09   = 0x00;
    VERA_CTRL    = DCSEL_0;
    CRITIC = 0;
}

/* ------------------------------------------------------------------ */

int main(void)
{
    vera_require();

    /* Soft-reset the VERA chip: clears all FX state, sets fx_pixel_pos = 256
     * (POS_S = 0x80).  Safe here because VERA is not driving the display. */
    VERA_CTRL = 0x80;

    printf("VERA FX coprocessor test\n");
    printf("========================\n");

    printf("\n[1] Post-reset position (bug-9: POS_S must be 0x80)\n");
    VERA_CTRL = DCSEL_5;
    check_b("X_POS_S after reset", VERA_REG09, 0x80);
    check_b("Y_POS_S after reset", VERA_REG0A, 0x80);
    VERA_CTRL = DCSEL_0;

    test_fx_ctrl();
    test_tilebase_mapbase();
    test_fx_mult_reg();
    test_incr();
    test_pos_integer();
    test_pos_subpixel();
    test_cache_rw();
    test_multiplier();
    test_transparency();

    printf("\n========================\n");
    printf("PASS: %d   FAIL: %d\n",
           (unsigned int)g_pass, (unsigned int)g_fail);
    printf("Press any key...\n");
    cgetc();
    return (g_fail == 0) ? 0 : 1;
}
