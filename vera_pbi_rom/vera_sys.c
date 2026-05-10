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
#define VERA_REQ_HOOKS 0x04
#define VERA_REQ_CURSOR 0x05
#define VERA_REQ_HOOK_PUTC 0x06

#define VERA_SCREEN_COLS 80
#define VERA_SCREEN_ROWS 25
#define VERA_CHAR_BLANK  0x20
#define VERA_TEXT_COLOR  0x61
#define VERA_SCREEN_BASE_M 0xB0
#define VERA_SCREEN_BANK   0x11
#define VERA_CURSOR_RATE   20u

#define LMARGN_ADDR (*(volatile unsigned char*)0x0052)
#define RMARGN_ADDR (*(volatile unsigned char*)0x0053)
#define ROWCRS_ADDR (*(volatile unsigned char*)0x0054)
#define COLCRS_ADDR (*(volatile unsigned char*)0x0055)

#define HATABS_ADDR ((volatile unsigned char*)0x031A)
#define IOCB_ADDR   ((volatile unsigned char*)0x0340)

#define HATABS_SIZE         33u
#define HATABS_ENTRY_SIZE    3u
#define HANDLER_TABLE_SIZE  16u
#define IOCB_SIZE           16u
#define IOCB_COUNT           8u

#define IOCB_ICHID_OFFSET    0u
#define IOCB_ICPTL_OFFSET    6u
#define IOCB_ICPTH_OFFSET    7u

#define EMUOS_EDITRV_ADDR 0xE400u
#define EMUOS_SCRENV_ADDR 0xE410u

extern void InitVbi(void);
extern void CallVeraApiService(void);
extern char vera_vbi_end;
extern unsigned char vera_editrv[];
extern unsigned char vera_screnv[];
extern unsigned char vera_hook_col_before;
extern unsigned char vera_hook_row_before;
extern unsigned vera_orig_editor_put;
extern unsigned vera_orig_screen_put;
extern void VeraEditorPutHook(void);
extern void VeraScreenPutHook(void);

static void install_hook_for_device(unsigned char device_name,
                                    volatile unsigned char* wrapper_table,
                                    unsigned* original_put_vector,
                                    unsigned hook_put_vector);
static void install_es_hooks(void);
static void patch_open_iocbs(unsigned char hatabs_slot, unsigned put_vector);
static void vera_hide_cursor(void);
static void vera_show_cursor(void);
static void vera_redraw_cursor(void);
static void vera_touch_cursor(void);
static void vera_sync_atari_cursor(void);
static void vera_put_hook_char(unsigned char ch);

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
unsigned char vera_cursor_saved_char = VERA_CHAR_BLANK;
unsigned char vera_cursor_saved_color = VERA_TEXT_COLOR;
unsigned char vera_cursor_draw_x;
unsigned char vera_cursor_draw_y;
unsigned char vera_cursor_drawn;
unsigned char vera_cursor_frames = VERA_CURSOR_RATE;
unsigned char vera_cursor_enabled;
unsigned char vera_cursor_phase;

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

static void vera_read_cell(unsigned char column,
                           unsigned char row,
                           unsigned char* ch,
                           unsigned char* color)
{
    vera_set_addr((unsigned char)(column << 1), row);
    *ch = VERA_DATA0;
    *color = VERA_DATA0;
}

static void vera_write_cell(unsigned char column,
                            unsigned char row,
                            unsigned char ch,
                            unsigned char color)
{
    vera_set_addr((unsigned char)(column << 1), row);
    VERA_DATA0 = ch;
    VERA_DATA0 = color;
}

static unsigned char vera_cursor_cell_color(void)
{
    unsigned char text_color = (unsigned char)(vera_cursor_saved_color & 0xF0u);
    unsigned char background_color = (unsigned char)(vera_cursor_saved_color & 0x0Fu);

    if (vera_cursor_phase == 0) {
        return (unsigned char)(text_color | (text_color >> 4));
    }

    return (unsigned char)((background_color << 4) | background_color);
}

static void vera_set_cursor(unsigned char column, unsigned char row)
{
    volatile VeraCtl* ctl = vera_ctl();

    vera_hide_cursor();

    if (column >= VERA_SCREEN_COLS) {
        column = VERA_SCREEN_COLS - 1;
    }
    if (row >= VERA_SCREEN_ROWS) {
        row = VERA_SCREEN_ROWS - 1;
    }

    ctl->cursor_x = column;
    ctl->cursor_y = row;
    vera_touch_cursor();
}

