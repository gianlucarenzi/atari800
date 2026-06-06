/**
 * ============================================================================
 * RUNCPM.C - FujiNet RunCPM Terminal with Asynchronous Buffering
 * Supports Direct VERA 80x30 and Standard Atari 40-col fallback.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <unistd.h>
#include <atari.h>
#include <stdint.h>

#include "../logo.x16.h"


/* --- Critical section: NMI (VBI) + IRQ protection ---
 * CRITIC ($42) must be modified with a single INC/DEC opcode; a C expression
 * like OS.critic++ can emit LDA/ADC/STA which an NMI can tear.  SEI/CLI
 * additionally blocks maskable IRQs. */
#define ENTER_CRITICAL  do { asm("inc $42"); asm("sei"); } while(0)
#define EXIT_CRITICAL   do { asm("cli"); asm("dec $42"); } while(0)

/* --- SIO Constants --- */
#define DFUJI           0x71
#define DREAD           0x40
#define DWRITE          0x80
#define SUCCESS         1
#define E_EOF           136
#define TIMEOUT         0x1F

/* --- Ring Buffer Configuration --- */
#define RING_SIZE       2048
unsigned char  ring_buf[RING_SIZE];
unsigned short head  = 0;
unsigned short tail  = 0;
unsigned short count = 0;

/* --- SIO Buffers --- */
unsigned char sio_rx_tmp[256];
unsigned char tx_buf[64];
/* FujiNet expects exactly 256 bytes for Open command spec */
unsigned char devicespec[256];

/* --- State Variables --- */
unsigned char trip = 0;
void*         old_vprced;
unsigned char old_enabled;
unsigned char old_soundr;

/* --- VERA driver detection (used for cursor tracking and FLUSH_KBD) --- */
#define VCTL_SIG0          'V'
#define VCTL_SIG1          'C'
#define VCTL_SIG2          'T'
#define VCTL_SIG3          'L'

#define VCTL_FLAGS         4
#define VCTL_REQUEST       5
#define VCTL_PARAM0        6
#define VCTL_CURSOR_X      8
#define VCTL_CURSOR_Y      9
#define VCTL_ENTRY_LO      10
#define VCTL_ENTRY_HI      11

#define VCTL_PARAM1        7    /* current text color byte (bg<<4|fg), VERA palette */
#define VCTL_SCREEN_COLS   16   /* viewport width  in chars (set by loader) */
#define VCTL_SCREEN_ROWS   17   /* viewport height in chars (set by loader) */

#define VCTL_FLAG_API_READY 0x80
#define VERA_REQ_FLUSH_KBD  0x05

static volatile unsigned char* vctl           = 0;
static void                 (*vera_api_entry)(void) = 0;

static volatile unsigned char* find_vctl_block(void)
{
    uint16_t base;
    uint16_t a;

    base = (uint16_t) ((uintptr_t) OS.memtop + 1u);

    if ((base < 0x2000u) || (base >= 0xC000u))
        return 0;

    for (a = base; a < 0xC000u - 16u; ++a)
    {
        volatile unsigned char* p = (volatile unsigned char*) (uintptr_t) a;
        if ((p[0] == VCTL_SIG0) && (p[1] == VCTL_SIG1) &&
            (p[2] == VCTL_SIG2) && (p[3] == VCTL_SIG3))
            return p;
    }
    return 0;
}

static void vera_api_init(void)
{
    uint16_t entry;

    vctl           = find_vctl_block();
    vera_api_entry = 0;

    if (!vctl)
        return;

    if ((vctl[VCTL_FLAGS] & VCTL_FLAG_API_READY) == 0)
    {
        vctl = 0;
        return;
    }

    entry = (uint16_t) vctl[VCTL_ENTRY_LO] | ((uint16_t) vctl[VCTL_ENTRY_HI] << 8);

    if ((entry < 0x2000u) || (entry >= 0xC000u))
    {
        vctl = 0;
        return;
    }

    vera_api_entry = (void (*)(void)) (uintptr_t) entry;
}

/*
 * kbcode_table — KBCODE → ATASCII, same layout as vera_sys_es_hook.s.
 * Blocks: [0..63]=unshifted, [64..127]=SHIFT, [128..191]=CTRL, [192..255]=CTRL+SHIFT.
 * $80 = undefined key, $82/$83/$84 = CAPS LOCK toggle (handled separately).
 */
