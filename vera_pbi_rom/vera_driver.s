; vera_driver.s — VERA PBI driver core (warm reinit + putc state machine).
;
; The putc state machine renders a 40x24 ATASCII viewport at the top-left
; of VERA's 80x25 tilemap. Cursor state lives in the VCTL block so the VBI
; blinker and the PBI ROM warm-recovery path can both see it.

    .setcpu "6502"

    .export _vera_warm_reinit, _CallVeraApiService, _VeraApiService
    .import _vera_x16_font, _vera_ctl_block
    .import _vera_cursor_invalidate, cursor_draw

; ============================================================================
; VCTL block offsets — must stay in sync with vera_stub.s + the bootstrap.
; ============================================================================

VERACTL_FLAGS       = 4
VERACTL_REQUEST     = 5
VERACTL_PARAM0      = 6
VERACTL_PARAM1      = 7
VERACTL_CURSOR_X    = 8
VERACTL_CURSOR_Y    = 9

VCTL_FLAG_METRONOME = $01
VCTL_FLAG_ESCAPE    = $02           ; next byte is treated as literal
VCTL_FLAG_API_READY = $80

VERA_REQ_PUTC       = $03

; ============================================================================
; Viewport — 80x24 primary display, top-left aligned in the 128x64 VERA
; tilemap. VERA is the authoritative screen; Atari screen RAM is not used.
; ============================================================================

SCREEN_COLS_VIEW    = 80
SCREEN_ROWS_VIEW    = 60
READY_ROW           = 8             ; row used by warm_reinit's banner

; ============================================================================
; VERA hardware register base and register names (synced with vera_pbi_handler.s)
; ============================================================================

PBI_ADDR        = $D100

VERA_ADDR_L     = PBI_ADDR + $00    ; VRAM address bits  7:0  (active port)
VERA_ADDR_M     = PBI_ADDR + $01    ; VRAM address bits 15:8
VERA_ADDR_H     = PBI_ADDR + $02    ; bit[0]=A16  bits[7:4]=INCR
VERA_DATA0      = PBI_ADDR + $03    ; VRAM data port 0
VERA_DATA1      = PBI_ADDR + $04    ; VRAM data port 1
VERA_CTRL_REG   = PBI_ADDR + $05    ; CTRL: ADDRSEL(0) DCSEL(1) RESET(7)
VERA_IEN        = PBI_ADDR + $06    ; Interrupt enable
VERA_ISR        = PBI_ADDR + $07    ; Interrupt status (write 1 to clear)

VERA_DC_VIDEO   = PBI_ADDR + $09    ; Output enable, layer enable, sprites
VERA_DC_HSCALE  = PBI_ADDR + $0A    ; Horizontal scale (128 = 1:1)
VERA_DC_VSCALE  = PBI_ADDR + $0B    ; Vertical scale
VERA_DC_BORDER  = PBI_ADDR + $0C    ; Border colour index

; ============================================================================
; VERA constants (synced with vera_pbi_handler.s)
; ============================================================================

VERA_INC0       = $00           ; No auto-increment
VERA_INC1       = $10           ; Auto-increment by 1

VERA_DCSEL0     = $00           ; Access DC_VIDEO/HSCALE/VSCALE/BORDER bank
VERA_DCSEL1     = $02           ; Access DC_HSTART/HSTOP/VSTART/VSTOP bank

VERA_VIDEO_VGA  = $01           ; VGA output (640x480)
VERA_LAYER1_EN  = $20           ; Enable Layer 1

VERA_MAP_128x64 = $60           ; 128-tile wide, 64-tile tall map

SCREEN_ADDR     = $01B000       ; Tilemap start (128x64 = 8 KB, in bank 1)
CHARSET_ADDR    = $01F000       ; Character glyphs (256 chars x 8 bytes)

SCREEN_MAPBASE  = $D8           ; L1_MAPBASE  = SCREEN_ADDR >> 9
SCREEN_TILEBASE = $F8           ; L1_TILEBASE = CHARSET_ADDR >> 9, 8x8 tiles

MAP_COLS        = 128
MAP_ROWS        = 64
TEXT_COLOR      = $61           ; White on blue

