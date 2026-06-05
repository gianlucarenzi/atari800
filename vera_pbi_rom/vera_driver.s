; vera_driver.s — VERA PBI driver core (warm reinit + putc state machine).
;
; The putc state machine renders a 40x24 ATASCII viewport at the top-left
; of VERA's 80x25 tilemap. Cursor state lives in the VCTL block so the VBI
; blinker and the PBI ROM warm-recovery path can both see it.

    .setcpu "6502"

    .export _vera_warm_reinit, _vera_hw_reinit, _vera_wait_and_clear, _CallVeraApiService, _VeraApiService
    .import _vera_x16_font, _vera_ctl_block
    .import _vera_cursor_invalidate, cursor_draw, cursor_at_x, cursor_at_y, cursor_enabled
    .import _vera_trigger_click, _vera_scroll_hook
    .import nibble_tmp

    .include "vera_common.inc"
    .include "atari.inc"

VER_LINE_ADDR   = SCREEN_ADDR + (1 * MAP_COLS * 2) + (8 * 2)
HOST_LINE_ADDR  = SCREEN_ADDR + (3 * MAP_COLS * 2) + (8 * 2)

VERA_TEXT_COLOR     = TEXT_COLOR
VERA_INVERSE_COLOR  = $16           ; swap nibbles of $61: BG=1 white, FG=6 blue

    .segment "LOWBSS"

; Scratch byte used by routines that need a loop counter outside Y/X.
putc_tmp:           .res 1
; Inverse-video flag set by print_literal, used by clear-row helpers.
putc_inverse:       .res 1
save_nmien:         .res 1
first_init:         .res 1
wait_target:        .res 1      ; RTCLOK+2 frame-count target for _vera_wait_and_clear

    .segment "CODE"

; ============================================================================
; _VeraApiService — placeholder kept for symbol stability across versions
; ============================================================================

_VeraApiService:
    rts

; ============================================================================
; vera_init_hw — configure VERA Layer 1 and display composer registers.
; Safe to call even when the PBI ROM already ran: all writes are idempotent.
; Must be called before vera_load_font (which reads and preserves DC_VIDEO).
; ============================================================================

vera_init_hw:
    ; Layer 1: 128×64 tilemap, mapbase at SCREEN_ADDR, tilebase at CHARSET_ADDR.
    lda #VERA_DCSEL0
    sta VERA_CTRL
    lda #VERA_MAP_128x64
    sta VERA_L1_CONFIG
    lda #SCREEN_MAPBASE
    sta VERA_L1_MAPBASE
    lda #SCREEN_TILEBASE_REG
    sta VERA_L1_TILEBASE
    lda #$00
    sta VERA_L1_HSCR_L
    sta VERA_L1_HSCR_H
    sta VERA_L1_VSCR_L
    sta VERA_L1_VSCR_H

    ; Display composer bank 1: active area clipping for 640×480 VGA.
    lda #VERA_DCSEL1
    sta VERA_CTRL
    lda #DC_HSTART_VAL
    sta VERA_DC_HSTART
    lda #DC_HSTOP_VAL
    sta VERA_DC_HSTOP
    lda #DC_VSTART_VAL
    sta VERA_DC_VSTART
    lda #DC_VSTOP_VAL
    sta VERA_DC_VSTOP

    ; Display composer bank 0: enable VGA output + Layer 1, dynamic scale.
    lda #VERA_DCSEL0
    sta VERA_CTRL
    lda #(VERA_VIDEO_VGA | VERA_LAYER1_EN)
    sta VERA_DC_VIDEO
    lda #HSCALE_VAL
    sta VERA_DC_HSCALE
    lda #VSCALE_VAL
    sta VERA_DC_VSCALE
    lda #$06
    sta VERA_DC_BORDER
    rts

; ============================================================================
; _vera_hw_reinit — lightweight warm restart: reconfigure VERA hw + reload font.
; No busy-wait, no screen clear.  CRITIC=1 during the VERA writes so the VBI
; cursor blinker doesn't race with register accesses.
; Called by common_reinit on every warm reset instead of _vera_warm_reinit.
; ============================================================================

_vera_hw_reinit:
    lda #1
    sta CRITIC
    jsr vera_init_hw
    jsr vera_load_font
    lda #KBD_KRPDEL_FAST
    sta KRPDEL
    lda #KBD_KEYREP_FAST
    sta KEYREP
    lda LMARGN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS
    lda LMARGN
    sta COLCRS
    lda #0
    sta CRITIC
    rts

; ============================================================================
; _vera_wait_and_clear — wait ~2 s then blank the VERA viewport.
;
; Uses RTCLOK+2 ($14), which is incremented on every VBI frame by the OS
; immediate VBI handler regardless of CRITIC. Caller must ensure CRITIC=0
; before calling so RTCLOK advances.
; ============================================================================

WAIT_FRAMES = 100       ; 2.0 s @ 50 Hz (PAL) / 1.67 s @ 60 Hz (NTSC)

_vera_wait_and_clear:
    lda #0
    sta cursor_enabled      ; hide cursor during wait + clear
    lda RTCLOK+2
    clc
    adc #WAIT_FRAMES
    sta wait_target