static const unsigned char kbcode_table[256] = {
    /* Unshifted */
    0x6C,0x6A,0x3B,0x80,0x80,0x6B,0x2B,0x2A,
    0x6F,0x80,0x70,0x75,0x9B,0x69,0x2D,0x3D,
    0x76,0x80,0x63,0x80,0x80,0x62,0x78,0x7A,
    0x34,0x80,0x33,0x36,0x1B,0x35,0x32,0x31,
    0x2C,0x20,0x2E,0x6E,0x80,0x6D,0x2F,0x81,
    0x72,0x80,0x65,0x79,0x7F,0x74,0x77,0x71,
    0x39,0x80,0x30,0x37,0x7E,0x38,0x3C,0x3E,
    0x66,0x68,0x64,0x80,0x82,0x67,0x73,0x61,
    /* SHIFT */
    0x4C,0x4A,0x3A,0x80,0x80,0x4B,0x5C,0x5E,
    0x4F,0x80,0x50,0x55,0x9B,0x49,0x5F,0x7C,
    0x56,0x80,0x43,0x80,0x80,0x42,0x58,0x5A,
    0x24,0x80,0x23,0x26,0x1B,0x25,0x22,0x21,
    0x5B,0x20,0x5D,0x4E,0x80,0x4D,0x3F,0x80,
    0x52,0x80,0x45,0x59,0x9F,0x54,0x57,0x51,
    0x28,0x80,0x29,0x27,0x9C,0x40,0x7D,0x9D,
    0x46,0x48,0x44,0x80,0x83,0x47,0x53,0x41,
    /* CTRL */
    0x0C,0x0A,0x7B,0x80,0x80,0x0B,0x1E,0x1F,
    0x0F,0x80,0x10,0x15,0x9B,0x09,0x1C,0x1D,
    0x16,0x80,0x03,0x80,0x80,0x02,0x18,0x1A,
    0x80,0x80,0x85,0x80,0x1B,0x80,0xFD,0x80,
    0x00,0x20,0x60,0x0E,0x80,0x0D,0x80,0x80,
    0x12,0x80,0x05,0x19,0x9E,0x14,0x17,0x11,
    0x80,0x80,0x80,0x80,0xFE,0x80,0x7D,0xFF,
    0x06,0x08,0x04,0x80,0x84,0x07,0x13,0x01,
    /* CTRL+SHIFT (same as CTRL) */
    0x0C,0x0A,0x7B,0x80,0x80,0x0B,0x1E,0x1F,
    0x0F,0x80,0x10,0x15,0x9B,0x09,0x1C,0x1D,
    0x16,0x80,0x03,0x80,0x80,0x02,0x18,0x1A,
    0x80,0x80,0x85,0x80,0x1B,0x80,0xFD,0x80,
    0x00,0x20,0x60,0x0E,0x80,0x0D,0x80,0x80,
    0x12,0x80,0x05,0x19,0x9E,0x14,0x17,0x11,
    0x80,0x80,0x80,0x80,0xFE,0x80,0x7D,0xFF,
    0x06,0x08,0x04,0x80,0x84,0x07,0x13,0x01
};

/*
 * Keyboard input.
 *
 * Three paths tried in order:
 *  1. OS.ch ($02FC)    — set by _vera_kbd_irq_handler / VBI @vbi_detect via stx CH
 *  2. VERA ring buffer — set by VBI @vbi_detect push (in case stx CH doesn't reach)
 *  3. Direct SKSTAT/KBCODE poll + kbcode_table translation (always works)
 */

static unsigned char kbd_pending = 0xFF;
static unsigned char kbd_caps    = 0xFF; /* $FF = CAPS active (matches VERA driver default) */

static unsigned char kbd_translate(unsigned char kb)
{
    unsigned char c = kbcode_table[kb];
    /* CAPS toggle */
    if (c == 0x82 || c == 0x83 || c == 0x84)
    {
        kbd_caps ^= 0xFF;
        return 0xFF;            /* no char to return */
    }
    if (c == 0x80 || c == 0x81)
        return 0xFF;            /* undefined */
    /* Apply CAPS LOCK: flip case for a-z / A-Z */
    if (kbd_caps == 0xFF)
    {
        unsigned char u = c & 0xDF;
        if (u >= 'A' && u <= 'Z')
            c ^= 0x20;
    }
    return c;
}

static unsigned char kbd_poll_kbcode(void)
{
    static unsigned char last_kb = 0xFF;
    unsigned char sk = *(volatile unsigned char*)0xD20F;
    unsigned char kb;

    if (sk & 0x04)          /* bit2=1 = no key held */
    {
        last_kb = 0xFF;
        return 0xFF;
    }

    kb = *(volatile unsigned char*)0xD209;
    if (kb == 0xFF || kb == last_kb)
        return 0xFF;

    last_kb = kb;
    return kbd_translate(kb);
}

static unsigned char kb_haschar(void)
{
    unsigned char c;

    if (kbd_pending != 0xFF)
        return 1;

    /* Direct SKSTAT/KBCODE poll — single path, no double-echo risk.
     * OS.ch and VERA ring are skipped: @vbi_detect fills both in the same
     * VBI tick, causing the same keypress to be detected twice. */
    c = kbd_poll_kbcode();
    if (c != 0xFF)
    {
        kbd_pending = c;
        return 1;
    }

    return 0;
}

static unsigned char kb_getchar(void)
{
    unsigned char c = kbd_pending;
    kbd_pending = 0xFF;
    return c;
}

/* --- External Assembly Wrappers --- */
extern void __fastcall__ siov(void);
extern void ih(void);


/* 
 * ============================================================================
 * FUJINET N: PROTOCOL WRAPPERS
 * ============================================================================
 */

