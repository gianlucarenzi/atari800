    .setcpu "6502"

    .export _InitVbi, _vera_vbi_end, _vbi_handler
    .export _vera_save_c_sp, _vera_warm_start
    .import _VeraApiService, _vera_ctl_block, _vera_warm_reinit

    .include "atari.inc"

; VERA registers
VERA_ADDR_L = $D100
VERA_ADDR_M = $D101
VERA_ADDR_H = $D102
VERA_DATA0  = $D103
VERA_CTRL   = $D105

; VeraCtl offsets
VERACTL_FLAGS    = 4
VERACTL_CURSOR_X = 8
VERACTL_CURSOR_Y = 9
VERA_CTL_FLAG_METRONOME = $01
VERA_CTL_FLAG_API_READY = $80

VBI_RATE        = 10
VBI_FREQ        = $08
VBI_VOLUME      = $AF
VBI_CURSOR_RATE = 20

VERA_SCREEN_BASE_M = $B0
VERA_SCREEN_BANK   = $11
VERA_TEXT_COLOR    = $61
SETVBV = $E45C

    .segment "LOWBSS"
frames_until_click: .res 1
click_active:       .res 1
cursor_frames:      .res 1
cursor_phase:       .res 1
cursor_drawn:       .res 1
cursor_draw_x:      .res 1
cursor_draw_y:      .res 1
cursor_saved_char:  .res 1
cursor_saved_color: .res 1
vera_save_addr_l:   .res 1
vera_save_addr_m:   .res 1
vera_save_addr_h:   .res 1
vera_save_ctrl:     .res 1

    .segment "CODE"

_InitVbi:
    sei
    ldy #<_vbi_handler
    ldx #>_vbi_handler
    lda #7
    jsr SETVBV
    lda #VBI_RATE
    sta frames_until_click
    lda #0
    sta click_active
    lda #VBI_CURSOR_RATE
    sta cursor_frames
    lda #0
    sta cursor_phase
    sta cursor_drawn
    cli
    rts

; Compatibility shim kept while external callers are migrated off the old cc65 path.
_vera_save_c_sp:
    rts

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

_vbi_handler:
    lda CRITIC
    beq @vbi_ok
    jmp XITVBV

@vbi_ok:
    pha
    txa
    pha
    tya
    pha

    ; --- Metronome Logic ---
    lda _vera_ctl_block + VERACTL_FLAGS
    and #VERA_CTL_FLAG_METRONOME
    bne @metronome_on

    ; Metronome OFF
    lda click_active
    beq @metronome_done
    lda #0
    sta AUDC4
    sta click_active
    lda #VBI_RATE
    sta frames_until_click
    jmp @metronome_done

@metronome_on:
    lda click_active
    beq @check_timer
    lda #0
    sta AUDC4
    sta click_active
@check_timer:
    dec frames_until_click
    bne @metronome_done
    lda #VBI_FREQ
    sta AUDF4
    lda #VBI_VOLUME
    sta AUDC4
    lda #VBI_RATE
    sta frames_until_click
    lda #1
    sta click_active

@metronome_done:
    ; --- Cursor Logic ---
    dec cursor_frames
    beq @do_cursor_toggle
    jmp @vbi_done

@do_cursor_toggle:
    lda #VBI_CURSOR_RATE
    sta cursor_frames
    jmp @cursor_toggle_real

@vbi_done:
    pla
    tay
    pla
    tax
    pla
    jmp XITVBV

@cursor_toggle_real:
    ; Save VERA state
    lda VERA_ADDR_L
    sta vera_save_addr_l
    lda VERA_ADDR_M
    sta vera_save_addr_m
    lda VERA_ADDR_H
    sta vera_save_addr_h
    lda VERA_CTRL
    sta vera_save_ctrl

    ; Select ADDR0
    lda vera_save_ctrl
    and #$FE
    sta VERA_CTRL

    ; Set VERA address to cursor position: 
    ; Address = (Y * 160) + (X * 2)
    
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp #25 ; Standard screen rows
    bcc @calc_row
    jmp @cursor_done
    
@calc_row:
    ; Calculate row offset: Y * 160
    tay
    asl
    asl
    asl
    asl
    asl ; Y * 32
    sta VERA_ADDR_L ; Temporary
    tya
    asl
    asl
    asl
    asl
    asl
    asl
    asl ; Y * 128
    clc
    adc VERA_ADDR_L
    sta VERA_ADDR_L
    tya
    rol
    rol
    and #$01
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    
    ; Add column offset: X * 2
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl
    clc
    adc VERA_ADDR_L
    sta VERA_ADDR_L
    lda VERA_ADDR_M
    adc #0
    sta VERA_ADDR_M
    
    lda #($10 | VERA_SCREEN_BANK) ; Increment 1
    sta VERA_ADDR_H

    ; Toggle phase
    lda cursor_phase
    eor #$01
    sta cursor_phase
    bne @cursor_on

@cursor_off:
    ; Standard color
    lda #' '
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    jmp @cursor_done

@cursor_on:
    ; Inverted/Blink color
    lda #' '
    sta VERA_DATA0
    lda #$66
    sta VERA_DATA0

@cursor_done:
    ; Restore VERA state
    lda vera_save_addr_l
    sta VERA_ADDR_L
    lda vera_save_addr_m
    sta VERA_ADDR_M
    lda vera_save_addr_h
    sta VERA_ADDR_H
    lda vera_save_ctrl
    sta VERA_CTRL
    jmp @vbi_done

_CallVeraApiService:
    pha
    txa
    pha
    tya
    pha
    jsr _VeraApiService
    pla
    tay
    pla
    tax
    pla
    rts

_vera_vbi_end:
