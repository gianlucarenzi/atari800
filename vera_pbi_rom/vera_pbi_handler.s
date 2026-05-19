; ============================================================================
; vera_pbi_handler.s - PBI Device Driver ROM for the VeraX16 FPGA card
;
; Installs a VERA (Video Enhanced Retro Adapter) video card as an Atari
; Parallel Bus Interface (PBI) device.  The ROM occupies $D800-$DFFF (2 KB)
; when the card is selected via the $D1FF PBI latch.
;
; Atari PBI ROM header at $D800 (Earl Rice / ANTIC Magazine format):
;   $D800-$D801  Checksum word (optional, 0 here)
;   $D802        Revision byte
;   $D803        PBI device ID mask  (power-of-2, 1-$80)
;   $D804        Device type / flags
;   $D805-$D807  JMP to low-level I/O handler
;   $D808-$D80A  JMP to interrupt handler
;   $D80B        Manufacturer code ($91 = Atari-compatible)
;   $D80C        CIO handler device name (ASCII, stored in HATABS)
;   $D80D-$D80E  OPEN  vector (handler address - 1)
;   $D80F-$D810  CLOSE vector
;   $D811-$D812  GET BYTE vector
;   $D813-$D814  PUT BYTE vector
;   $D815-$D816  GET STATUS vector
;   $D817-$D818  SPECIAL vector
;   $D819-$D81B  JMP to INIT handler (called at cold/warm start)
;   $D81C        Reserved ($00)
;
; VERA register base: $D100  (PBI_ADDR)
; VERA VRAM: 128 KB ($00000-$1FFFF), accessed via DATA0 with address ports
;
; (C) 2025-2026 RetroBit Lab
; Written by Gianluca Renzi <gianlucarenzi@eurek.it>
; ============================================================================

    .setcpu "6502"

    .include "vera_common.inc"
    .include "atari.inc"

; ============================================================================
; Zero Page scratchpad addresses
; ============================================================================

TMP_PTR_LO      = $CB
TMP_PTR_HI      = $CC
TMP0            = $CD
TMP1            = $CE
TMP2            = $CF

; ============================================================================
; VERA constants
; ============================================================================

VER_LINE_ADDR   = SCREEN_ADDR + (1 * MAP_COLS * 2) + (2 * 2)
HOST_LINE_ADDR  = SCREEN_ADDR + (3 * MAP_COLS * 2) + (2 * 2)

.macro PRINT_LINE addr, label
    .local CopyChar, Done
    lda #<(addr)
    sta VERA_ADDR_L
    lda #>(addr)
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^(addr))
    sta VERA_ADDR_H
    ldx #0
CopyChar:
    lda label,x
    beq Done
    sta VERA_DATA0
    lda #TEXT_COLOR
    sta VERA_DATA0
    inx
    bne CopyChar
Done:
.endmacro

    .segment "CODE"

    .word $0000
    .byte $00
    .byte DEVICE_ID_MASK
    .byte $00

    jmp IOVECTOR
    jmp IRQVECTOR

    .byte $91
    .byte 'V'

    .word NONEED-1
    .word NONEED-1
    .word NONEED-1
    .word NONEED-1
    .word NONEED-1
    .word NONEED-1

    jmp INIT
    .byte $00

IOVECTOR:
    clc
    rts

IRQVECTOR:
    lda #$00
    sta VERA_ISR
    rts

; Offsets within the 16-byte VCTL block (signature + ptr table).
VCTL_SIG0_OFF   = 0
VCTL_SIG1_OFF   = 1
VCTL_SIG2_OFF   = 2
VCTL_SIG3_OFF   = 3
VCTL_VBI_LO_OFF = 12
VCTL_VBI_HI_OFF = 13
VCTL_REINIT_LO_OFF = 14
VCTL_REINIT_HI_OFF = 15