unsigned char nopen(void)
{
    /* Clear and prepare devicespec buffer */
    memset(devicespec, 0, 256);
    strcpy((char*)devicespec, "N1:CPM:///");
    /* FujiNet expects ATASCII EOL ($9B) as terminator */
    devicespec[strlen((char*)devicespec)] = 0x9B;

    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'O';
    OS.dcb.dstats = DWRITE;
    OS.dcb.dbuf   = devicespec;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 256;      /* Fixed 256 byte payload */
    OS.dcb.daux1  = 0x0C;
    OS.dcb.daux2  = 3;        /* Translation CRLF */
    siov();
    return OS.dcb.dstats;
}

unsigned char nstatus(void)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'S';
    OS.dcb.dstats = DREAD;
    OS.dcb.dbuf   = OS.dvstat;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 4;
    OS.dcb.daux1  = 0;
    OS.dcb.daux2  = 0;
    siov();
    return OS.dvstat[3];
}

unsigned char nread(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'R';
    OS.dcb.dstats = DREAD;
    OS.dcb.dbuf   = buf;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = len;
    OS.dcb.daux1  = len & 0xFF;
    OS.dcb.daux2  = (len >> 8) & 0xFF;
    siov();
    return OS.dcb.dstats;
}

unsigned char nwrite(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'W';
    OS.dcb.dstats = DWRITE;
    OS.dcb.dbuf   = buf;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = len;
    OS.dcb.daux1  = len & 0xFF;
    OS.dcb.daux2  = (len >> 8) & 0xFF;
    siov();
    return OS.dcb.dstats;
}

/* 
 * ============================================================================
 * RING BUFFER LOGIC
 * ============================================================================
 */

void ring_put(unsigned char c)
{
    ENTER_CRITICAL;
    if (count < RING_SIZE)
    {
        ring_buf[head] = c;
        head = (head + 1) % RING_SIZE;
        count++;
    }
    EXIT_CRITICAL;
}

unsigned char ring_get(void)
{
    unsigned char c = 0;
    ENTER_CRITICAL;
    if (count > 0)
    {
        c = ring_buf[tail];
        tail = (tail + 1) % RING_SIZE;
        count--;
    }
    EXIT_CRITICAL;
    return c;
}

/* 
 * ============================================================================
 * UTILITIES
 * ============================================================================
 */

unsigned char atascii_to_ascii(unsigned char c)
{
    if (c == 155) return 13;   /* ATASCII EOL  → CR  */
    if (c == 126) return 8;    /* ATASCII BS   → BS  */
    if (c == 127) return 9;    /* ATASCII TAB  → TAB */
    return c;
}

/* --- Terminal output: VT100/ANSI parser -> ATASCII primitives --- */
#define ATASCII_ESC          0x1B
#define ATASCII_CURSOR_UP    0x1C
#define ATASCII_CURSOR_DOWN  0x1D
#define ATASCII_CURSOR_LEFT  0x1E
#define ATASCII_CURSOR_RIGHT 0x1F
#define ATASCII_CLEAR        0x7D
#define ATASCII_BACKSPACE    0x7E
#define ATASCII_TAB          0x7F
#define ATASCII_EOL          0x9B
#define ATASCII_DELETE_LINE  0x9C
#define ATASCII_INSERT_LINE  0x9D
#define ATASCII_BELL         0xFD
#define ATASCII_DELETE_CHAR  0xFE
#define ATASCII_INSERT_CHAR  0xFF

#define VCTL_CURSOR_X        8
#define VCTL_CURSOR_Y        9

#define VT_MAX_PARAMS        8

typedef enum
{
    VT_ST_NORM    = 0,
    VT_ST_ESC     = 1,
    VT_ST_CSI     = 2,
    VT_ST_VT52Y_R = 3,   /* ESC Y — waiting for row byte  */
    VT_ST_VT52Y_C = 4    /* ESC Y row — waiting for col byte */
} vt_parse_state_t;

static struct
{
    vt_parse_state_t st;
    unsigned char    params[VT_MAX_PARAMS];
    unsigned char    pcount;
    unsigned char    curparam;
    unsigned char    have_param;

    unsigned char    cur_x;
    unsigned char    cur_y;
    unsigned char    saved_x;
    unsigned char    saved_y;

    unsigned char    scroll_top;    /* 0-based */
    unsigned char    scroll_bottom; /* 0-based */
    unsigned char    vt52_row;      /* pending row for ESC Y r c */

    unsigned char    fg;            /* current VERA fg color (0-15) */
    unsigned char    bg;            /* current VERA bg color (0-15) */
    unsigned char    bold;          /* SGR bold flag → bright colors */

    unsigned char    last_was_cr;
} vt;

/* ANSI color index (0-7) → VERA color index (0-15) */
static const unsigned char ansi_to_vera[8] =
{
    0,   /* 0 black   → VERA 0  black      */
    2,   /* 1 red     → VERA 2  red        */
    5,   /* 2 green   → VERA 5  green      */
    7,   /* 3 yellow  → VERA 7  yellow     */
    6,   /* 4 blue    → VERA 6  blue       */
    4,   /* 5 magenta → VERA 4  purple     */
    3,   /* 6 cyan    → VERA 3  cyan       */
    1    /* 7 white   → VERA 1  white      */
};

/* Bright variants (+8 in VERA palette) */
static const unsigned char ansi_to_vera_bright[8] =
{
    11,  /* 0 bright black   → VERA 11 dark grey   */
    10,  /* 1 bright red     → VERA 10 light red   */
    13,  /* 2 bright green   → VERA 13 light green */
    15,  /* 3 bright yellow  → VERA 15 light grey  */
    14,  /* 4 bright blue    → VERA 14 light blue  */
    4,   /* 5 bright magenta → VERA 4  purple      */
    3,   /* 6 bright cyan    → VERA 3  cyan        */
    1    /* 7 bright white   → VERA 1  white       */
};

static void vt_apply_color(void)
{
    unsigned char fg = vt.bold ? ansi_to_vera_bright[vt.fg & 7]
                               : ansi_to_vera[vt.fg & 7];
    unsigned char color = (unsigned char)((vt.bg << 4) | fg);
    if (vctl)
        vctl[VCTL_PARAM1] = color;
}

static unsigned char term_cols(void)
{
    /* Read from VCTL when VERA is active (set by loader to SCREEN_COLS_VIEW).
     * Fall back to OS.rmargn+1 for non-VERA or old drivers without the field. */
    if (vctl && vctl[VCTL_SCREEN_COLS])
        return vctl[VCTL_SCREEN_COLS];
    return (unsigned char) (OS.rmargn + 1u);
}

static unsigned char term_rows(void)
{
    /* Read from VCTL when VERA is active (set by loader to SCREEN_ROWS_VIEW).
     * This handles 80x30 (30), 80x60 (60), 40x30 (30) automatically.
     * Fall back to 24 for non-VERA or old drivers without the field. */
    if (vctl && vctl[VCTL_SCREEN_ROWS])
        return vctl[VCTL_SCREEN_ROWS];
    return 24u;   /* non-VERA: standard 40x24 Atari */
}

static void term_sync_cursor(void)
{
    if (vctl)
    {
        vt.cur_x = vctl[VCTL_CURSOR_X];
        vt.cur_y = vctl[VCTL_CURSOR_Y];
    }
    else
    {
        /* Without VERA: read position from OS display handler (ROWCRS/COLCRS). */
        vt.cur_x = (unsigned char) OS.colcrs;
        vt.cur_y = OS.rowcrs;
    }
}

static void term_out_atascii(unsigned char c)
{
    /* putchar expects an int containing an unsigned char value. */
    putchar((unsigned char) c);
}

static void term_cursor_left(unsigned char n)
{
    unsigned char i;

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_CURSOR_LEFT);
    }

    if (vt.cur_x >= n)
    {
        vt.cur_x -= n;
    }
    else
    {
        vt.cur_x = 0;
    }
}

static void term_cursor_right(unsigned char n)
{
    unsigned char i;

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_CURSOR_RIGHT);
    }

    vt.cur_x += n;
}

static void term_cursor_up(unsigned char n)
{
    unsigned char i;

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_CURSOR_UP);
    }

    if (vt.cur_y >= n)
    {
        vt.cur_y -= n;
    }
    else
    {
        vt.cur_y = 0;
    }
}

static void term_cursor_down(unsigned char n)
{
    unsigned char i;

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_CURSOR_DOWN);
    }

    vt.cur_y += n;
}

static void term_move_abs(unsigned char row1, unsigned char col1)
{
    unsigned char cols;
    unsigned char rows;
    unsigned char target_x;
    unsigned char target_y;

    cols = term_cols();
    rows = term_rows();

    if (row1 < 1)
    {
        row1 = 1;
    }
    if (col1 < 1)
    {
        col1 = 1;
    }
    if (row1 > rows)
    {
        row1 = rows;
    }
    if (col1 > cols)
    {
        col1 = cols;
    }

    target_y = (unsigned char) (row1 - 1u);
    target_x = (unsigned char) (col1 - 1u);

    term_sync_cursor();

    if (vt.cur_y > target_y)
    {
        term_cursor_up((unsigned char) (vt.cur_y - target_y));
    }
    else if (vt.cur_y < target_y)
    {
        term_cursor_down((unsigned char) (target_y - vt.cur_y));
    }

    if (vt.cur_x > target_x)
    {
        term_cursor_left((unsigned char) (vt.cur_x - target_x));
    }
    else if (vt.cur_x < target_x)
    {
        term_cursor_right((unsigned char) (target_x - vt.cur_x));
    }
}

static void term_save_cursor(void)
{
    term_sync_cursor();
    vt.saved_x = vt.cur_x;
    vt.saved_y = vt.cur_y;
}

static void term_restore_cursor(void)
{
    term_move_abs((unsigned char) (vt.saved_y + 1u), (unsigned char) (vt.saved_x + 1u));
}

