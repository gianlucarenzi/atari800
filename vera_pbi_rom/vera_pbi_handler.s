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
; CIO interface:
;   GET BYTE: reads  VERA register at index given in ICAX1 of the IOCB
;   PUT BYTE: writes VERA register at index given in ICAX1 of the IOCB
;   GET STATUS: returns VERA_CTRL register value
;   SPECIAL:
;     XIO 32,"V:" enables the resident VERA metronome
;     XIO 33,"V:" disables it again
;     XIO 34,"V:" clears the resident VERA text surface
;     XIO 35,"V:" draws a resident VERA backend demo page
;     XIO 36,"V:" prints one ATASCII character from ICAX1
;     XIO 37,"V:" sets the resident cursor to ICAX1/ICAX2
;
; VERA register base: $D100  (PBI_ADDR)
; VERA VRAM: 128 KB ($00000-$1FFFF), accessed via DATA0 with address ports
;
; (C) 2025-2026 RetroBit Lab
; Written by Gianluca Renzi <gianlucarenzi@eurek.it>
; ============================================================================

    .setcpu "6502"

; ============================================================================
; OS equates
; ============================================================================

PDVMSK  = $0247         ; PBI device mask     (enabled-device bitmask)
PNDEVREQ = $0248        ; PBI device request  (this device's bit, set by OS)
PDIMSK  = $0249         ; PBI interrupt mask

NEWDEV  = $E486         ; Install device handler in HATABS
GENDEV  = $E48F         ; Generic CIO device handler vector

ICCOM   = $0342         ; IOCB command byte
ICAX1   = $034A         ; Auxiliary byte 1 (used here as register index)
ICAX2   = $034B         ; Auxiliary byte 2
CRITIC  = $42           ; Critical section flag (0 = deferred VBI enabled)

; Shared XIO/VBI control block. It lives at the beginning of the resident
; LOWBSS area in VERA.SYS, which is fixed at $8000 by vera_sys.cfg.
VERA_CTL_BASE = $8000

CIOStatNotSupported = $92

; ============================================================================
; VERA hardware register base and register names
; ============================================================================

PBI_ADDR        = $D100

VERA_ADDR_L     = PBI_ADDR + $00    ; VRAM address bits  7:0  (active port)
VERA_ADDR_M     = PBI_ADDR + $01    ; VRAM address bits 15:8
VERA_ADDR_H     = PBI_ADDR + $02    ; bit[0]=A16  bits[7:4]=INCR
VERA_DATA0      = PBI_ADDR + $03    ; VRAM data port 0
VERA_DATA1      = PBI_ADDR + $04    ; VRAM data port 1
VERA_CTRL_REG   = PBI_ADDR + $05    ; CTRL: ADDRSEL(0) DCSEL(1) RESET(7)
VERA_IEN        = PBI_ADDR + $06    ; Interrupt enable
VERA_ISR        = PBI_ADDR + $07    ; Interrupt status (write 1 to clear)

VERA_DC_VIDEO   = PBI_ADDR + $09    ; Output enable, layer enable, sprites
VERA_DC_HSCALE  = PBI_ADDR + $0A    ; Horizontal scale (128 = 1:1)
VERA_DC_VSCALE  = PBI_ADDR + $0B    ; Vertical scale
VERA_DC_BORDER  = PBI_ADDR + $0C    ; Border colour index

VERA_DC_HSTART  = PBI_ADDR + $09    ; Active area start column (/4)
VERA_DC_HSTOP   = PBI_ADDR + $0A    ; Active area stop  column (/4)
VERA_DC_VSTART  = PBI_ADDR + $0B    ; Active area start row    (/2)
VERA_DC_VSTOP   = PBI_ADDR + $0C    ; Active area stop  row    (/2)

VERA_L1_CONFIG  = PBI_ADDR + $14
VERA_L1_MAPBASE = PBI_ADDR + $15
VERA_L1_TILEBASE= PBI_ADDR + $16
VERA_L1_HSCR_L  = PBI_ADDR + $17
VERA_L1_HSCR_H  = PBI_ADDR + $18
VERA_L1_VSCR_L  = PBI_ADDR + $19
VERA_L1_VSCR_H  = PBI_ADDR + $1A

VERA_REG_ARRAY  = PBI_ADDR + $00

; ============================================================================
; VERA constants
; ============================================================================

DEVICE_ID_MASK  = $80           ; This card occupies PBI bit 7
DEVNAM          = 'V'           ; CIO device name registered in HATABS

VERA_INC0       = $00           ; No auto-increment
VERA_INC1       = $10           ; Auto-increment by 1

VERA_DCSEL0     = $00           ; Access DC_VIDEO/HSCALE/VSCALE/BORDER bank
VERA_DCSEL1     = $02           ; Access DC_HSTART/HSTOP/VSTART/VSTOP bank

VERA_VIDEO_VGA  = $01           ; VGA output (640x480)
VERA_LAYER1_EN  = $20           ; Enable Layer 1

VERA_MAP_128x64 = $60           ; 128-tile wide, 64-tile tall map

SCREEN_ADDR     = $01B000       ; Tilemap start (128x64 = 8 KB, in bank 1)
CHARSET_ADDR    = $01F000       ; Character glyphs (256 chars x 16 bytes)

SCREEN_MAPBASE  = $D8           ; L1_MAPBASE  = SCREEN_ADDR >> 9
SCREEN_TILEBASE = $FA           ; L1_TILEBASE = CHARSET_ADDR >> 9 | height16

SCREEN_COLS     = 80
MAP_COLS        = 128
SCREEN_ROWS     = 25
TEXT_COLOR      = $61           ; White on blue

VERA_CTL_SIG0       = VERA_CTL_BASE + 0
VERA_CTL_SIG1       = VERA_CTL_BASE + 1
VERA_CTL_SIG2       = VERA_CTL_BASE + 2
VERA_CTL_SIG3       = VERA_CTL_BASE + 3
VERA_CTL_FLAGS      = VERA_CTL_BASE + 4
VERA_CTL_REQUEST    = VERA_CTL_BASE + 5
VERA_CTL_PARAM0     = VERA_CTL_BASE + 6
VERA_CTL_PARAM1     = VERA_CTL_BASE + 7
VERA_CTL_CURSOR_X   = VERA_CTL_BASE + 8
VERA_CTL_CURSOR_Y   = VERA_CTL_BASE + 9
VERA_CTL_ENTRY_LO   = VERA_CTL_BASE + 10
VERA_CTL_ENTRY_HI   = VERA_CTL_BASE + 11

VERA_CTL_FLAG_METRONOME = $01
VERA_CTL_FLAG_API_READY = $80

XIO_VERA_ENABLE  = 32
XIO_VERA_DISABLE = 33
XIO_VERA_CLEAR   = 34
XIO_VERA_DEMO    = 35
XIO_VERA_PUTC    = 36
XIO_VERA_CURSOR  = 37

VERA_REQ_NONE    = $00
VERA_REQ_CLEAR   = $01
VERA_REQ_DEMO    = $02
VERA_REQ_PUTC    = $03

DC_HSTART_VAL   = $00
DC_HSTOP_VAL    = $A0
DC_VSTART_VAL   = $14
DC_VSTOP_VAL    = $DC

BANNER1_ADDR    = SCREEN_ADDR + (9  * MAP_COLS * 2) + (26 * 2)
BANNER2_ADDR    = SCREEN_ADDR + (12 * MAP_COLS * 2) + (30 * 2)
BANNER3_ADDR    = SCREEN_ADDR + (15 * MAP_COLS * 2) + (37 * 2)

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
    .byte DEVNAM

    .word NONEED-1
    .word NONEED-1
    .word GETBYT-1
    .word PUTBYT-1
    .word GETSTA-1
    .word SPECIAL-1

    jmp INIT
    .byte $00

IOVECTOR:
    clc
    rts

IRQVECTOR:
    rts

INIT:
    lda PDVMSK
    ora PNDEVREQ
    sta PDVMSK

    ldx #DEVNAM
    lda #>GENDEV
    ldy #<GENDEV
    jsr NEWDEV

    jsr INIT_VERA_SCREEN
    rts

INIT_VERA_SCREEN:
    jsr WAIT_VERA

    lda #VERA_DCSEL0
    sta VERA_CTRL_REG
    lda #$00
    sta VERA_IEN
    sta VERA_ISR

    jsr LOAD_FULL_FONT

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
    lda #$00
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

    PRINT_LINE BANNER1_ADDR, BannerLine1
    PRINT_LINE BANNER2_ADDR, BannerLine2
    PRINT_LINE BANNER3_ADDR, BannerLine3
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

CLEAR_SCREEN:
    lda #<SCREEN_ADDR
    sta VERA_ADDR_L
    lda #>SCREEN_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^SCREEN_ADDR)
    sta VERA_ADDR_H
    ldy #SCREEN_ROWS
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
    rts

LOAD_FULL_FONT:
    lda #$00
    sta VERA_ADDR_L
    lda #$F0
    sta VERA_ADDR_M
    lda #(VERA_INC1 | $01)
    sta VERA_ADDR_H

    ldx #0
@NextChar:
    txa
    cmp #32
    bcc @ToControl
    cmp #96
    bcc @ToUpper
    tay
    jmp @SetSource

@ToControl:
    clc
    adc #64
    tay
    jmp @SetSource

@ToUpper:
    sec
    sbc #32
    tay

@SetSource:
    lda #0
    sta $81

    tya
    asl
    rol $81
    asl
    rol $81
    asl
    rol $81

    clc
    adc #<FontData
    sta $80
    lda $81
    adc #>FontData
    sta $81

    ldy #0
@CopyRows:
    lda ($80),y
    sta VERA_DATA0
    sta VERA_DATA0
    iny
    cpy #8
    bne @CopyRows

    inx
    cpx #128
    bne @NextChar
    rts

GETBYT:
    lda #$00
    sta CRITIC
    ldy ICAX1,x
    lda VERA_REG_ARRAY,y
    ldy #1
    sec
    rts

PUTBYT:
    pha
    lda #$00
    sta CRITIC
    ldy ICAX1,x
    pla
    sta VERA_REG_ARRAY,y
    ldy #1
    sec
    rts

GETSTA:
    lda #$00
    sta CRITIC
    lda VERA_CTRL_REG
    ldy #1
    sec
    rts

SPECIAL:
    lda #$00
    sta CRITIC
    lda VERA_CTL_SIG0
    cmp #'V'
    bne @NotSupported
    lda VERA_CTL_SIG1
    cmp #'C'
    bne @NotSupported
    lda VERA_CTL_SIG2
    cmp #'T'
    bne @NotSupported
    lda VERA_CTL_SIG3
    cmp #'L'
    bne @NotSupported

    lda ICCOM,x
    cmp #XIO_VERA_ENABLE
    beq @Enable
    cmp #XIO_VERA_DISABLE
    beq @Disable
    cmp #XIO_VERA_CLEAR
    beq @Clear
    cmp #XIO_VERA_DEMO
    beq @Demo
    cmp #XIO_VERA_PUTC
    beq @PutChar
    cmp #XIO_VERA_CURSOR
    beq @SetCursor

@NotSupported:
    lda #$00
    sta CRITIC
    ldy #CIOStatNotSupported
    rts

@Enable:
    lda VERA_CTL_FLAGS
    ora #VERA_CTL_FLAG_METRONOME
    sta VERA_CTL_FLAGS
    jmp NONEED

@Disable:
    lda VERA_CTL_FLAGS
    and #$FE
    sta VERA_CTL_FLAGS
    jmp NONEED

@Clear:
    lda #VERA_REQ_CLEAR
    sta VERA_CTL_REQUEST
    jsr CALL_SERVICE
    jmp NONEED

@Demo:
    lda #VERA_REQ_DEMO
    sta VERA_CTL_REQUEST
    jsr CALL_SERVICE
    jmp NONEED

@PutChar:
    lda ICAX1,x
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr CALL_SERVICE
    jmp NONEED

@SetCursor:
    lda ICAX1,x
    sta VERA_CTL_CURSOR_X
    lda ICAX2,x
    sta VERA_CTL_CURSOR_Y
    jmp NONEED

CALL_SERVICE:
    lda #$01
    sta CRITIC
    lda #>(@Return-1)
    pha
    lda #<(@Return-1)
    pha
    jmp (VERA_CTL_ENTRY_LO)
@Return:
    rts

NONEED:
    lda #$00
    sta CRITIC
    ldy #1
    sec
    rts

BannerLine1:
    .asciiz "**** COMMANDER X16 VERA ****"

BannerLine2:
    .asciiz "PBI VIDEO INTERFACE"

BannerLine3:
    .asciiz "READY."

FontData:
    .incbin "atari_font.bin"

; End of ROM
