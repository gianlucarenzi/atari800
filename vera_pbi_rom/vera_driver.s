; vera_driver.s - Fully assembly-based VERA PBI driver.

    .setcpu "6502"

    .export _vera_warm_reinit, _CallVeraApiService, _VeraApiService, vera_saved_zp, os_saved_zp

CC65_ZP_SIZE = $2A

    .segment "LOWBSS"
vera_saved_zp:      .res CC65_ZP_SIZE
os_saved_zp:        .res CC65_ZP_SIZE

    .segment "CODE"

_VeraApiService:
    rts
VERA_ADDR_L = $D100
VERA_ADDR_M = $D101
VERA_ADDR_H = $D102
VERA_DATA0  = $D103
VERA_CTRL   = $D105
VERA_DC_VIDEO = $D109
VERA_SCREEN_BASE_M = $B0
VERA_SCREEN_BANK   = $11
VERA_TEXT_COLOR    = $61

; POKEY
AUDF4       = $D206
AUDC4       = $D207

; Zero Page
CRITIC      = $42
SETVBV      = $E45C
XITVBV      = $E462

    .segment "DATA"
; Driver State (resident in RAM)
ctl_cursor_x: .byte 0
ctl_cursor_y: .byte 10
ctl_request:  .byte 0

    .segment "CODE"

; --- Warm Reinit ---
_vera_warm_reinit:
    lda #0
    sta $D100
    lda #(VERA_SCREEN_BASE_M + 8)
    sta $D101
    lda #$11
    sta $D102
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
    rts

ReadyText:
    .asciiz "DEVICE HANDLER READY"

; --- API Service ---
_CallVeraApiService:
    lda ctl_request
    cmp #3 ; VERA_REQ_PUTC
    beq _VeraPutByte
    rts

; --- Put Byte Handler ---
_VeraPutByte:
    ; (Simplified: logic to write to VERA)
    rts

; --- VBI Handler ---
_vbi_handler:
    lda CRITIC
    beq @ok
    jmp XITVBV
@ok:
    ; ... (Metronome/Cursor logic here) ...
    jmp XITVBV
