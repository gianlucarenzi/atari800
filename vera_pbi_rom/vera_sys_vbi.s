    .setcpu "6502"

    .export _InitVbi, _vera_vbi_end, _vbi_handler
    .export _vera_save_c_sp, _vera_warm_start
    .export _vera_cursor_invalidate
    .import _VeraApiService, _vera_ctl_block, _vera_warm_reinit
    .import __VERA_EXPORTS__

    .include "vera_common.inc"
    .include "atari.inc"


; ============================================================================
; Resident state (LOWBSS survives warm start)
; ============================================================================

    .segment "LOWBSS"

frames_until_click:  .res 1
click_active:        .res 1

cursor_frames:       .res 1         ; countdown to next blink toggle
cursor_drawn:        .res 1         ; 0 = erased, 1 = drawn (also = phase)
cursor_at_x:         .res 1         ; latched X where cursor is currently drawn
cursor_at_y:         .res 1         ; latched Y where cursor is currently drawn
cursor_saved_char:   .res 1         ; original char under cursor
cursor_saved_color:  .res 1         ; original color under cursor

vera_save_addr_l:    .res 1
vera_save_addr_m:    .res 1
vera_save_addr_h:    .res 1
vera_save_ctrl:      .res 1

nibble_tmp:          .res 1         ; scratch for the color nibble-swap


    .segment "CODE"

; ============================================================================
; _InitVbi — install the deferred VBI and prime state.
; ============================================================================

; EXPORTS offset for _vbi_handler — must stay in sync with vera_stub.s.
EXP_VBI_HANDLER = 10

_InitVbi:
    sei
    ; Reading the relocated _vbi_handler from the EXPORTS table via absolute
    ; addressing — the `#</#>` immediate-byte pattern would survive only
    ; with page-aligned MEMLO (see gen_fixups limitation).
    ldy __VERA_EXPORTS__+EXP_VBI_HANDLER
    ldx __VERA_EXPORTS__+EXP_VBI_HANDLER+1
    lda #7                          ; immediate+deferred (cf. SETVBV docs)
    jsr mSETVBV
    lda #VBI_RATE
    sta frames_until_click
    lda #0
    sta click_active
    lda #$FF
    sta cursor_at_x
    sta cursor_at_y
    lda #0
    sta cursor_drawn
    cli
    rts


; Compatibility shim kept while external callers migrate off the old cc65 path.
_vera_save_c_sp:
    rts


; ============================================================================
; _vera_cursor_invalidate — called by putc before writing VRAM. If the VBI
; has the cursor drawn (inverted block visible), restore the underlying cell
; first so putc doesn't have to fight a stale invert on the next blink.
;
; Forces ADDRSEL=0 because cursor_erase / point_vera_at_latched configure
; the DATA0 address; if a caller left ADDRSEL=1 we'd otherwise clobber the
; DATA1 pointer instead.
; ============================================================================

_vera_cursor_invalidate:
    pha
    txa
    pha
    tya
    pha
    lda VERA_CTRL
    pha
    lda cursor_drawn
    beq @done
    lda VERA_CTRL
    and #VERA_ADDRSEL_CLEAR
    sta VERA_CTRL
    jsr cursor_erase                ; restores saved char/color, sets drawn=0
@done:
    pla
    sta VERA_CTRL
    pla
    tay
    pla
    tax
    pla
    rts


; ============================================================================
; _vera_warm_start — entry referenced by VERACTL_REINIT_LO/HI.
; ============================================================================

_vera_warm_start:
    pha
    txa
    pha
    tya
    pha
    jsr _vera_warm_reinit
    pla
    tay
    pla
    tax
    pla
    rts


; ============================================================================
; _vbi_handler — deferred VBI. Bails out while a critical section is active
; so foreground VRAM writes are never interrupted.
; ============================================================================

_vbi_handler:
    lda CRITIC
    beq @ok
    jmp XITVBV