; ZP scratch for the (MEMLO - 16) pointer.
VCTL_PTR = TMP_PTR_LO   ; reuses $80/$81

PBI_INIT:
INIT:
    lda #0
    sta CRITIC

    lda PDVMSK
    ora SHPDVS
    sta PDVMSK

    jsr INIT_VERA_SCREEN
    rts

PBI_INIT_VERA_SCREEN:
INIT_VERA_SCREEN:
    jsr WAIT_VERA

    lda #VERA_DCSEL0
    sta VERA_CTRL_REG
    lda #$00
    sta VERA_IEN
    sta VERA_ISR

    jsr LOAD_BOOT_FONT

    lda #VERA_MAP_128x64
    sta VERA_L1_CONFIG
    lda #SCREEN_MAPBASE
    sta VERA_L1_MAPBASE
    lda #SCREEN_TILEBASE
    sta VERA_L1_TILEBASE

    lda #$00
    sta VERA_L1_HSCR_L
    sta VERA_L1_HSCR_H
    sta VERA_L1_VSCR_L
    sta VERA_L1_VSCR_H

    lda #VERA_DCSEL1
    sta VERA_CTRL_REG
    lda #DC_HSTART_VAL
    sta VERA_DC_HSTART
    lda #DC_HSTOP_VAL
    sta VERA_DC_HSTOP
    lda #DC_VSTART_VAL
    sta VERA_DC_VSTART
    lda #DC_VSTOP_VAL
    sta VERA_DC_VSTOP

    lda #VERA_DCSEL0
    sta VERA_CTRL_REG
    lda #(VERA_VIDEO_VGA | VERA_LAYER1_EN)
    sta VERA_DC_VIDEO
    lda #$80
    sta VERA_DC_HSCALE
    lda #$80
    sta VERA_DC_VSCALE
    lda #$06
    sta VERA_DC_BORDER

    jsr CLEAR_SCREEN

    lda #$02
    sta VERA_ADDR_L
    lda #$FA
    sta VERA_ADDR_M
    lda #(VERA_INC1 | $01)
    sta VERA_ADDR_H
    lda #$FF
    sta VERA_DATA0
    lda #$0F
    sta VERA_DATA0

    jsr PRINT_VERSION_LINE
    jsr PRINT_HOST_LINE
    rts

WAIT_VERA:
    ldx #$FF
@Loop:
    lda #$2A
    sta VERA_ADDR_L
    lda VERA_ADDR_L
    cmp #$2A
    beq @Done
    dex
    bne @Loop
@Done:
    rts

PBI_CLEAR_SCREEN:
CLEAR_SCREEN:
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
    lda #TEXT_COLOR
    sta VERA_DATA0
    dex
    bne @Col
    dey
    bne @Row
    pla                         ; Restore ANTIC DMA state
    sta DMACTL
    rts

LOAD_BOOT_FONT:
    ; Load only the chars needed by the boot banner into VERA VRAM.
    ; Each entry in boot_font_data: CHAR_INDEX, d0..d7 (9 bytes).
    ; VRAM address is computed as CHARSET_ADDR + CHAR_INDEX * 8.
    ; VERA_ADDR_H ($D102) is set once — increment selector persists.
    lda #<boot_font_data
    sta TMP_PTR_LO
    lda #>boot_font_data
    sta TMP_PTR_HI
    lda #(VERA_INC1 | $01)
    sta VERA_ADDR_H
    ldx #BOOT_FONT_COUNT
@entry:
    ldy #0
    lda (TMP_PTR_LO),y         ; Get character index
    pha
    and #$1F                   ; ADDR_L = (Index & $1F) << 3
    asl a
    asl a
    asl a
    sta VERA_ADDR_L
    pla
    lsr a                      ; ADDR_M = $F0 | (Index >> 5)
    lsr a
    lsr a
    lsr a
    lsr a
    ora #$F0
    sta VERA_ADDR_M
    ldy #1                     ; Start of data