static void term_erase_in_line(unsigned char mode)
{
    unsigned char cols;
    unsigned char x;
    unsigned char i;

    cols = term_cols();

    term_sync_cursor();
    x = vt.cur_x;

    if (mode == 2)
    {
        term_save_cursor();
        term_move_abs((unsigned char) (vt.cur_y + 1u), 1);

        for (i = 0; i < cols; ++i)
        {
            term_out_atascii(' ');
        }

        term_restore_cursor();
        return;
    }

    if (mode == 1)
    {
        term_save_cursor();
        term_move_abs((unsigned char) (vt.cur_y + 1u), 1);

        for (i = 0; (i <= x) && (i < cols); ++i)
        {
            term_out_atascii(' ');
        }

        term_restore_cursor();
        return;
    }

    /* mode 0 (default): cursor to end of line */
    term_save_cursor();

    for (i = x; i < cols; ++i)
    {
        term_out_atascii(' ');
    }

    term_restore_cursor();
}

static void term_erase_in_display(unsigned char mode)
{
    unsigned char rows;
    unsigned char y;

    rows = term_rows();

    if (mode == 2)
    {
        term_out_atascii(ATASCII_CLEAR);
        vt.cur_x = 0;
        vt.cur_y = 0;
        return;
    }

    term_save_cursor();
    term_sync_cursor();

    if (mode == 1)
    {
        /* from start to cursor */
        for (y = 0; y < vt.cur_y; ++y)
        {
            term_move_abs((unsigned char) (y + 1u), 1);
            term_erase_in_line(2);
        }

        term_move_abs((unsigned char) (vt.cur_y + 1u), 1);
        term_erase_in_line(1);
    }
    else
    {
        /* mode 0 default: cursor to end */
        term_erase_in_line(0);

        for (y = (unsigned char) (vt.cur_y + 1u); y < rows; ++y)
        {
            term_move_abs((unsigned char) (y + 1u), 1);
            term_erase_in_line(2);
        }
    }

    term_restore_cursor();
}

static void term_insert_lines(unsigned char n)
{
    unsigned char i;

    if (n == 0)
    {
        n = 1;
    }

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_INSERT_LINE);
    }
}

static void term_delete_lines(unsigned char n)
{
    unsigned char i;

    if (n == 0)
    {
        n = 1;
    }

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_DELETE_LINE);
    }
}

static void term_insert_chars(unsigned char n)
{
    unsigned char i;

    if (n == 0)
    {
        n = 1;
    }

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_INSERT_CHAR);
    }
}

static void term_delete_chars(unsigned char n)
{
    unsigned char i;

    if (n == 0)
    {
        n = 1;
    }

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(ATASCII_DELETE_CHAR);
    }
}

static void term_erase_chars(unsigned char n)
{
    unsigned char i;

    if (n == 0)
    {
        n = 1;
    }

    term_save_cursor();

    for (i = 0; i < n; ++i)
    {
        term_out_atascii(' ');
    }

    term_restore_cursor();
}

static void term_scroll_up(unsigned char n)
{
    unsigned char rows;
    unsigned char i;

    rows = term_rows();

    if (n == 0)
    {
        n = 1;
    }

    /* Only safe for full-screen region with current primitives. */
    if ((vt.scroll_top != 0) || (vt.scroll_bottom != (unsigned char) (rows - 1u)))
    {
        return;
    }

    term_save_cursor();

    for (i = 0; i < n; ++i)
    {
        term_move_abs(1, 1);
        term_delete_lines(1);
    }

    term_restore_cursor();
}

static void term_scroll_down(unsigned char n)
{
    unsigned char rows;
    unsigned char i;

    rows = term_rows();

    if (n == 0)
    {
        n = 1;
    }

    /* Only safe for full-screen region with current primitives. */
    if ((vt.scroll_top != 0) || (vt.scroll_bottom != (unsigned char) (rows - 1u)))
    {
        return;
    }

    term_save_cursor();

    for (i = 0; i < n; ++i)
    {
        term_move_abs(1, 1);
        term_insert_lines(1);
    }

    term_restore_cursor();
}

static void vt_reset(void)
{
    unsigned char rows;

    rows = term_rows();

    memset(&vt, 0, sizeof(vt));
    vt.st            = VT_ST_NORM;
    vt.scroll_top    = 0;
    vt.scroll_bottom = (unsigned char) (rows - 1u);
    vt.last_was_cr   = 0;
    vt.fg            = 7;   /* white */
    vt.bg            = 6;   /* blue  */
    vt.bold          = 0;
    vt_apply_color();
}

static unsigned char csi_param(unsigned char idx, unsigned char defval)
{
    if (idx >= vt.pcount)
    {
        return defval;
    }

    if (!vt.have_param && (idx == 0))
    {
        return defval;
    }

    return vt.params[idx];
}