@ok:
    pha
    txa
    pha
    tya
    pha

    jsr cursor_tick

    pla
    tay
    pla
    tax
    pla
    jmp XITVBV

_vera_vbi_end:

; ============================================================================
; cursor_tick — blink driver. Snapshots VERA state, then toggles between
; "draw" and "erase" once every VBI_CURSOR_RATE frames.
;
; Why latch the position on draw: foreground code can move the desired
; cursor at any time. If we always rendered against the current X/Y, the
; erase phase would clear the WRONG cell after a move, leaving a stuck
; cursor block on screen. Latching cursor_at_x/y at draw time pins the
; matching erase to the same cell.
; ============================================================================

cursor_tick:
    lda ROWCRS_OS
    cmp _vera_ctl_block + VERACTL_CURSOR_Y
    bne @do_sync
    lda COLCRS_OS
    cmp _vera_ctl_block + VERACTL_CURSOR_X
    beq @no_sync
@do_sync:
    ; Sync driver to OS shadow (arrow keys etc.)
    lda ROWCRS_OS
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    lda COLCRS_OS
    sta _vera_ctl_block + VERACTL_CURSOR_X
@no_sync:
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp cursor_at_y
    bne @do_update
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp cursor_at_x
    bne @do_update
    ; Same position — redraw if erased (e.g. by _vera_cursor_invalidate in scroll_up).
    lda cursor_drawn
    bne @done
    jsr cursor_draw
    rts

@do_update:
    lda cursor_drawn
    beq @no_erase
    jsr cursor_erase
@no_erase:
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta cursor_at_y
    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta cursor_at_x
    jsr cursor_draw
@done:
    rts


; ============================================================================
; cursor_draw — save the underlying char + color at (cursor_at_x, cursor_at_y),
; then write the char back with foreground and background swapped.
; ============================================================================

cursor_draw:
    lda cursor_at_x
    cmp #SCREEN_COLS
    bcs @oob
    lda cursor_at_y
    cmp #SCREEN_ROWS_VIEW
    bcs @oob

    jsr point_vera_at_latched

    lda VERA_DATA0
    sta cursor_saved_char
    lda VERA_DATA0
    sta cursor_saved_color

    jsr point_vera_at_latched

    lda cursor_saved_char
    sta VERA_DATA0
    lda cursor_saved_color
    jsr swap_color_nibbles
    sta VERA_DATA0

    lda #1
    sta cursor_drawn
    rts
@oob:
    ; Don't latch a draw — but also clear any stale drawn flag so the
    ; next tick will retry rather than try to erase nothing.
    lda #0
    sta cursor_drawn
    rts


; ============================================================================
; cursor_erase — restore the saved cell at the latched position.
; ============================================================================

cursor_erase:
    jsr point_vera_at_latched
    lda cursor_saved_char
    sta VERA_DATA0
    lda cursor_saved_color
    sta VERA_DATA0
    lda #0
    sta cursor_drawn
    rts


; ============================================================================
; point_vera_at_latched — set ADDR0 to (cursor_at_x, cursor_at_y).
; ============================================================================

point_vera_at_latched:
    lda cursor_at_x
    asl a                           ; X * 2 (X < 80 ⇒ no carry into ADDR_M)
    sta VERA_ADDR_L
    lda #SCREEN_ADDR_M
    clc
    adc cursor_at_y                 ; $B0 + Y (Y < 64 ⇒ no carry into bank)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    rts


; ============================================================================
; swap_color_nibbles — A := (A >> 4) | (A << 4). Swaps the foreground and
; background nibbles of a VERA attribute byte so the cursor cell appears
; inverted on top of the original character.
; ============================================================================

swap_color_nibbles:
    pha                             ; save original
    lsr a
    lsr a
    lsr a
    lsr a                           ; A = high nibble, now in low half
    sta nibble_tmp
    pla                             ; restore original
    asl a
    asl a
    asl a
    asl a                           ; A = low nibble, now in high half
    ora nibble_tmp
    rts
