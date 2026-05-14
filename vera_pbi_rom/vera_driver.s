; vera_driver.s — VERA PBI driver core (warm reinit + putc state machine).
;
; The putc state machine renders a 40x24 ATASCII viewport at the top-left
; of VERA's 80x25 tilemap. Cursor state lives in the VCTL block so the VBI
; blinker and the PBI ROM warm-recovery path can both see it.

    .setcpu "6502"

    .export _vera_warm_reinit, _CallVeraApiService, _VeraApiService
    .import _vera_x16_font, _vera_ctl_block
    .import _vera_cursor_invalidate

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
; Viewport — 40x24 mirror of an Atari GR.0 screen, top-left aligned in the
; 80x25 VERA tilemap.
; ============================================================================

SCREEN_COLS_VIEW    = 40
SCREEN_ROWS_VIEW    = 24
READY_ROW           = 8             ; row used by warm_reinit's banner

; ============================================================================
; VERA hardware
; ============================================================================

VERA_ADDR_L         = $D100
VERA_ADDR_M         = $D101
VERA_ADDR_H         = $D102
VERA_DATA0          = $D103
VERA_DATA1          = $D104
VERA_CTRL           = $D105
VERA_DC_VIDEO       = $D109

VERA_SCREEN_BASE_M  = $B0
VERA_SCREEN_BANK    = $11           ; bank 1 | INC1
VERA_ADDR_H_BASE    = VERA_SCREEN_BANK
VERA_TEXT_COLOR     = $61           ; (BG=6 blue) (FG=1 white)
VERA_INC1           = $10
VERA_ADDRSEL_CLEAR  = $FE

CHARSET_VRAM_L      = $00
CHARSET_VRAM_M      = $F0
CHARSET_VRAM_H      = $11

; ============================================================================
; ATASCII control set we recognise. Anything else is treated as printable.
; Inverse-video bit ($80) is stripped before tile lookup; the cell color is
; left at VERA_TEXT_COLOR (no inverse rendering yet — see Phase 1A scope).
; ============================================================================

ATASCII_EOL         = $9B
ATASCII_CLEAR       = $7D
ATASCII_BACKSPACE   = $7E
ATASCII_BELL        = $FD
ATASCII_ESC         = $1B
ATASCII_CURSOR_UP   = $1C
ATASCII_CURSOR_DOWN = $1D
ATASCII_CURSOR_LEFT = $1E
ATASCII_CURSOR_RIGHT = $1F

; ============================================================================
; OS equates (only what the warm-reinit banner needs — putc is self-contained)
; ============================================================================

CRITIC      = $42
SETVBV      = $E45C
XITVBV      = $E462

    .segment "LOWBSS"

; Scratch byte used by routines that need a loop counter outside Y/X.
putc_tmp:           .res 1

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

_vera_warm_reinit:
    jsr vera_load_font
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
    beq @done
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    inx
    bne @loop
@done:
    ; Park the cursor at column 0 of the row below the banner so the first
    ; user PRINT lands somewhere visible during Phase 1A interactive tests.
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #(READY_ROW + 1)
    sta _vera_ctl_block + VERACTL_CURSOR_Y
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
    .asciiz "DEVICE HANDLER READY."


; ============================================================================
; _CallVeraApiService — dispatch on VCTL_REQUEST. Currently only PUTC is
; routed; everything else falls through to rts (no-op).
; ============================================================================

_CallVeraApiService:
    lda _vera_ctl_block + VERACTL_REQUEST
    cmp #VERA_REQ_PUTC
    beq _VeraPutByte
    rts


; ============================================================================
; _VeraPutByte — write one ATASCII byte (in VCTL_PARAM0) to the 40x24 VERA
; viewport. Updates cursor X/Y in VCTL, scrolls when EOL pushes past row 23,
; and invalidates the VBI blinker so the new cursor position is honoured on
; the next tick.
; ============================================================================

_VeraPutByte:
    jsr _vera_cursor_invalidate
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
    jmp print_literal
@not_escaped:
    pla

    ; Long-jump dispatch — targets are too far for short branches.
    cmp #ATASCII_EOL
    bne @not_eol
    jmp do_eol
@not_eol:
    cmp #ATASCII_CLEAR
    bne @not_clear
    jmp do_clear
@not_clear:
    cmp #ATASCII_BACKSPACE
    bne @not_bs
    jmp do_backspace
@not_bs:
    cmp #ATASCII_ESC
    bne @not_esc
    jmp do_esc
@not_esc:
    cmp #ATASCII_CURSOR_UP
    bne @not_cu
    jmp do_cursor_up
@not_cu:
    cmp #ATASCII_CURSOR_DOWN
    bne @not_cd
    jmp do_cursor_down
@not_cd:
    cmp #ATASCII_CURSOR_LEFT
    bne @not_cl
    jmp do_cursor_left
@not_cl:
    cmp #ATASCII_CURSOR_RIGHT
    bne @not_cr
    jmp do_cursor_right
@not_cr:
    cmp #ATASCII_BELL
    bne @not_bell
    jmp do_bell
@not_bell:

    ; Default: printable — strip the inverse-video bit so we don't index past
    ; the 128-tile font (inverse rendering deferred to a later iteration).
    and #$7F
    jmp print_literal


; ----------------------------------------------------------------------------
; print_literal — write A to (cursor_x, cursor_y), advance, wrap on overflow.
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
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0

    inc _vera_ctl_block + VERACTL_CURSOR_X
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp #SCREEN_COLS_VIEW
    bcc @done
    jsr cr_lf
@done:
    rts


; ----------------------------------------------------------------------------
; cr_lf — newline: x=0, y++, scroll if past last row.
; ----------------------------------------------------------------------------

cr_lf:
    lda #0
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
; scroll_up — shift rows 1..23 up to rows 0..22, clear row 23.
;
; Uses DATA0 (source) and DATA1 (destination) so a single inner loop streams
; bytes through both ports with one read/write per cycle.
; ----------------------------------------------------------------------------

scroll_up:
    lda #0
    sta putc_tmp                        ; dest row index
@row_loop:
    ; DATA0 → read source row (= dest row + 1)
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
    lda #$00
    sta VERA_CTRL
    lda #0
    sta putc_tmp                        ; current row
@row_loop:
    lda #0
    sta VERA_ADDR_L
    lda putc_tmp
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldy #SCREEN_COLS_VIEW
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    dey
    bne @col_loop
    inc putc_tmp
    lda putc_tmp
    cmp #SCREEN_ROWS_VIEW
    bne @row_loop

    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_X
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    rts


; ----------------------------------------------------------------------------
; do_backspace — ATASCII BS ($7E): x--, blank the cell, no wrap to prev row.
; ----------------------------------------------------------------------------

do_backspace:
    lda _vera_ctl_block + VERACTL_CURSOR_X
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
