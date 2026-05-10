; VERA setup helpers for the Atari XL PBI board.

BITMAP_WIDTH = 320
BITMAP_HEIGHT = 240
TILE_MAP_WIDTH = 128
TILE_MAP_HEIGHT = 64

setup_vera_for_bitmap_and_tile_map:
    ldx #$A5
wait_for_vera:
    lda #42
    sta VERA_ADDR_LOW

    lda VERA_ADDR_LOW
    cmp #42
    beq vera_ready

    ldy #0
vera_boot_snooze:
    nop
    nop
    nop
    nop
    nop
    nop
    iny
    bne vera_boot_snooze

    dex
    bne wait_for_vera

vera_ready:
    lda VERA_DC_VIDEO
    and #%00000011
    bne output_mode_is_set

    lda VERA_DC_VIDEO
    ora #%00000001
    sta VERA_DC_VIDEO

output_mode_is_set:
    lda VERA_DC_VIDEO
    ora #%01110000
    sta VERA_DC_VIDEO

    lda #0
    sta VERA_L0_HSCROLL_L
    sta VERA_L0_HSCROLL_H
    sta VERA_L0_VSCROLL_L
    sta VERA_L0_VSCROLL_H
    sta VERA_L1_HSCROLL_L
    sta VERA_L1_HSCROLL_H
    sta VERA_L1_VSCROLL_L
    sta VERA_L1_VSCROLL_H

    lda #$40
    sta VERA_DC_HSCALE
    sta VERA_DC_VSCALE

    lda #%00000000
    sta VERA_CTRL

    lda #(4+3)
    sta VERA_L0_CONFIG

    lda #($000 >> 1)
    sta VERA_L0_TILEBASE

    lda #%10100000
    sta VERA_L1_CONFIG

    lda #($1B0 >> 1)
    sta VERA_L1_MAPBASE

    lda #($1F0 >> 1)
    sta VERA_L1_TILEBASE

    rts

copy_petscii_charset:
    rts

clear_tilemap_screen:
    lda #%00010001
    sta VERA_ADDR_BANK
    lda #$B0
    sta VERA_ADDR_HIGH
    lda #$00
    sta VERA_ADDR_LOW

    ldy #(TILE_MAP_HEIGHT / (256 / TILE_MAP_WIDTH))
vera_clear_fill_tile_map:
    ldx #0
vera_clear_fill_tile_map_row:
    lda #$20
    sta VERA_DATA0
    lda #COLOR_TRANSPARANT
    sta VERA_DATA0
    inx
    bne vera_clear_fill_tile_map_row
    dey
    bne vera_clear_fill_tile_map

    rts

init_cursor:
    lda #LEFT_MARGIN
    sta INDENTATION
    sta CURSOR_X
    lda #TOP_MARGIN
    sta CURSOR_Y
    rts