@tile:
    lda (TMP_PTR_LO),y
    sta VERA_DATA0             ; Write scanline once for 8x8
    iny
    cpy #9
    bne @tile
    lda TMP_PTR_LO
    clc
    adc #9
    sta TMP_PTR_LO
    bcc @next
    inc TMP_PTR_HI
@next:
    dex
    bne @entry
    rts

WRITE_CHAR:
    sta VERA_DATA0
    lda TMP0
    sta VERA_DATA0
    rts

PRINT_PTR:
    ldy #0
@Loop:
    lda (TMP_PTR_LO),y
    beq @Done
    jsr WRITE_CHAR
    iny
    bne @Loop
@Done:
    rts

PRINT_DECIMAL:
    ldy #0
@Hundreds:
    cmp #100
    bcc @Tens
    sec
    sbc #100
    iny
    bne @Hundreds
@Tens:
    ldx #0
@TenLoop:
    cmp #10
    bcc @Emit
    sec
    sbc #10
    inx
    bne @TenLoop
@Emit:
    sta TMP2
    tya
    beq @MaybeTens
    clc
    adc #'0'
    jsr WRITE_CHAR
@MaybeTens:
    cpx #0
    beq @Ones
    txa
    clc
    adc #'0'
    jsr WRITE_CHAR
@Ones:
    lda TMP2
    clc
    adc #'0'
    jmp WRITE_CHAR

PRINT_VERSION_LINE:
    lda #TEXT_COLOR
    sta TMP0
    lda LMARGN
    asl a
    sta VERA_ADDR_L
    lda #>(SCREEN_ADDR + (1 * MAP_COLS * 2))
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^(SCREEN_ADDR + (1 * MAP_COLS * 2)))
    sta VERA_ADDR_H
    lda #<VersionPrefix
    sta TMP_PTR_LO
    lda #>VersionPrefix
    sta TMP_PTR_HI
    jsr PRINT_PTR

    lda #$7E
    sta VERA_CTRL_REG
    lda VERA_DC_HSCALE
    jsr PRINT_DECIMAL
    lda #'.'
    jsr WRITE_CHAR
    lda VERA_DC_VSCALE
    jsr PRINT_DECIMAL
    lda #'.'
    jsr WRITE_CHAR
    lda VERA_DC_BORDER
    jsr PRINT_DECIMAL
    lda #$00
    sta VERA_CTRL_REG
    rts

PRINT_HOST_LINE:
    lda #TEXT_COLOR
    sta TMP0
    lda LMARGN
    asl a
    sta VERA_ADDR_L
    lda #>(SCREEN_ADDR + (3 * MAP_COLS * 2))
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^(SCREEN_ADDR + (3 * MAP_COLS * 2)))
    sta VERA_ADDR_H
    lda RAMTOP
    cmp #$80
    bcs HostMaybeXE
    lda #<Host600XL
    sta TMP_PTR_LO
    lda #>Host600XL
    sta TMP_PTR_HI
    lda #TEXT_COLOR
    sta TMP0
    jmp PRINT_PTR
HostMaybeXE:
    jsr HAS_XE_BANK
    bcc HostNoXE
    lda #<Host130XE
    sta TMP_PTR_LO
    lda #>Host130XE
    sta TMP_PTR_HI
    lda #TEXT_COLOR
    sta TMP0
    jmp PRINT_PTR
HostNoXE:
    lda #<Host800XL
    sta TMP_PTR_LO
    lda #>Host800XL
    sta TMP_PTR_HI
    lda #TEXT_COLOR
    sta TMP0
    jmp PRINT_PTR

