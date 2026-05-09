#include <atari.h>

#define MEMLO_ADDR (*(unsigned*)0x02E7)
#define AUDF1_ADDR (*(unsigned char*)0xD200)
#define AUDC1_ADDR (*(unsigned char*)0xD201)

#define VERA_ADDR_L (*(volatile unsigned char*)0xD100)
#define VERA_ADDR_M (*(volatile unsigned char*)0xD101)
#define VERA_ADDR_H (*(volatile unsigned char*)0xD102)
#define VERA_DATA0  (*(volatile unsigned char*)0xD103)

#define FREQ   0x08
#define VOLUME 0xAF
#define RATE   10

#define VERA_CTL_FLAG_METRONOME 0x01
#define VERA_CTL_FLAG_API_READY 0x80
#define VERA_CTL_SIZE           12u

#define VERA_REQ_NONE  0x00
#define VERA_REQ_CLEAR 0x01
#define VERA_REQ_DEMO  0x02
#define VERA_REQ_PUTC  0x03

#define VERA_SCREEN_COLS 80
#define VERA_SCREEN_ROWS 25
#define VERA_TEXT_COLOR  0x61
#define VERA_SCREEN_BASE_M 0xB0
#define VERA_SCREEN_BANK   0x11

extern void InitVbi(void);
extern void CallVeraApiService(void);
extern char vera_vbi_end;

typedef struct VeraCtl {
    unsigned char sig0;
    unsigned char sig1;
    unsigned char sig2;
    unsigned char sig3;
    unsigned char flags;
    unsigned char request;
    unsigned char param0;
    unsigned char param1;
    unsigned char cursor_x;
    unsigned char cursor_y;
    unsigned char entry_lo;
    unsigned char entry_hi;
} VeraCtl;

#pragma bss-name(push, "VERACTL")
volatile VeraCtl vera_ctl_block;
#pragma bss-name(pop)

unsigned char frames_until_click = RATE;
unsigned char click_active;
unsigned char resident_end_marker;

static volatile VeraCtl* vera_ctl(void)
{
    return &vera_ctl_block;
}

static void vera_set_addr(unsigned char column_addr, unsigned char row)
{
    VERA_ADDR_L = column_addr;
    VERA_ADDR_M = (unsigned char)(VERA_SCREEN_BASE_M + row);
    VERA_ADDR_H = VERA_SCREEN_BANK;
}

static void vera_set_cursor(unsigned char column, unsigned char row)
{
    volatile VeraCtl* ctl = vera_ctl();

    if (column >= VERA_SCREEN_COLS) {
        column = VERA_SCREEN_COLS - 1;
    }
    if (row >= VERA_SCREEN_ROWS) {
        row = VERA_SCREEN_ROWS - 1;
    }

    ctl->cursor_x = column;
    ctl->cursor_y = row;
}

static void vera_set_cell(unsigned char column, unsigned char row, unsigned char ch)
{
    vera_set_addr((unsigned char)(column << 1), row);
    VERA_DATA0 = ch;
    VERA_DATA0 = VERA_TEXT_COLOR;
}

static void vera_clear_line(unsigned char row)
{
    unsigned char column;

    vera_set_addr(0x00, row);
    for (column = 0; column < VERA_SCREEN_COLS; ++column) {
        VERA_DATA0 = ' ';
        VERA_DATA0 = VERA_TEXT_COLOR;
    }
}

static void vera_copy_line(unsigned char src_row, unsigned char dst_row)
{
    unsigned char column;
    unsigned char ch;
    unsigned char color;

    for (column = 0; column < VERA_SCREEN_COLS; ++column) {
        vera_set_addr((unsigned char)(column << 1), src_row);
        ch = VERA_DATA0;
        color = VERA_DATA0;

        vera_set_addr((unsigned char)(column << 1), dst_row);
        VERA_DATA0 = ch;
        VERA_DATA0 = color;
    }
}

static void vera_scroll_up(void)
{
    unsigned char row;

    for (row = 1; row < VERA_SCREEN_ROWS; ++row) {
        vera_copy_line(row, (unsigned char)(row - 1));
    }
    vera_clear_line(VERA_SCREEN_ROWS - 1);
}

static void vera_newline(void)
{
    volatile VeraCtl* ctl = vera_ctl();

    ctl->cursor_x = 0;
    if (ctl->cursor_y >= (VERA_SCREEN_ROWS - 1)) {
        vera_scroll_up();
    }
    else {
        ++ctl->cursor_y;
    }
}