DC_HSTART_VAL   = $00
DC_HSTOP_VAL    = $A0
DC_VSTART_VAL   = $00
DC_VSTOP_VAL    = $F0

LOGO1_ADDR      = SCREEN_ADDR + (0 * MAP_COLS * 2) + (0 * 2)
LOGO2_ADDR      = SCREEN_ADDR + (1 * MAP_COLS * 2) + (0 * 2)
LOGO3_ADDR      = SCREEN_ADDR + (2 * MAP_COLS * 2) + (0 * 2)
LOGO4_ADDR      = SCREEN_ADDR + (3 * MAP_COLS * 2) + (0 * 2)
LOGO5_ADDR      = SCREEN_ADDR + (4 * MAP_COLS * 2) + (0 * 2)
LOGO6_ADDR      = SCREEN_ADDR + (5 * MAP_COLS * 2) + (0 * 2)
LOGO7_ADDR      = SCREEN_ADDR + (6 * MAP_COLS * 2) + (0 * 2)
VER_LINE_ADDR   = SCREEN_ADDR + (1 * MAP_COLS * 2) + (8 * 2)
HOST_LINE_ADDR  = SCREEN_ADDR + (3 * MAP_COLS * 2) + (8 * 2)

; Internal driver shims for legacy code
VERA_SCREEN_BASE_M  = >SCREEN_ADDR
VERA_ADDR_H_BASE    = (VERA_INC1 | ^SCREEN_ADDR)
VERA_TEXT_COLOR     = TEXT_COLOR
VERA_CTRL           = VERA_CTRL_REG

CHARSET_VRAM_L      = <CHARSET_ADDR
CHARSET_VRAM_M      = >CHARSET_ADDR
CHARSET_VRAM_H      = (VERA_INC1 | ^CHARSET_ADDR)

; ============================================================================
; ATASCII control set we recognise. Anything else is treated as printable.
; Inverse-video bit ($80) is stripped before tile lookup; the cell color is
; left at VERA_TEXT_COLOR (no inverse rendering yet — see Phase 1A scope).
; ============================================================================

ATASCII_EOL         = $9B
ATASCII_CLEAR       = $7D
ATASCII_BACKSPACE   = $7E
ATASCII_TAB         = $7F
ATASCII_BELL        = $FD
ATASCII_ESC         = $1B
ATASCII_CURSOR_UP   = $1C
ATASCII_CURSOR_DOWN = $1D
ATASCII_CURSOR_LEFT = $1E
ATASCII_CURSOR_RIGHT = $1F
ATASCII_DELETE_LINE = $9C
ATASCII_INSERT_LINE = $9D
ATASCII_DELETE_CHAR = $FE
ATASCII_INSERT_CHAR = $FF

VERA_INVERSE_COLOR  = $16           ; swap nibbles of $61: BG=1 white, FG=6 blue

; ============================================================================
; OS equates (only what the warm-reinit banner needs — putc is self-contained)
; ============================================================================

CRITIC      = $42
SETVBV      = $E45C
XITVBV      = $E462
LMARGIN     = $52

    .segment "LOWBSS"

; Scratch byte used by routines that need a loop counter outside Y/X.
putc_tmp:           .res 1
; Inverse-video flag set by print_literal, used by clear-row helpers.
putc_inverse:       .res 1
save_nmien:         .res 1
first_init:         .res 1

    .segment "CODE"

; ============================================================================
; _VeraApiService — placeholder kept for symbol stability across versions
; ============================================================================

_VeraApiService:
    rts


; ============================================================================
; _vera_warm_reinit — uploads the font, draws the boot banner. Called by the
; bootstrap at install and by the warm-start hook on every reset.
; ============================================================================
RTCLOK = $14
ROWCRS = $54
COLCRS = $55

_vera_warm_reinit:
    jsr vera_load_font

    lda first_init
    bne @skip_banner

    ; Print Banner (Cold Start only)
    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda #(VERA_SCREEN_BASE_M + READY_ROW)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldx #0
@loop:
    lda ReadyText,x
    beq @banner_done
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    inx
    bne @loop
@banner_done:
    lda #1
    sta first_init