static void vt_dispatch_csi(unsigned char final)
{
    unsigned char n1;
    unsigned char n2;

    n1 = csi_param(0, 0);
    n2 = csi_param(1, 0);

    switch (final)
    {
        case 'A': /* CUU */
            if (n1 == 0)
            {
                n1 = 1;
            }
            term_sync_cursor();
            term_cursor_up(n1);
            break;

        case 'B': /* CUD */
            if (n1 == 0)
            {
                n1 = 1;
            }
            term_sync_cursor();
            term_cursor_down(n1);
            break;

        case 'C': /* CUF */
            if (n1 == 0)
            {
                n1 = 1;
            }
            term_sync_cursor();
            term_cursor_right(n1);
            break;

        case 'D': /* CUB */
            if (n1 == 0)
            {
                n1 = 1;
            }
            term_sync_cursor();
            term_cursor_left(n1);
            break;

        case 'H': /* CUP */
        case 'f':
            if (n1 == 0)
            {
                n1 = 1;
            }
            if (n2 == 0)
            {
                n2 = 1;
            }
            term_move_abs(n1, n2);
            break;

        case 'J': /* ED */
            term_erase_in_display(n1);
            break;

        case 'K': /* EL */
            term_erase_in_line(n1);
            break;

        case 'm': /* SGR — Select Graphic Rendition */
        {
            unsigned char pi;
            unsigned char changed = 0;

            if (vt.pcount == 0)
            {
                /* ESC[m = reset */
                vt.fg   = 7;
                vt.bg   = 6;
                vt.bold = 0;
                changed = 1;
            }

            for (pi = 0; pi < vt.pcount; ++pi)
            {
                unsigned char p = vt.params[pi];

                if (p == 0)
                {
                    vt.fg = 7; vt.bg = 6; vt.bold = 0;
                    changed = 1;
                }
                else if (p == 1)
                {
                    vt.bold = 1;
                    changed = 1;
                }
                else if (p == 22)
                {
                    vt.bold = 0;
                    changed = 1;
                }
                else if (p >= 30 && p <= 37)
                {
                    vt.fg = (unsigned char)(p - 30);
                    changed = 1;
                }
                else if (p == 39)
                {
                    vt.fg = 7;
                    changed = 1;
                }
                else if (p >= 40 && p <= 47)
                {
                    vt.bg = ansi_to_vera[p - 40];
                    changed = 1;
                }
                else if (p == 49)
                {
                    vt.bg = 6;
                    changed = 1;
                }
                else if (p >= 90 && p <= 97)  /* bright fg */
                {
                    vt.fg = (unsigned char)(p - 90);
                    vt.bold = 1;
                    changed = 1;
                }
                else if (p >= 100 && p <= 107) /* bright bg */
                {
                    vt.bg = ansi_to_vera_bright[p - 100];
                    changed = 1;
                }
            }

            if (changed)
                vt_apply_color();
            break;
        }

        case 's': /* save cursor */
            term_save_cursor();
            break;

        case 'u': /* restore cursor */
            term_restore_cursor();
            break;

        case 'r': /* DECSTBM (scroll region) */
        {
            unsigned char rows;
            unsigned char top;
            unsigned char bot;

            rows = term_rows();
            top  = n1 ? (unsigned char) (n1 - 1u) : 0;
            bot  = n2 ? (unsigned char) (n2 - 1u) : (unsigned char) (rows - 1u);

            if ((top < rows) && (bot < rows) && (top < bot))
            {
                vt.scroll_top    = top;
                vt.scroll_bottom = bot;
            }
            else
            {
                vt.scroll_top    = 0;
                vt.scroll_bottom = (unsigned char) (rows - 1u);
            }

            /* VT100 homes cursor on region set. */
            term_move_abs(1, 1);
            break;
        }

        case 'L': /* IL */
            term_insert_lines(n1);
            break;

        case 'M': /* DL */
            term_delete_lines(n1);
            break;

        case '@': /* ICH */
            term_insert_chars(n1);
            break;

        case 'P': /* DCH */
            term_delete_chars(n1);
            break;

        case 'X': /* ECH */
            term_erase_chars(n1);
            break;

        case 'S': /* SU */
            term_scroll_up(n1);
            break;

        case 'T': /* SD */
            term_scroll_down(n1);
            break;

        default:
            break;
    }
}