@wc_wait:
    lda RTCLOK+2
    sec
    sbc wait_target
    bmi @wc_wait
    jmp do_clear            ; tail call; do_clear re-enables cursor on exit

; ============================================================================

_vera_warm_reinit:
    lda #1
    sta CRITIC          ; block deferred VBI cursor during init

    jsr vera_init_hw
    jsr vera_load_font

    lda #KBD_KRPDEL_FAST
    sta KRPDEL
    lda #KBD_KEYREP_FAST
    sta KEYREP

    lda first_init
    bne @warm_reinit_done   ; warm re-run: skip banner, wait, clear

    ; Cold install only: print banner, wait 2 s, clear screen.
    lda #$00
    sta VERA_CTRL
    lda LMARGN
    asl a                               ; x*2 (each cell = char + color byte)
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    inx
    bne @loop
@banner_done:
    lda #1
    sta first_init

    lda #0
    sta CRITIC              ; allow VBI so RTCLOK advances
    jsr _vera_wait_and_clear

@warm_reinit_done:
    ; Cursor init @ (LMARGN, 0)
    lda LMARGN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS
    lda LMARGN
    sta COLCRS
    lda #0
    sta CRITIC
    lda #0
    sta PBI_LATCH
    rts

; ============================================================================
; vera_load_font — copy the 2 KB X16 font into VRAM at the charset address.
; Disables Layer 1 output during upload to avoid flashing garbage tiles.
; ============================================================================