@skip_banner:
    ; Wait 
    lda #250
    ldx RTCLOK
@wait:
    cpx RTCLOK
    beq @wait
    dex
    bne @wait

    jsr do_clear

    ; Inizializzazione cursore a (LMARGIN,0)
    lda LMARGIN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS
    lda LMARGIN
    sta COLCRS
    rts

; ============================================================================
; vera_load_font — copy the 1 KB X16 font into VRAM at the charset address.
; Disables Layer 1 output during upload to avoid flashing garbage tiles.
; ============================================================================

vera_load_font:
    lda #$00
    sta VERA_CTRL
    lda VERA_DC_VIDEO
    pha
    and #$CF
    sta VERA_DC_VIDEO
    lda #CHARSET_VRAM_L
    sta VERA_ADDR_L
    lda #CHARSET_VRAM_M
    sta VERA_ADDR_M
    lda #CHARSET_VRAM_H
    sta VERA_ADDR_H
    ldx #$00
@copy_page0:
    lda _vera_x16_font,x
    sta VERA_DATA0
    inx
    bne @copy_page0
    ldx #$00
@copy_page1:
    lda _vera_x16_font + $100,x
    sta VERA_DATA0
    inx
    bne @copy_page1
    ldx #$00
@copy_page2:
    lda _vera_x16_font + $200,x
    sta VERA_DATA0
    inx
    bne @copy_page2
    ldx #$00
@copy_page3:
    lda _vera_x16_font + $300,x
    sta VERA_DATA0
    inx
    bne @copy_page3
    pla
    sta VERA_DC_VIDEO
    rts


ReadyText:
    .asciiz "DEVICE DRIVER INSTALLED"


; ============================================================================
; _CallVeraApiService — dispatch on VCTL_REQUEST. Currently only PUTC is
; routed; everything else falls through to rts (no-op).
; ============================================================================

_CallVeraApiService:
    sei
    lda #1
    sta CRITIC
    lda _vera_ctl_block + VERACTL_REQUEST
    cmp #VERA_REQ_PUTC
    beq @do_putc
    lda #0
    sta CRITIC
    cli
    rts
@do_putc:
    jsr _VeraPutByte
    lda #0
    sta CRITIC
    cli
    rts


; ============================================================================
; _VeraPutByte — write one ATASCII byte (in VCTL_PARAM0) to the 40x24 VERA
; viewport. Updates cursor X/Y in VCTL, scrolls when EOL pushes past row 23,
; and invalidates the VBI blinker so the new cursor position is honoured on
; the next tick.
; ============================================================================

_VeraPutByte:
    jsr _vera_cursor_invalidate
    ; Reset inverse flag; will be set only by printable chars with bit 7.
    lda #$00
    sta putc_inverse
    lda _vera_ctl_block + VERACTL_PARAM0

    ; If the previous byte was ESC, render this one literally and clear flag.
    pha
    lda _vera_ctl_block + VERACTL_FLAGS
    and #VCTL_FLAG_ESCAPE
    beq @not_escaped
    lda _vera_ctl_block + VERACTL_FLAGS
    and #($FF - VCTL_FLAG_ESCAPE)
    sta _vera_ctl_block + VERACTL_FLAGS
    pla
    jsr print_literal
    jmp @done_putc
@not_escaped:
    pla

    ; Dispatch handlers.
    cmp #ATASCII_EOL
    bne @not_eol
    jsr do_eol
    jmp @done_putc
@not_eol:
    cmp #ATASCII_CLEAR
    bne @not_clear
    jsr do_clear
    jmp @done_putc
@not_clear:
    cmp #ATASCII_BACKSPACE
    bne @not_bs
    jsr do_backspace
    jmp @done_putc
@not_bs:
    cmp #ATASCII_ESC
    bne @not_esc
    jsr do_esc
    jmp @done_putc
@not_esc:
    cmp #ATASCII_CURSOR_UP
    bne @not_cu
    jsr do_cursor_up
    jmp @done_putc
@not_cu:
    cmp #ATASCII_CURSOR_DOWN
    bne @not_cd
    jsr do_cursor_down
    jmp @done_putc