static void vt_feed(unsigned char c)
{
    if (vt.st == VT_ST_NORM)
    {
        if (c == 0x1B)
        {
            vt.st = VT_ST_ESC;
            return;
        }

        /* Common ASCII control translations -> ATASCII */
        if (c == '\r')
        {
            term_out_atascii(ATASCII_EOL);
            vt.last_was_cr = 1;
            return;
        }

        if (c == '\n')
        {
            if (vt.last_was_cr)
            {
                vt.last_was_cr = 0;
                return;
            }

            term_out_atascii(ATASCII_EOL);
            return;
        }

        vt.last_was_cr = 0;

        if (c == '\b')
        {
            term_out_atascii(ATASCII_BACKSPACE);
            return;
        }

        if (c == '\t')
        {
            term_out_atascii(ATASCII_TAB);
            return;
        }

        if (c == 0x07)
        {
            term_out_atascii(ATASCII_BELL);
            return;
        }

        /* Default: printable byte (7-bit). */
        term_out_atascii((unsigned char) (c & 0x7F));
        return;
    }

    if (vt.st == VT_ST_VT52Y_R)
    {
        vt.vt52_row = (unsigned char)(c - 32);
        vt.st       = VT_ST_VT52Y_C;
        return;
    }

    if (vt.st == VT_ST_VT52Y_C)
    {
        unsigned char col = (unsigned char)(c - 32);
        term_move_abs((unsigned char)(vt.vt52_row + 1u),
                      (unsigned char)(col + 1u));
        vt.st = VT_ST_NORM;
        return;
    }

    if (vt.st == VT_ST_ESC)
    {
        if (c == '[')
        {
            vt.st         = VT_ST_CSI;
            vt.pcount     = 0;
            vt.curparam   = 0;
            vt.have_param = 0;
            memset(vt.params, 0, sizeof(vt.params));
            return;
        }

        /* VT52 single-char sequences */
        if (c == 'A')
        {
            term_sync_cursor();
            term_cursor_up(1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'B')
        {
            term_sync_cursor();
            term_cursor_down(1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'C')
        {
            term_sync_cursor();
            term_cursor_right(1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'D')
        {
            term_sync_cursor();
            term_cursor_left(1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'H') /* cursor home */
        {
            term_move_abs(1, 1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'E') /* erase screen + home */
        {
            term_out_atascii(ATASCII_CLEAR);
            vt.cur_x = 0;
            vt.cur_y = 0;
            vt.st    = VT_ST_NORM;
            return;
        }

        if (c == 'J') /* erase to end of screen */
        {
            term_erase_in_display(0);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'K') /* erase to end of line */
        {
            term_erase_in_line(0);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'I') /* reverse line feed */
        {
            term_sync_cursor();
            term_cursor_up(1);
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'Y') /* direct cursor address: ESC Y row+32 col+32 */
        {
            vt.st = VT_ST_VT52Y_R;
            return;
        }

        if (c == '7') /* DECSC */
        {
            term_save_cursor();
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == '8') /* DECRC */
        {
            term_restore_cursor();
            vt.st = VT_ST_NORM;
            return;
        }

        if (c == 'c') /* RIS */
        {
            vt_reset();
            term_out_atascii(ATASCII_CLEAR);
            vt.st = VT_ST_NORM;
            return;
        }

        /* Unhandled ESC sequences: ignore. */
        vt.st = VT_ST_NORM;
        return;
    }

    /* CSI state */
    if ((c >= '0') && (c <= '9'))
    {
        vt.have_param = 1;
        vt.curparam   = (unsigned char) (vt.curparam * 10u + (unsigned char) (c - '0'));
        return;
    }

    if (c == ';')
    {
        if (vt.pcount < (unsigned char) sizeof(vt.params))
        {
            vt.params[vt.pcount++] = vt.curparam;
        }

        vt.curparam   = 0;
        vt.have_param = 1;
        return;
    }

    /* Private mode prefixes like ? or > — ignore but keep parsing. */
    if ((c == '?') || (c == '>') || (c == '='))
    {
        return;
    }

    /* Final byte: commit last param (if any), dispatch, return to normal. */
    if (vt.pcount < (unsigned char) sizeof(vt.params))
    {
        vt.params[vt.pcount++] = vt.curparam;
    }

    vt_dispatch_csi(c);
    vt.st = VT_ST_NORM;
}

void terminal_putc(unsigned char c)
{
    vt_feed(c);
}


/* Send one ATASCII keystroke to FujiNet, expanding cursor keys to VT100. */
static void kb_send(void)
{
    unsigned char c = kb_getchar();

    switch (c)
    {
        case ATASCII_CURSOR_UP:
            tx_buf[0] = 0x1B; tx_buf[1] = '['; tx_buf[2] = 'A';
            nwrite(tx_buf, 3);
            break;
        case ATASCII_CURSOR_DOWN:
            tx_buf[0] = 0x1B; tx_buf[1] = '['; tx_buf[2] = 'B';
            nwrite(tx_buf, 3);
            break;
        case ATASCII_CURSOR_LEFT:
            tx_buf[0] = 0x1B; tx_buf[1] = '['; tx_buf[2] = 'D';
            nwrite(tx_buf, 3);
            break;
        case ATASCII_CURSOR_RIGHT:
            tx_buf[0] = 0x1B; tx_buf[1] = '['; tx_buf[2] = 'C';
            nwrite(tx_buf, 3);
            break;
        default:
            tx_buf[0] = atascii_to_ascii(c);
            nwrite(tx_buf, 1);
            break;
    }
}

/*
 * ============================================================================
 * MAIN TERMINAL LOOP
 * ============================================================================
 */

int main(void)
{
    unsigned char status;
    unsigned short bw, i, chunk;
    int running = 1;

    /* Silence noisy I/O (SIO beeps) during the terminal session. */
    old_soundr = OS.soundr;
    OS.soundr  = 0;

    /* Detect VERA driver (for cursor tracking and kbd ring flush). */
    vera_api_init();
    vt_reset();

    /* Disable ATARI ANTIC display — VERA/VGA is our output; prevents ANTIC
     * screen RAM from being dirtied by the OS cursor/E: handler.
     * ONLY do this if VERA is detected, otherwise we need ANTIC for fallback. */
    if (vctl)
    {
        *(volatile unsigned char*)0x022F = 0x00;   /* SDMCTL = 0 */
    }
    else
    {
        /* White Characters on Blue Background */
        *(volatile unsigned char*)0x02C5 = 0xFF;
        *(volatile unsigned char*)0x02C6 = 0x84;
    }

    /* Helper: set VERA text color directly (fg/bg are VERA palette indices). */
#define SET_COLOR(fg, bg) do { if (vctl) vctl[VCTL_PARAM1] = (unsigned char)(((bg)<<4)|(fg)); } while(0)

    /* Helper: print a string through vt_feed (handles CRLF, no color escape). */
#define P(s) { const char *_p = s; while(*_p) terminal_putc((unsigned char)*_p++); }

    /* Clear VERA screen, home cursor */
    P("\x1B[2J\x1B[H");

    /* Logo from vera_logo_editor (logo.x16.h).
     * draw_logo writes cursor+color directly to the VCTL block and calls
     * putchar() for each glyph — no VT100 parsing, full 256-char VERA set. */
    if (vctl)
    {
        draw_logo(vctl);
    }
    else
    {
        P("  ****  ATARI COMPUTERS ****\n");
    }
    /* atari@VERA-X16 header */
    SET_COLOR(1, 6);   P("  atari");
    SET_COLOR(14, 6);  P("@");
    if (vctl)
    {
        SET_COLOR(3, 6);   P("VERA-X16\r\n");
    }
    else
    {
        P("ANTIC VIDEO MODE\n");
    }

    SET_COLOR(11, 6);  P("  --------------------------\r\n");

    /* Color swatches — 8 normal + 8 bright backgrounds */
    if (vctl)
    {
        unsigned char ci;
        SET_COLOR(14, 6);  P("  Colors:   ");
        for (ci = 0; ci < 8; ++ci)
        {
            SET_COLOR(1, ci);
            P(" ");
        }
        P(" ");
        for (ci = 8; ci < 16; ++ci)
        {
            SET_COLOR(0, ci);
            P(" ");
        }
    }
    SET_COLOR(1, 6);   P("\r\n\r\n");

    SET_COLOR(13, 6);  P("  Connecting to CP/M...\r\n\r\n");
    SET_COLOR(1, 6);   /* restore default */

#undef SET_COLOR
#undef P

    /* Clear OS.ch and flush the VERA kbd ring + repeat state.
     * _vera_kbd_irq_handler updates CH; kbhit()/cgetc() handle the rest. */
    OS.ch = 0xFF;
    if (vctl && vera_api_entry)
    {
        vctl[VCTL_REQUEST] = VERA_REQ_FLUSH_KBD;
        vera_api_entry();
    }

    /* Initialize FujiNet session */
    if (nopen() != SUCCESS)
    {
        OS.soundr = old_soundr;
        printf("Open Error!\n");
        while(!kbhit());
        return 1;
    }

    /* --- Interrupt Setup --- */
    old_vprced = OS.vprced;
    old_enabled = PIA.pactl & 1;
    
    PIA.pactl &= (~1);
    OS.vprced = ih;
    PIA.pactl |= 1;

    /* Re-enable keyboard IRQ after SIO: nopen() leaves IRQEN with keyboard
     * bit cleared. Our VKEYBD handler re-arms it on each keypress, but we
     * need at least one IRQ to fire first. Writing $C0 here primes the pump. */
    OS.ch = 0xFF;
    *(volatile unsigned char*)0xD20E = 0xC0;   /* IRQEN: keyboard + break key */

    printf("Connected.\n\n");

    while (running)
    {
        /* 1. KEYBOARD -> FUJINET */
        if (kb_haschar())
        {
            kb_send();
        }

        /* 2. PRODUCER: SIO -> RING BUFFER */
        if (trip)
        {
            trip = 0;
            status = nstatus();
            
            if (status == E_EOF)
            {
                printf("\nDisconnected.\n");
                running = 0;
            }
            else
            {
                bw = (OS.dvstat[1] << 8) | OS.dvstat[0];
                if (bw > 0)
                {
                    if (bw > 256) bw = 256;
                    if (nread(sio_rx_tmp, bw) == SUCCESS)
                    {
                        for (i = 0; i < bw; ++i) ring_put(sio_rx_tmp[i]);
                    }
                }
            }
            PIA.pactl |= 1; 
        }

        /* 3. CONSUMER: RING BUFFER -> SCREEN */
        if (count > 0)
        {
            chunk = (count > 128) ? 128 : count;
            for (i = 0; i < chunk; ++i)
            {
                terminal_putc(ring_get());
            }
        }
    }

    /* --- Cleanup --- */
    PIA.pactl &= (~1);
    OS.vprced = old_vprced;
    PIA.pactl |= old_enabled;
    OS.soundr  = old_soundr;

    return 0;
}