static void vera_put_char(unsigned char ch)
{
    volatile VeraCtl* ctl = vera_ctl();

    if (ch == 0x9B || ch == '\n' || ch == '\r') {
        vera_newline();
        return;
    }

    if (ch == 0x08 || ch == 0x7E) {
        if (ctl->cursor_x != 0) {
            --ctl->cursor_x;
            vera_set_cell(ctl->cursor_x, ctl->cursor_y, ' ');
        }
        return;
    }

    if (ch < 0x20) {
        return;
    }

    vera_set_cell(ctl->cursor_x, ctl->cursor_y, ch);
    ++ctl->cursor_x;
    if (ctl->cursor_x >= VERA_SCREEN_COLS) {
        vera_newline();
    }
}

static void vera_write_text(const char* text)
{
    while (*text != '\0') {
        vera_put_char((unsigned char)*text);
        ++text;
    }
}

static void vera_clear_text(void)
{
    unsigned char row;

    for (row = 0; row < VERA_SCREEN_ROWS; ++row) {
        vera_clear_line(row);
    }
    vera_set_cursor(0, 0);
}

static void vera_draw_demo(void)
{
    vera_clear_text();
    vera_write_text("VERA BACKEND READY");
    vera_put_char(0x9B);
    vera_put_char(0x9B);
    vera_write_text("XIO 34  CLEAR SCREEN");
    vera_put_char(0x9B);
    vera_write_text("XIO 35  DRAW THIS DEMO");
    vera_put_char(0x9B);
    vera_write_text("XIO 37  SET CURSOR  (AX1=X AX2=Y)");
    vera_put_char(0x9B);
    vera_write_text("XIO 36  PUT CHAR    (AX1=ATASCII)");
    vera_put_char(0x9B);
    vera_put_char(0x9B);
    vera_write_text("K: RESTA ATARI OS. E:/S: NON SONO HOOKATI.");
}

void VeraApiService(void)
{
    volatile VeraCtl* ctl = vera_ctl();
    unsigned char request = ctl->request;

    if (request == VERA_REQ_NONE) {
        return;
    }

    switch (request) {
    case VERA_REQ_CLEAR:
        vera_clear_text();
        break;

    case VERA_REQ_DEMO:
        vera_draw_demo();
        break;

    case VERA_REQ_PUTC:
        vera_put_char(ctl->param0);
        break;

    default:
        break;
    }

    ctl->request = VERA_REQ_NONE;
}

static void reserve_resident(void)
{
    unsigned resident_end = (unsigned)&resident_end_marker + 1u;
    unsigned vbi_end = (unsigned)&vera_vbi_end;

    if (resident_end < vbi_end) {
        resident_end = vbi_end;
    }

    if (MEMLO_ADDR < resident_end) {
        MEMLO_ADDR = resident_end;
    }
}

static void init_control_block(void)
{
    volatile VeraCtl* ctl = vera_ctl();
    unsigned char flags = 0x00;

    if (ctl->sig0 == 'V' &&
        ctl->sig1 == 'C' &&
        ctl->sig2 == 'T' &&
        ctl->sig3 == 'L') {
        flags = (unsigned char)(ctl->flags & VERA_CTL_FLAG_METRONOME);
    }

    ctl->flags = flags;
    ctl->request = VERA_REQ_NONE;
    ctl->param0 = 0x00;
    ctl->param1 = 0x00;
    ctl->cursor_x = 0x00;
    ctl->cursor_y = 0x00;
    ctl->entry_lo = (unsigned char)((unsigned)CallVeraApiService & 0xFFu);
    ctl->entry_hi = (unsigned char)(((unsigned)CallVeraApiService >> 8) & 0xFFu);

    ctl->sig0 = 'V';
    ctl->sig1 = 'C';
    ctl->sig2 = 'T';
    ctl->sig3 = 'L';
    ctl->flags |= VERA_CTL_FLAG_API_READY;
}

void VBI(void)
{
    volatile VeraCtl* ctl = vera_ctl();

    if ((ctl->flags & VERA_CTL_FLAG_METRONOME) == 0) {
        if (click_active) {
            AUDC1_ADDR = 0x00;
            click_active = 0;
        }
        frames_until_click = RATE;
        return;
    }

    if (click_active) {
        AUDC1_ADDR = 0x00;
        click_active = 0;
    }

    if (--frames_until_click == 0) {
        AUDF1_ADDR = FREQ;
        AUDC1_ADDR = VOLUME;
        frames_until_click = RATE;
        click_active = 1;
    }
}

int main(void)
{
    reserve_resident();
    init_control_block();
    InitVbi();
    ANTIC.nmien = NMIEN_VBI;
	//for(;;);
    return 0;
}