@not_cd:
    cmp #ATASCII_CURSOR_LEFT
    bne @not_cl
    jsr do_cursor_left
    jmp @done_putc
@not_cl:
    cmp #ATASCII_CURSOR_RIGHT
    bne @not_cr
    jsr do_cursor_right
    jmp @done_putc
@not_cr:
    cmp #ATASCII_BELL
    bne @not_bell
    jsr do_bell
    jmp @done_putc
@not_bell:
    cmp #ATASCII_TAB
    bne @not_tab
    jsr do_tab
    jmp @done_putc
@not_tab:
    cmp #ATASCII_DELETE_LINE
    bne @not_dl
    jsr do_delete_line
    jmp @done_putc
@not_dl:
    cmp #ATASCII_INSERT_LINE
    bne @not_il
    jsr do_insert_line
    jmp @done_putc
@not_il:
    cmp #ATASCII_DELETE_CHAR
    bne @not_dc
    jsr do_delete_char
    jmp @done_putc
@not_dc:
    cmp #ATASCII_INSERT_CHAR
    bne @not_ic
    jsr do_insert_char
    jmp @done_putc
@not_ic:

    ; Default: printable. Bit 7 signals inverse video.
    and #$80
    sta putc_inverse
    lda _vera_ctl_block + VERACTL_PARAM0
    and #$7F
    jsr print_literal

@done_putc:
    ; Sync back to OS shadow registers.
    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta COLCRS
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS
    rts


; ----------------------------------------------------------------------------
; print_literal — write A to (cursor_x, cursor_y), advance, wrap on overflow.
; putc_inverse must be set before entry: $80 = inverse, $00 = normal.
; ----------------------------------------------------------------------------

print_literal:
    pha
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a                               ; x*2 (each cell = char + color byte)
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    pla
    sta VERA_DATA0
    ; Choose normal or inverse color based on putc_inverse flag.
    lda putc_inverse
    beq @normal_color
    lda #VERA_INVERSE_COLOR
    bne @write_color
@normal_color:
    lda #VERA_TEXT_COLOR
@write_color:
    sta VERA_DATA0
    ; Clear inverse flag for next call.
    lda #$00
    sta putc_inverse

    inc _vera_ctl_block + VERACTL_CURSOR_X
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp #SCREEN_COLS_VIEW
    bcc @done
    jsr cr_lf
@done:
    rts


; ----------------------------------------------------------------------------
; cr_lf — newline: x=LMARGIN, y++, scroll if past last row.
; ----------------------------------------------------------------------------

cr_lf:
    lda LMARGIN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    inc _vera_ctl_block + VERACTL_CURSOR_Y
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp #SCREEN_ROWS_VIEW
    bcc @done
    ; Keep cursor on the last row; scroll up to make room.
    lda #(SCREEN_ROWS_VIEW - 1)
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    jsr scroll_up
@done:
    rts


; ----------------------------------------------------------------------------
; scroll_up — shift rows 1..59 up to rows 0..58, clear row 59.
;
; Uses DATA0 (source) and DATA1 (destination) so a single inner loop streams
; bytes through both ports with one read/write per cycle.
; Optimized by disabling interrupts and ANTIC DMA.
; ----------------------------------------------------------------------------

DMACTL      = $022F

scroll_up:
    sei
    lda #1                      ; Set Critical Section
    sta CRITIC
    lda DMACTL                  ; Save ANTIC DMA state
    pha
    lda #0                      ; Disable ANTIC DMA
    sta DMACTL

    lda #0
    sta putc_tmp                ; dest row index
@row_loop:
    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; DATA1 → write dest row
    lda #$01
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ldy #(SCREEN_COLS_VIEW * 2)         ; 80 bytes per row (40 char + 40 color)
@byte_loop:
    lda VERA_DATA0
    sta VERA_DATA1
    dey
    bne @byte_loop

    ; Back to DATA0 for the next setup.
    lda #$00
    sta VERA_CTRL

    inc putc_tmp
    lda putc_tmp
    cmp #(SCREEN_ROWS_VIEW - 1)
    bne @row_loop

    ; Clear the freshly-vacated last row.
    lda #0
    sta VERA_ADDR_L
    lda #(VERA_SCREEN_BASE_M + SCREEN_ROWS_VIEW - 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldy #SCREEN_COLS_VIEW
