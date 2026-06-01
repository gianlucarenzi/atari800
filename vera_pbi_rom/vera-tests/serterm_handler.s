; ============================================================================
; SERTERM_HANDLER.S - Low-level SIO & VERA Direct Hardware Renderer
; ============================================================================
; Optimized for stability and direct VERA 80x30 hardware access.
; ============================================================================

.include "atari.inc"
.include "vera_common.inc"

.import _trip
.import _vera_x16_font

.export _siov, _ih
.export _v_init, _v_putc, _v_cls

.segment "DATA"
v_cursor_x: .byte 0
v_cursor_y: .byte 0

.segment "CODE"

_siov:
    jsr SIOV
    rts

_ih:
    lda $D300               ; Clear PIA flag
    lda #$01
    sta _trip
    pla
    rti

; ----------------------------------------------------------------------------
; _v_init: Full VERA 80x30 configuration
; ----------------------------------------------------------------------------
_v_init:
    lda #0
    sta v_cursor_x
    sta v_cursor_y

    ; 1. Configure Display Composer Bank 1 (Active Area Clipping)
    lda #VERA_DCSEL1
    sta VERA_CTRL
    lda #$00                ; HSTART = 0
    sta VERA_DC_HSTART
    lda #$A0                ; HSTOP = 640 (/4)
    sta VERA_DC_HSTOP
    lda #$00                ; VSTART = 0
    sta VERA_DC_VSTART
    lda #$F0                ; VSTOP = 480 (/2)
    sta VERA_DC_VSTOP

    ; 2. Configure Display Composer Bank 0 (Output Mode)
    lda #VERA_DCSEL0
    sta VERA_CTRL
    lda #(VERA_VIDEO_VGA | VERA_LAYER1_EN)
    sta VERA_DC_VIDEO
    lda #$80                ; 1:1 scale
    sta VERA_DC_HSCALE
    sta VERA_DC_VSCALE
    lda #$00
    sta VERA_DC_BORDER

    ; 3. Configure Layer 1 (128x64 map, 16px tiles)
    lda #VERA_MAP_128x64
    sta VERA_L1_CONFIG
    lda #SCREEN_MAPBASE
    sta VERA_L1_MAPBASE
    lda #(SCREEN_TILEBASE | 2) ; 16px tile height
    sta VERA_L1_TILEBASE
    
    jsr v_load_font
    jsr _v_cls
    rts

v_load_font:
    lda #$00
    sta VERA_CTRL
    lda #CHARSET_VRAM_L
    sta VERA_ADDR_L
    lda #CHARSET_VRAM_M
    sta VERA_ADDR_M
    lda #CHARSET_VRAM_H
    sta VERA_ADDR_H

    ; Use a pointer in Zero Page for font source
    lda #<_vera_x16_font
    sta $CB
    lda #>_vera_x16_font
    sta $CC

    ldx #8                  ; 8 pages = 2048 bytes
    ldy #0
@l:
    lda ($CB),y
    sta VERA_DATA0
    iny
    bne @l
    inc $CC                 ; Next page
    dex
    bne @l
    rts

_v_cls:
    lda #$00
    sta VERA_CTRL
    lda #<SCREEN_ADDR
    sta VERA_ADDR_L
    lda #>SCREEN_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^SCREEN_ADDR)
    sta VERA_ADDR_H

    ldx #64                 ; Clear all 64 rows of the map
@row:
    ldy #128                ; Entire map width
@col:
    lda #' '
    sta VERA_DATA0
    lda #$61                ; Blue on White
    sta VERA_DATA0
    dey
    bne @col
    dex
    bne @row
    
    lda #0
    sta v_cursor_x
    sta v_cursor_y
    rts

_v_putc:
    cmp #155
    beq @nl
    cmp #13
    beq @nl
    cmp #10
    beq @skip

    pha
    jsr @set_addr
    pla
    sta VERA_DATA0
    lda #$61
    sta VERA_DATA0

    inc v_cursor_x
    lda v_cursor_x
    cmp #80
    bcc @done
    
@nl:
    lda #0
    sta v_cursor_x
    inc v_cursor_y
    lda v_cursor_y
    cmp #30
    bcc @done
    
    jsr v_scroll
    lda #29
    sta v_cursor_y
@done:
@skip:
    rts

@set_addr:
    lda #$00
    sta VERA_CTRL
    lda v_cursor_x
    asl a
    sta VERA_ADDR_L
    lda v_cursor_y
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    rts

v_scroll:
    lda DMACTL
    pha
    lda #0
    sta DMACTL

    ldx #0
@rl:
    lda #$01
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    txa
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    txa
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ldy #160                ; 80 chars * 2
@c:
    lda VERA_DATA0
    sta VERA_DATA1
    dey
    bne @c

    inx
    cpx #29
    bne @rl

    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda #(VERA_SCREEN_BASE_M + 29)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldy #80
@cl:
    lda #' '
    sta VERA_DATA0
    lda #$61
    sta VERA_DATA0
    dey
    bne @cl

    pla
    sta DMACTL
    rts