vera_load_font:
    lda #$00
    sta VERA_CTRL
    lda VERA_DC_VIDEO
    pha
    and #$CF
    sta VERA_DC_VIDEO

    ; Initialize the source address for the copy loop.
    ; This avoids persistent self-modification issues on warm start.
    ; We use a local .word slot because the relocator doesn't support
    ; immediate HI/LO patches (lda #< / lda #>).
    lda font_ptr
    sta @src_ptr+1
    lda font_ptr+1
    sta @src_ptr+2

    lda #CHARSET_VRAM_L
    sta VERA_ADDR_L
    lda #CHARSET_VRAM_M
    sta VERA_ADDR_M
    lda #CHARSET_VRAM_H
    sta VERA_ADDR_H

    ldx #FONT_PAGES             ; pages × 256 bytes = 128 chars × TILE_HEIGHT bytes
    ldy #$00
@src_ptr:
@copy_loop:
    lda _vera_x16_font,y        ; Address is patched at runtime by the code above
    sta VERA_DATA0
    iny
    bne @copy_loop
    inc @src_ptr+2              ; Increment the high byte of the source address
    dex
    bne @copy_loop

    pla
    sta VERA_DC_VIDEO
    rts

font_ptr:       .word _vera_x16_font


ReadyText:
    .asciiz "DEVICE DRIVER INSTALLED"


; ============================================================================
; _CallVeraApiService — dispatch on VCTL_REQUEST.
; ============================================================================

    .import kbd_ring_buf, kbd_ring_rd, kbd_ring_wr, kbd_repeat_raw

_CallVeraApiService:
    lda #1
    sta CRITIC
    lda _vera_ctl_block + VERACTL_REQUEST
    cmp #VERA_REQ_PUTC
    beq @do_putc
    cmp #VERA_REQ_GETC
    beq @do_getc
    cmp #VERA_REQ_FLUSH_KBD
    beq @do_flush_kbd
    lda #0
    sta CRITIC
    rts
@do_putc:
    jsr _VeraPutByte
    lda #0
    sta CRITIC
    rts
@do_getc:
    jsr _VeraGetByte
    lda #0
    sta CRITIC
    rts
@do_flush_kbd:
    jsr _VeraFlushKbd
    lda #0
    sta CRITIC
    rts


; ============================================================================
; _VeraGetByte — read one character from the keyboard ring buffer.
; Returns char in VERACTL_PARAM0, or $FF if empty.
; ============================================================================

_VeraGetByte:
    lda kbd_ring_rd
    cmp kbd_ring_wr
    beq @empty
    
    tax
    lda kbd_ring_buf, x
    sta _vera_ctl_block + VERACTL_PARAM0
    
    inx
    txa
    and #$0F
    sta kbd_ring_rd
    rts

@empty:
    lda #$FF
    sta _vera_ctl_block + VERACTL_PARAM0
    rts


; ============================================================================
; _VeraFlushKbd — flush the keyboard ring and cancel any pending key-repeat.
; Called via VERA_REQ_FLUSH_KBD. Atomically empties the ring (wr = rd) and
; resets kbd_repeat_raw to KEY_NONE so the repeat tick can't re-inject the
; last pressed key. Also clears VERACTL_PARAM0 so callers never read stale
; PUTC data as a spurious keypress.
; ============================================================================

_VeraFlushKbd:
    sei
    lda kbd_ring_rd
    sta kbd_ring_wr             ; empty ring: wr = rd
    lda #KEY_NONE               ; = $FF
    sta kbd_repeat_raw          ; cancel repeat state
    cli
    sta _vera_ctl_block + VERACTL_PARAM0    ; PARAM0 = $FF (no key pending)
    rts


; ============================================================================
; _VeraPutByte — write one ATASCII byte (in VCTL_PARAM0) to the 80x30 VERA
; viewport. Updates cursor X/Y in VCTL, scrolls when EOL pushes past row 29,
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
    ; Honour bit 7 (inverse video) exactly like the default printable path.
    pha
    and #$80
    sta putc_inverse
    pla
    and #$7F
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
    ; Sync position to OS shadows and latch for VBI, then draw cursor immediately
    ; so it is never invisible after any putc/scroll/edit operation.
    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta cursor_at_x
    sta COLCRS
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta cursor_at_y
    sta ROWCRS
    jsr cursor_draw
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
    ; Inverse: swap fg/bg nibbles of current color.
    lda _vera_ctl_block + VERACTL_PARAM1
    pha
    lsr a
    lsr a
    lsr a
    lsr a               ; A = bg nibble in low half
    sta nibble_tmp
    pla
    asl a
    asl a
    asl a
    asl a               ; A = fg nibble in high half
    ora nibble_tmp      ; A = (fg<<4)|bg = swapped
    bne @write_color
@normal_color:
    lda _vera_ctl_block + VERACTL_PARAM1
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
; cr_lf — newline: x=LMARGN, y++, scroll if past last row.
; ----------------------------------------------------------------------------

cr_lf:
    lda LMARGN
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


scroll_up:
    jsr _vera_scroll_hook       ; keep E: logical-line tracking in sync
    jsr _vera_cursor_invalidate
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

    ldy #(SCREEN_COLS_VIEW * 2 / 8)         ; 160 / 8 = 20 iterations
@byte_loop:
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
    lda VERA_DATA0
    sta VERA_DATA1
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    dey
    bne @clear_loop

    pla                         ; Restore ANTIC DMA state
    sta DMACTL
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
    lda #1
    sta CRITIC              ; block deferred VBI so cursor blinker can't race VERA_CTRL
    jsr _vera_cursor_invalidate
    lda #0                  ; Ensure ADDRSEL=0
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

    ldy #MAP_COLS               ; 128 tiles × 2 bytes = 256 bytes per row
@col_loop:
    lda #' '
    sta VERA_DATA0
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    dey
    bne @col_loop

    inc putc_tmp
    lda putc_tmp
    cmp #64                     ; Clear all 64 rows of the 128x64 map
    bne @row_loop

    lda LMARGN
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #0
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    lda #1
    sta cursor_enabled      ; cursor visible from now on
    lda #0
    sta CRITIC              ; re-enable deferred VBI
    rts


; ----------------------------------------------------------------------------
; do_backspace — ATASCII BS ($7E): x--, blank the cell, no wrap to prev row.
; ----------------------------------------------------------------------------

do_backspace:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp LMARGN
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
    lda _vera_ctl_block + VERACTL_PARAM1
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
    cmp LMARGN
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
; do_bell — ATASCII BELL ($FD): trigger a brief audio click.
; ----------------------------------------------------------------------------

do_bell:
    jmp _vera_trigger_click


; ----------------------------------------------------------------------------
; do_tab — ATASCII TAB ($7F): advance X to next multiple of 8, and
; clamp at SCREEN_COLS_VIEW.
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
    jsr _vera_cursor_invalidate
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    dey
    bne @dl_clr_loop
    rts


; ----------------------------------------------------------------------------
; do_insert_line — ATASCII $9D: shift rows cursor_y..22 down one, clear row
;                  cursor_y.
; ----------------------------------------------------------------------------

do_insert_line:
    jsr _vera_cursor_invalidate
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    dey
    bne @il_clr_loop
    rts


; ----------------------------------------------------------------------------
; do_delete_char — ATASCII $FE: shift cells cursor_x+1..SCREEN_COLS_VIEW left,
; blank col SCREEN_COLS_VIEW. Uses DATA0/DATA1 with sequential read/write:
; set DATA0 one cell ahead of DATA1, read from DATA0 then write to DATA1
; in each iteration.
; ----------------------------------------------------------------------------

do_delete_char:
    jsr _vera_cursor_invalidate
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
    beq @dc_blank                       ; cursor already at col SCREEN_COLS_VIEW
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
    ; Blank column SCREEN_COLS_VIEW.
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    rts


; ----------------------------------------------------------------------------
; do_insert_char — ATASCII $FF: shift cells cursor_x..78 right, blank cursor.
; Iterates from col 78 down to cursor_x to avoid overlap.
; For each column, reads char+color from col N into A/X, writes to col N+1.
; ----------------------------------------------------------------------------

do_insert_char:
    jsr _vera_cursor_invalidate
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    rts

_pbi_clear_screen:
    lda DMACTL                  ; Save ANTIC DMA state
    pha
    lda #0
    sta DMACTL                  ; Disable ANTIC DMA
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
    lda _vera_ctl_block + VERACTL_PARAM1
    sta VERA_DATA0
    dex
    bne @Col
    dey
    bne @Row
    pla                         ; Restore ANTIC DMA state
    sta DMACTL
    rts
