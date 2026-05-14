; vera_driver.s - Fully assembly-based VERA PBI driver.

    .setcpu "6502"

    .export _vera_warm_reinit, _CallVeraApiService, _VeraApiService
    .import _vera_x16_font

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
VERA_INC1          = $10
CHARSET_VRAM_L     = $00
CHARSET_VRAM_M     = $F0
CHARSET_VRAM_H     = $11

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
    jsr vera_load_font
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