@clear_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @clear_loop

    pla                         ; Restore ANTIC DMA state
    sta DMACTL
    lda #0                      ; Clear Critical Section
    sta CRITIC
    cli
    rts


; ----------------------------------------------------------------------------
; do_eol — ATASCII EOL ($9B): newline.
; ----------------------------------------------------------------------------

do_eol:
    jsr cr_lf
    rts


; ----------------------------------------------------------------------------
; do_clear — ATASCII CLEAR ($7D): blank the viewport, cursor to (0,0).
; ----------------------------------------------------------------------------

do_clear:
    sei
    lda #0                      ; Ensure ADDRSEL=0
    sta VERA_CTRL
    
    lda #0
    sta putc_tmp                ; row counter
@row_loop:
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE       ; Bank 1, INC=1
    sta VERA_ADDR_H

    ldy #0                      ; 256 bytes = 128 tiles
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @col_loop

    inc putc_tmp
    lda putc_tmp
    cmp #64                     ; Clear all 64 rows of the 128x64 map
    bne @row_loop

    lda LMARGIN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    cli
    rts


; ----------------------------------------------------------------------------
; do_backspace — ATASCII BS ($7E): x--, blank the cell, no wrap to prev row.
; ----------------------------------------------------------------------------

do_backspace:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp LMARGIN
    beq @done
    dec _vera_ctl_block + VERACTL_CURSOR_X
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
@done:
    rts


; ----------------------------------------------------------------------------
; do_esc — ATASCII ESC ($1B): set the flag so the next byte prints literal.
; ----------------------------------------------------------------------------

do_esc:
    lda _vera_ctl_block + VERACTL_FLAGS
    ora #VCTL_FLAG_ESCAPE
    sta _vera_ctl_block + VERACTL_FLAGS
    rts


; ----------------------------------------------------------------------------
; do_cursor_* — clamp at viewport edges, no wrap, no scroll on arrow keys.
; ----------------------------------------------------------------------------

do_cursor_up:
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    beq @done
    dec _vera_ctl_block + VERACTL_CURSOR_Y
@done:
    rts

do_cursor_down:
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp #(SCREEN_ROWS_VIEW - 1)
    bcs @done
    inc _vera_ctl_block + VERACTL_CURSOR_Y
@done:
    rts

do_cursor_left:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp LMARGIN
    beq @done
    dec _vera_ctl_block + VERACTL_CURSOR_X
@done:
    rts

do_cursor_right:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp #(SCREEN_COLS_VIEW - 1)
    bcs @done
    inc _vera_ctl_block + VERACTL_CURSOR_X
@done:
    rts