HAS_XE_BANK:
    php
    sei
    lda PORTB
    sta TMP0
    lda $4000
    sta TMP1
    lda #$5A
    sta $4000
    lda TMP0
    and #$C3
    ora #$20
    sta PORTB
    lda $4000
    sta TMP2
    lda #$A5
    sta $4000
    lda TMP0
    sta PORTB
    lda $4000
    cmp #$5A
    bne @No
    lda TMP1
    sta $4000
    lda TMP0
    and #$C3
    ora #$20
    sta PORTB
    lda TMP2
    sta $4000
    lda TMP0
    sta PORTB
    plp
    sec
    rts
@No:
    lda TMP1
    sta $4000
    lda TMP0
    sta PORTB
    plp
    clc
    rts

NONEED:
    lda #$00
    sta CRITIC
    ldy #1
    sec
    sta PBI_LATCH ; $D1FF -> 0 Reenable the Math Pack when exiting
    rts

VersionPrefix:
    .asciiz "VERA MODULE FW:"
Host600XL:
    .asciiz "ATARI 600XL"
Host800XL:
    .asciiz "ATARI 800XL"
Host130XE:
    .asciiz "ATARI 130XE"

; Boot font: 27 chars x 9 bytes = 243 bytes.
; Only the characters actually used by the boot banner text.
; vera_sys will overwrite these with the full 1KB font.
; Format per entry: CHAR_INDEX, d0..d7
BOOT_FONT_COUNT = 27

boot_font_data:
    .byte $20, $00,$00,$00,$00,$00,$00,$00,$00  ; ' '
    .byte $2E, $00,$00,$00,$00,$00,$18,$18,$00  ; '.'
    .byte $30, $3C,$66,$6E,$76,$66,$66,$3C,$00  ; '0'
    .byte $31, $18,$18,$38,$18,$18,$18,$7E,$00  ; '1'
    .byte $32, $3C,$66,$06,$0C,$30,$60,$7E,$00  ; '2'
    .byte $33, $3C,$66,$06,$1C,$06,$66,$3C,$00  ; '3'
    .byte $34, $06,$0E,$1E,$66,$7F,$06,$06,$00  ; '4'
    .byte $35, $7E,$60,$7C,$06,$06,$66,$3C,$00  ; '5'
    .byte $36, $3C,$66,$60,$7C,$66,$66,$3C,$00  ; '6'
    .byte $37, $7E,$66,$0C,$18,$18,$18,$18,$00  ; '7'
    .byte $38, $3C,$66,$66,$3C,$66,$66,$3C,$00  ; '8'
    .byte $39, $3C,$66,$66,$3E,$06,$66,$3C,$00  ; '9'
    .byte $3A, $00,$00,$18,$00,$00,$18,$00,$00  ; ':'
    .byte $41, $18,$3C,$66,$7E,$66,$66,$66,$00  ; 'A'
    .byte $44, $78,$6C,$66,$66,$66,$6C,$78,$00  ; 'D'
    .byte $45, $7E,$60,$60,$78,$60,$60,$7E,$00  ; 'E'
    .byte $46, $7E,$60,$60,$78,$60,$60,$60,$00  ; 'F'
    .byte $49, $3C,$18,$18,$18,$18,$18,$3C,$00  ; 'I'
    .byte $4C, $60,$60,$60,$60,$60,$60,$7E,$00  ; 'L'
    .byte $4D, $63,$77,$7F,$6B,$63,$63,$63,$00  ; 'M'
    .byte $4F, $3C,$66,$66,$66,$66,$66,$3C,$00  ; 'O'
    .byte $52, $7C,$66,$66,$7C,$78,$6C,$66,$00  ; 'R'
    .byte $54, $7E,$18,$18,$18,$18,$18,$18,$00  ; 'T'
    .byte $55, $66,$66,$66,$66,$66,$66,$3C,$00  ; 'U'
    .byte $56, $66,$66,$66,$66,$66,$3C,$18,$00  ; 'V'
    .byte $57, $63,$63,$63,$6B,$7F,$77,$63,$00  ; 'W'
    .byte $58, $66,$66,$3C,$18,$3C,$66,$66,$00  ; 'X'
