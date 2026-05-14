    .setcpu "6502"

    .export _InitVbi, _vera_vbi_end, _vbi_handler
    .export _vera_save_c_sp, _vera_warm_start
    .export _vera_cursor_invalidate
    .import _VeraApiService, _vera_ctl_block, _vera_warm_reinit
    .import __VERA_EXPORTS__

    .include "atari.inc"

; ============================================================================
; VERA hardware registers
; ============================================================================

VERA_ADDR_L         = $D100
VERA_ADDR_M         = $D101
VERA_ADDR_H         = $D102
VERA_DATA0          = $D103
VERA_CTRL           = $D105

VERA_INC1           = $10           ; ADDR_H[7:4] = 1 → auto-increment by 1
VERA_ADDRSEL_CLEAR  = $FE           ; mask to clear CTRL bit0 (select ADDR0)

; ============================================================================
; Screen layout — must stay in sync with vera_pbi_handler.s
;   SCREEN_ADDR  = $01B000    (bank 1, mid byte $B0)
;   MAP_COLS     = 128        → per-row stride = 256 bytes
;   visible 80x25 inside a 128x64 tilemap
; ============================================================================

SCREEN_ADDR_M       = $B0
SCREEN_ADDR_BANK    = $01
VERA_ADDR_H_BASE    = VERA_INC1 | SCREEN_ADDR_BANK  ; $11

SCREEN_COLS         = 80
SCREEN_ROWS         = 25

; ============================================================================
; VeraCtl block offsets
; ============================================================================

VERACTL_FLAGS       = 4
VERACTL_CURSOR_X    = 8
VERACTL_CURSOR_Y    = 9
VERA_CTL_FLAG_METRONOME = $01
VERA_CTL_FLAG_API_READY = $80

; ============================================================================
; Timing / audio
; ============================================================================

VBI_RATE            = 10            ; frames between metronome ticks
VBI_FREQ            = $08
VBI_VOLUME          = $AF
VBI_CURSOR_RATE     = 20            ; frames between cursor blink toggles

SETVBV              = $E45C

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
    jsr SETVBV
    lda #VBI_RATE
    sta frames_until_click
    lda #0
    sta click_active
    sta cursor_drawn                ; start in erased state
    lda #VBI_CURSOR_RATE
    sta cursor_frames
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
    lda cursor_drawn
    beq @done
    lda VERA_CTRL
    and #VERA_ADDRSEL_CLEAR
    sta VERA_CTRL
    jsr cursor_erase                ; restores saved char/color, sets drawn=0
@done:
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

    jsr metronome_tick
    jsr cursor_tick

    pla
    tay
    pla
    tax
    pla
    jmp XITVBV

_vera_vbi_end:


; ============================================================================
; metronome_tick — audible heartbeat on POKEY voice 4.
;   Driven by VERA_CTL_FLAG_METRONOME in _vera_ctl_block[VERACTL_FLAGS].
; ============================================================================

metronome_tick:
    lda _vera_ctl_block + VERACTL_FLAGS
    and #VERA_CTL_FLAG_METRONOME
    bne @on

    ; Flag is OFF — make sure the click isn't sustained.
    lda click_active
    beq @done
    lda #0
    sta AUDC4
    sta click_active
    lda #VBI_RATE
    sta frames_until_click
    rts

@on:
    ; If the previous frame emitted a click, silence it now.
    lda click_active
    beq @count
    lda #0
    sta AUDC4
    sta click_active
@count:
    dec frames_until_click
    bne @done
    lda #VBI_FREQ
    sta AUDF4
    lda #VBI_VOLUME
    sta AUDC4
    lda #VBI_RATE
    sta frames_until_click
    lda #1
    sta click_active
@done:
    rts


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
    dec cursor_frames
    beq @go
    rts
@go:
    lda #VBI_CURSOR_RATE
    sta cursor_frames

    ; Snapshot VERA state, then force ADDR0 selection for our work.
    lda VERA_CTRL
    sta vera_save_ctrl
    and #VERA_ADDRSEL_CLEAR
    sta VERA_CTRL
    lda VERA_ADDR_L
    sta vera_save_addr_l
    lda VERA_ADDR_M
    sta vera_save_addr_m
    lda VERA_ADDR_H
    sta vera_save_addr_h

    lda cursor_drawn
    bne @erase
    jsr cursor_draw
    jmp @restore
@erase:
    jsr cursor_erase

@restore:
    lda vera_save_addr_l
    sta VERA_ADDR_L
    lda vera_save_addr_m
    sta VERA_ADDR_M
    lda vera_save_addr_h
    sta VERA_ADDR_H
    lda vera_save_ctrl
    sta VERA_CTRL
    rts


; ============================================================================
; cursor_draw — at the position requested by _vera_ctl_block, save the
; underlying char + color, then write the char back with foreground and
; background swapped (classic inverted-block cursor).
;
; Address math (per-row stride = 256, MAP_COLS=128 × 2):
;   ADDR_L = X * 2
;   ADDR_M = $B0 + Y
;   ADDR_H = $11               (bank 1 | INC1)
;
; Reads via VERA_DATA0 advance the pointer by 1 each, so after reading
; char + color the pointer sits 2 bytes past the target cell. We must
; re-point ADDR before writing — otherwise the write lands on the next
; cell.
; ============================================================================

cursor_draw:
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp #SCREEN_COLS
    bcs @oob
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp #SCREEN_ROWS
    bcs @oob

    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta cursor_at_x
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta cursor_at_y

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
    adc cursor_at_y                 ; $B0 + Y (Y < 25 ⇒ no carry into bank)
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
