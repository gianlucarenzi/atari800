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

/* --- VERA keyboard input (only when the VERA *driver* is installed) --- */
#define VCTL_SIG0          'V'
#define VCTL_SIG1          'C'
#define VCTL_SIG2          'T'
#define VCTL_SIG3          'L'

#define VCTL_FLAGS         4
#define VCTL_REQUEST       5
#define VCTL_PARAM0        6
#define VCTL_ENTRY_LO      10
#define VCTL_ENTRY_HI      11

#define VCTL_FLAG_API_READY 0x80
#define VERA_REQ_GETC       0x04

static volatile unsigned char* vctl           = 0;
static void                 (*vera_api_entry)(void) = 0;
static unsigned char          vera_pending   = 0xFF;

static volatile unsigned char* find_vctl_block(void)
{
    uint16_t base;
    uint16_t a;

    base = (uint16_t) ((uintptr_t) OS.memtop + 1u);

    /* Sanity: VERA driver lives in RAM below ROM ($C000). */
    if ((base < 0x2000u) || (base >= 0xC000u))
    {
        return 0;
    }

    /* Scan a small window above MEMTOP for the "VCTL" signature. */
    for (a = base; a < 0xC000u - 16u; ++a)
    {
        volatile unsigned char* p;

        p = (volatile unsigned char*) (uintptr_t) a;

        if ((p[0] == VCTL_SIG0) && (p[1] == VCTL_SIG1) && (p[2] == VCTL_SIG2) && (p[3] == VCTL_SIG3))
        {
            return p;
        }
    }

    return 0;
}

static void vera_api_init(void)
{
    uint16_t entry;

    vctl           = find_vctl_block();
    vera_api_entry = 0;
    vera_pending   = 0xFF;

    if (!vctl)
    {
        return;
    }

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

static unsigned char vera_getc_nb(void)
{
    if (!vctl || !vera_api_entry)
    {
        return 0xFF;
    }

    vctl[VCTL_REQUEST] = VERA_REQ_GETC;
    vera_api_entry();
    return vctl[VCTL_PARAM0];
}

static unsigned char kb_haschar(void)
{
    if (!vctl)
    {
        return kbhit();
    }

    if (vera_pending != 0xFF)
    {
        return 1;
    }

    vera_pending = vera_getc_nb();
    return (vera_pending != 0xFF);
}

static unsigned char kb_getchar(void)
{
    unsigned char c;

    if (!vctl)
    {
        return cgetc();
    }

    if (vera_pending == 0xFF)
    {
        vera_pending = vera_getc_nb();
    }

    c            = vera_pending;
    vera_pending = 0xFF;
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
    if (count < RING_SIZE)
    {
        ring_buf[head] = c;
        head = (head + 1) % RING_SIZE;
        count++;
    }
}

unsigned char ring_get(void)
{
    unsigned char c = 0;
    if (count > 0)
    {
        c = ring_buf[tail];
        tail = (tail + 1) % RING_SIZE;
        count--;
    }
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
    VT_ST_NORM = 0,
    VT_ST_ESC  = 1,
    VT_ST_CSI  = 2
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

    unsigned char    last_was_cr;
} vt;

static unsigned char term_cols(void)
{
    /* OS.rmargn is the last column index (0-based). */
    return (unsigned char) (OS.rmargn + 1u);
}

static unsigned char term_rows(void)
{
    /* With VERA 80x30 driver loaded, cursor Y runs 0..29. */
    return (unsigned char) (vctl ? 30u : 24u);
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

        case 'm': /* SGR */
            /* Attributes/colors not implemented (monochrome). */
            break;

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

    /* Init screen/terminal */
    cursor(1);
    clrscr();
    printf("CP/M Terminal\n");

    /* Silence noisy I/O (SIO beeps) during the terminal session. */
    old_soundr = OS.soundr;
    OS.soundr  = 0;

    /* If the VERA 80x30 driver is loaded, use its non-blocking GETC API. */
    vera_api_init();
    vt_reset();

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