static void vera_set_cell(unsigned char column, unsigned char row, unsigned char ch)
{
    vera_write_cell(column, row, ch, VERA_TEXT_COLOR);
}

static void vera_clear_line(unsigned char row)
{
    unsigned char column;

    vera_set_addr(0x00, row);
    for (column = 0; column < VERA_SCREEN_COLS; ++column) {
        VERA_DATA0 = VERA_CHAR_BLANK;
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

    vera_hide_cursor();

    if (ch == 0x9B || ch == 0x1B || ch == '\n' || ch == '\r') {
        vera_newline();
        vera_touch_cursor();
        return;
    }

    if (ch & 0x80u) {
        ch &= 0x7Fu;
    }

    if (ch == 0x08 || ch == 0x7E) {
        if (ctl->cursor_x != 0) {
            --ctl->cursor_x;
            vera_set_cell(ctl->cursor_x, ctl->cursor_y, VERA_CHAR_BLANK);
        }
        vera_touch_cursor();
        return;
    }

    if (ch < 0x20) {
        vera_touch_cursor();
        return;
    }

    vera_set_cell(ctl->cursor_x, ctl->cursor_y, ch);
    ++ctl->cursor_x;
    if (ctl->cursor_x >= VERA_SCREEN_COLS) {
        vera_newline();
    }
    vera_touch_cursor();
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
    volatile VeraCtl* ctl = vera_ctl();

    vera_hide_cursor();

    for (row = 0; row < VERA_SCREEN_ROWS; ++row) {
        vera_clear_line(row);
    }
    ctl->cursor_x = 0;
    ctl->cursor_y = 0;
    vera_touch_cursor();
}

static void vera_hide_cursor(void)
{
    if (!vera_cursor_drawn) {
        return;
    }

    vera_write_cell(vera_cursor_draw_x,
                    vera_cursor_draw_y,
                    vera_cursor_saved_char,
                    vera_cursor_saved_color);
    vera_cursor_drawn = 0;
}

static void vera_show_cursor(void)
{
    volatile VeraCtl* ctl = vera_ctl();

    if (!vera_cursor_enabled) {
        return;
    }

    vera_read_cell(ctl->cursor_x,
                   ctl->cursor_y,
                   &vera_cursor_saved_char,
                   &vera_cursor_saved_color);
    vera_cursor_draw_x = ctl->cursor_x;
    vera_cursor_draw_y = ctl->cursor_y;
    vera_redraw_cursor();
}

static void vera_redraw_cursor(void)
{
    vera_write_cell(vera_cursor_draw_x,
                    vera_cursor_draw_y,
                    VERA_CHAR_BLANK,
                    vera_cursor_cell_color());
    vera_cursor_drawn = 1;
}

static void vera_touch_cursor(void)
{
    if (!vera_cursor_enabled) {
        return;
    }

    vera_cursor_phase = 0;
    vera_cursor_frames = VERA_CURSOR_RATE;
    vera_show_cursor();
}

static void vera_sync_atari_cursor(void)
{
    vera_set_cursor(COLCRS_ADDR, ROWCRS_ADDR);
}

static void vera_put_hook_char(unsigned char ch)
{
    unsigned char old_column = vera_hook_col_before;
    unsigned char old_row = vera_hook_row_before;

    if (ch == 0x7Du) {
        vera_clear_text();
        vera_sync_atari_cursor();
        return;
    }

    if (ch == 0x9Bu || ch == 0x1Bu || ch == '\n' || ch == '\r') {
        vera_sync_atari_cursor();
        return;
    }

    if (ch & 0x80u) {
        ch &= 0x7Fu;
    }

    if (old_column < VERA_SCREEN_COLS && old_row < VERA_SCREEN_ROWS && ch >= 0x20u) {
        vera_hide_cursor();
        vera_write_cell(old_column, old_row, ch, VERA_TEXT_COLOR);
    }

    vera_sync_atari_cursor();
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
    vera_write_text("XIO 38  ENABLE E:/S: HOOKS");
    vera_put_char(0x9B);
    vera_put_char(0x9B);
    vera_write_text("K: RESTA ATARI OS. E:/S: HOOK MANUALE.");
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

    case VERA_REQ_HOOK_PUTC:
        vera_put_hook_char(ctl->param0);
        break;

    case VERA_REQ_HOOKS:
        install_es_hooks();
        vera_cursor_enabled = 1;
        vera_touch_cursor();
        break;

    case VERA_REQ_CURSOR:
        vera_set_cursor(ctl->cursor_x, ctl->cursor_y);
        break;

    default:
        break;
    }

    ctl->request = VERA_REQ_NONE;
}