; ----------------------------------------------------------------------------
; do_bell — ATASCII BELL ($FD): visual no-op (Phase 1A doesn't touch POKEY).
; ----------------------------------------------------------------------------

do_bell:
    rts


; ----------------------------------------------------------------------------
; do_tab — ATASCII TAB ($7F): advance X to next multiple of 8, clamp at 79.
; ----------------------------------------------------------------------------

do_tab:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    clc
    adc #8
    and #$F8                            ; round down to multiple of 8
    cmp #SCREEN_COLS_VIEW
    bcc @ok
    lda #(SCREEN_COLS_VIEW - 1)
@ok:
    sta _vera_ctl_block + VERACTL_CURSOR_X
    rts


; ----------------------------------------------------------------------------
; do_delete_line — ATASCII $9C: scroll rows cursor_y+1..23 up one, clear 23.
; ----------------------------------------------------------------------------

do_delete_line:
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta putc_tmp                        ; first dest row = cursor_y
@dl_row:
    lda putc_tmp
    cmp #(SCREEN_ROWS_VIEW - 1)
    beq @dl_clear                       ; last row: just clear it
    ; DATA0 = source row (putc_tmp + 1), DATA1 = dest row (putc_tmp).
    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    lda #$01
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ldy #(SCREEN_COLS_VIEW * 2)
@dl_copy:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @dl_copy

    lda #$00
    sta VERA_CTRL
    inc putc_tmp
    bne @dl_row                         ; always taken (putc_tmp < number of rows)

@dl_clear:
    ; Clear the last row.
    lda #0
    sta VERA_ADDR_L
    lda #(VERA_SCREEN_BASE_M + SCREEN_ROWS_VIEW - 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldy #SCREEN_COLS_VIEW
@dl_clr_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @dl_clr_loop
    rts


; ----------------------------------------------------------------------------
; do_insert_line — ATASCII $9D: shift rows cursor_y..22 down one, clear row
;                  cursor_y.
; ----------------------------------------------------------------------------

do_insert_line:
    ; Start from row 22 (second-to-last) and move down to cursor_y.
    lda #(SCREEN_ROWS_VIEW - 2)
    sta putc_tmp
@il_row:
    lda putc_tmp
    cmp _vera_ctl_block + VERACTL_CURSOR_Y
    bcc @il_clear                       ; gone past cursor_y: clear that row
    ; DATA0 = source row (putc_tmp), DATA1 = dest row (putc_tmp + 1).
    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    lda #$01
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ldy #(SCREEN_COLS_VIEW * 2)
@il_copy:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @il_copy

    lda #$00
    sta VERA_CTRL
    dec putc_tmp
    bpl @il_row

@il_clear:
    ; Clear the cursor row.
    lda #0
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldy #SCREEN_COLS_VIEW
@il_clr_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @il_clr_loop
    rts


; ----------------------------------------------------------------------------
; do_delete_char — ATASCII $FE: shift cells cursor_x+1..79 left, blank col 79.
; Uses DATA0/DATA1 with sequential read/write: set DATA0 one cell ahead of
; DATA1, read from DATA0 then write to DATA1 in each iteration.
; ----------------------------------------------------------------------------

do_delete_char:
    lda #$00
    sta VERA_CTRL
    ; DATA1 → dest: cursor_x cell.
    lda #$01
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; DATA0 → source: cursor_x + 1 cell.
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    clc
    adc #1
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; Count of cells to copy = (SCREEN_COLS_VIEW - 1 - cursor_x), × 2 bytes.
    lda #(SCREEN_COLS_VIEW - 1)
    sec
    sbc _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    beq @dc_blank                       ; cursor already at col 79
    tay
@dc_copy:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @dc_copy

@dc_blank:
    ; Blank column 79.
    lda #$00
    sta VERA_CTRL
    lda #((SCREEN_COLS_VIEW - 1) * 2)
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    rts


; ----------------------------------------------------------------------------
; do_insert_char — ATASCII $FF: shift cells cursor_x..78 right, blank cursor.
; Iterates from col 78 down to cursor_x to avoid overlap.
; For each column, reads char+color from col N into A/X, writes to col N+1.
; ----------------------------------------------------------------------------

do_insert_char:
    lda #(SCREEN_COLS_VIEW - 2)
    sta putc_tmp
@ic_shift:
    lda putc_tmp
    cmp _vera_ctl_block + VERACTL_CURSOR_X
    bcc @ic_blank                       ; gone past cursor_x

    lda #$00
    sta VERA_CTRL
    ; Read char+color from column putc_tmp.
    lda putc_tmp
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda VERA_DATA0                      ; char
    pha
    lda VERA_DATA0                      ; color (INC1 auto-advanced addr)
    tax                                 ; save color in X

    ; Write char+color to column putc_tmp + 1.
    lda putc_tmp
    clc
    adc #1
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    pla                                 ; char
    sta VERA_DATA0
    txa                                 ; color
    sta VERA_DATA0

    dec putc_tmp
    bpl @ic_shift

@ic_blank:
    ; Blank the cursor cell.
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    sta VERA_ADDR_L
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    rts

_pbi_clear_screen:
    lda #<SCREEN_ADDR
    sta VERA_ADDR_L
    lda #>SCREEN_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^SCREEN_ADDR)
    sta VERA_ADDR_H
    ldy #MAP_ROWS
@Row:
    ldx #MAP_COLS
@Col:
    lda #' '
    sta VERA_DATA0
    lda #TEXT_COLOR
    sta VERA_DATA0
    dex
    bne @Col
    dey
    bne @Row
    rts