static void patch_open_iocbs(unsigned char hatabs_slot, unsigned put_vector)
{
    unsigned char iocb_index;

    for (iocb_index = 0; iocb_index < IOCB_COUNT; ++iocb_index) {
        volatile unsigned char* iocb = IOCB_ADDR + (iocb_index * IOCB_SIZE);

        if (iocb[IOCB_ICHID_OFFSET] != hatabs_slot) {
            continue;
        }

        iocb[IOCB_ICPTL_OFFSET] = (unsigned char)(put_vector & 0xFFu);
        iocb[IOCB_ICPTH_OFFSET] = (unsigned char)((put_vector >> 8) & 0xFFu);
    }
}

static void install_hook_for_device(unsigned char device_name,
                                    volatile unsigned char* wrapper_table,
                                    unsigned* original_put_vector,
                                    unsigned hook_put_vector)
{
    unsigned char hatabs_slot;
    unsigned wrapper_addr = (unsigned)wrapper_table;

    for (hatabs_slot = 0; hatabs_slot < HATABS_SIZE; hatabs_slot += HATABS_ENTRY_SIZE) {
        unsigned current_table_addr;
        volatile unsigned char* current_table;
        unsigned fallback_table_addr;
        unsigned char table_index;

        if (HATABS_ADDR[hatabs_slot] == 0x00) {
            break;
        }
        if (HATABS_ADDR[hatabs_slot] != device_name) {
            continue;
        }

        current_table_addr = (unsigned)HATABS_ADDR[hatabs_slot + 1] |
                             ((unsigned)HATABS_ADDR[hatabs_slot + 2] << 8);
        fallback_table_addr = (device_name == 'E') ? EMUOS_EDITRV_ADDR : EMUOS_SCRENV_ADDR;

        if (current_table_addr == wrapper_addr) {
            current_table_addr = fallback_table_addr;
        }

        current_table = (volatile unsigned char*)current_table_addr;

        for (table_index = 0; table_index < HANDLER_TABLE_SIZE; ++table_index) {
            wrapper_table[table_index] = current_table[table_index];
        }

        *original_put_vector = ((unsigned)wrapper_table[6] |
                                ((unsigned)wrapper_table[7] << 8)) + 1u;

        wrapper_table[6] = (unsigned char)(hook_put_vector & 0xFFu);
        wrapper_table[7] = (unsigned char)((hook_put_vector >> 8) & 0xFFu);

        if ((unsigned)HATABS_ADDR[hatabs_slot + 1] != (wrapper_addr & 0xFFu) ||
            (unsigned)HATABS_ADDR[hatabs_slot + 2] != (wrapper_addr >> 8)) {
            HATABS_ADDR[hatabs_slot + 1] = (unsigned char)(wrapper_addr & 0xFFu);
            HATABS_ADDR[hatabs_slot + 2] = (unsigned char)((wrapper_addr >> 8) & 0xFFu);
        }

        patch_open_iocbs(hatabs_slot, hook_put_vector);
        break;
    }
}

static void install_es_hooks(void)
{
    install_hook_for_device('E',
                            vera_editrv,
                            &vera_orig_editor_put,
                            (unsigned)VeraEditorPutHook - 1u);
    install_hook_for_device('S',
                            vera_screnv,
                            &vera_orig_screen_put,
                            (unsigned)VeraScreenPutHook - 1u);
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

    vera_cursor_saved_char = VERA_CHAR_BLANK;
    vera_cursor_saved_color = VERA_TEXT_COLOR;
    vera_cursor_draw_x = 0x00;
    vera_cursor_draw_y = 0x00;
    vera_cursor_drawn = 0;
    vera_cursor_frames = VERA_CURSOR_RATE;
    vera_cursor_enabled = 0;
    vera_cursor_phase = 0;
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
    }
    else {
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

    if ((ctl->flags & VERA_CTL_FLAG_API_READY) == 0) {
        return;
    }

    if (vera_cursor_frames != 0) {
        --vera_cursor_frames;
        return;
    }

    vera_cursor_frames = VERA_CURSOR_RATE;
    if (!vera_cursor_drawn) {
        return;
    }

    vera_cursor_phase ^= 1u;
    vera_redraw_cursor();
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
