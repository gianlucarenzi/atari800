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

; RAM gate: CIO dispatch table + stubs copied to page 6 by INIT.
; Layout (75 bytes total):
;   +0   dispatch table  12 bytes (6 × addr-1 word)
;   +12  stub_noneed      8 bytes  OPEN/CLOSE: inline result, no ROM map
;   +20  epilogue         7 bytes  shared: unmap ROM / CLI / RTS
;   +27  stub_get        12 bytes  SEI/map/JSR GETBYT /JMP epilogue
;   +39  stub_put        12 bytes  SEI/map/JSR PUTBYT /JMP epilogue
;   +51  stub_status     12 bytes  SEI/map/JSR GETSTA /JMP epilogue
;   +63  stub_special    12 bytes  SEI/map/JSR SPECIAL/JMP epilogue
GATE_RAM          = $0600
GATE_STUB_NONEED  = GATE_RAM + 12
GATE_EPILOGUE     = GATE_RAM + 20
GATE_STUB_GET     = GATE_RAM + 27
GATE_STUB_PUT     = GATE_RAM + 39
GATE_STUB_STATUS  = GATE_RAM + 51
GATE_STUB_SPECIAL = GATE_RAM + 63

ICCOM   = $0342         ; IOCB command byte
ICAX1   = $034A         ; Auxiliary byte 1 (used here as register index)
ICAX2   = $034B         ; Auxiliary byte 2
CRITIC  = $42           ; Critical section flag (0 = deferred VBI enabled)
RAMTOP  = $6A
PORTB   = $D301

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
CHARSET_ADDR    = $01F000       ; Character glyphs (256 chars x 8 bytes)

SCREEN_MAPBASE  = $D8           ; L1_MAPBASE  = SCREEN_ADDR >> 9
SCREEN_TILEBASE = $F8           ; L1_TILEBASE = CHARSET_ADDR >> 9, 8x8 tiles

SCREEN_COLS     = 80
MAP_COLS        = 128
MAP_ROWS        = 64
SCREEN_ROWS     = 25
TEXT_COLOR      = $61           ; White on blue

TMP_PTR_LO      = $80
TMP_PTR_HI      = $81
TMP0            = $82
TMP1            = $83
TMP2            = $84

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
XIO_VERA_HOOKS   = 38

VERA_REQ_NONE    = $00
VERA_REQ_CLEAR   = $01
VERA_REQ_DEMO    = $02
VERA_REQ_PUTC    = $03
VERA_REQ_HOOKS   = $04
VERA_REQ_CURSOR  = $05

DC_HSTART_VAL   = $00
DC_HSTOP_VAL    = $A0
DC_VSTART_VAL   = $00
DC_VSTOP_VAL    = $F0

LOGO1_ADDR      = SCREEN_ADDR + (0 * MAP_COLS * 2) + (0 * 2)
LOGO2_ADDR      = SCREEN_ADDR + (1 * MAP_COLS * 2) + (0 * 2)
LOGO3_ADDR      = SCREEN_ADDR + (2 * MAP_COLS * 2) + (0 * 2)
LOGO4_ADDR      = SCREEN_ADDR + (3 * MAP_COLS * 2) + (0 * 2)
LOGO5_ADDR      = SCREEN_ADDR + (4 * MAP_COLS * 2) + (0 * 2)
LOGO6_ADDR      = SCREEN_ADDR + (5 * MAP_COLS * 2) + (0 * 2)
LOGO7_ADDR      = SCREEN_ADDR + (6 * MAP_COLS * 2) + (0 * 2)
VER_LINE_ADDR   = SCREEN_ADDR + (1 * MAP_COLS * 2) + (8 * 2)
HOST_LINE_ADDR  = SCREEN_ADDR + (3 * MAP_COLS * 2) + (8 * 2)

LOGO_9C         = 1
LOGO_DF         = 2
LOGO_A9         = 3
LOGO_9A         = 4
LOGO_A5         = 5
LOGO_A7         = 6
LOGO_9F         = 7
LOGO_B5         = 8
LOGO_B6         = 9
LOGO_B7         = 10
LOGO_BB         = 11
LOGO_AC         = 12
LOGO_9E         = 13
LOGO_AF         = 14
LOGO_BE         = 15
LOGO_BC         = 16
LOGO_81         = 17
LOGO_AA         = 18
LOGO_B4         = 19

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

    ; Copy CIO gate (dispatch table + stubs) to page-6 RAM
    ldy #(GATE_CODE_LEN - 1)
@copy:
    lda gate_code,y
    sta GATE_RAM,y
    dey
    bpl @copy

    ; Register the RAM gate in HATABS (not GENDEV: that reads $D80D each
    ; call, which would hit the math pack when the ROM is not mapped)
    ldx #DEVNAM
    lda #>GATE_RAM
    ldy #<GATE_RAM
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

    jsr DRAW_LOGO
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

CLEAR_SCREEN:
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
    rts

LOAD_BOOT_FONT:
    ; Load only the 46 chars needed by the boot banner into VERA VRAM.
    ; Each entry in boot_font_data: ADDR_L, ADDR_M, d0..d7 (10 bytes).
    ; ADDR_H ($D102) is set once — increment selector persists across
    ; ADDR_L/M writes, so DATA0 auto-advances correctly per character.
    lda #<boot_font_data
    sta TMP_PTR_LO
    lda #>boot_font_data
    sta TMP_PTR_HI
    lda #(VERA_INC1 | $01)
    sta VERA_ADDR_H
    ldx #BOOT_FONT_COUNT
@entry:
    ldy #0
    lda (TMP_PTR_LO),y
    sta VERA_ADDR_L
    iny
    lda (TMP_PTR_LO),y
    sta VERA_ADDR_M
    ldy #2
@tile:
    lda (TMP_PTR_LO),y
    sta VERA_DATA0
    iny
    cpy #10
    bne @tile
    lda TMP_PTR_LO
    clc
    adc #10
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

DRAW_LOGO:
    ldx #0
@Next:
    lda LogoColors,x
    sta TMP0
    lda LogoAddrLo,x
    sta VERA_ADDR_L
    lda LogoAddrHi,x
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^LOGO1_ADDR)
    sta VERA_ADDR_H
    lda LogoPtrLo,x
    sta TMP_PTR_LO
    lda LogoPtrHi,x
    sta TMP_PTR_HI
    jsr PRINT_PTR
    inx
    cpx #7
    bne @Next
    lda #TEXT_COLOR
    sta TMP0
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
    lda #<VER_LINE_ADDR
    sta VERA_ADDR_L
    lda #>VER_LINE_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^VER_LINE_ADDR)
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
    lda #<HOST_LINE_ADDR
    sta VERA_ADDR_L
    lda #>HOST_LINE_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^HOST_LINE_ADDR)
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
    cmp #XIO_VERA_HOOKS
    beq @Hooks

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
    lda #VERA_REQ_CURSOR
    sta VERA_CTL_REQUEST
    jsr CALL_SERVICE
    jmp NONEED

@Hooks:
    lda #VERA_REQ_HOOKS
    sta VERA_CTL_REQUEST
    jsr CALL_SERVICE
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

LogoLine1:
    .byte $02, ' ', ' ', ' ', ' ', ' ', $03, 0
LogoLine2:
    .byte $05, $02, ' ', ' ', ' ', $03, $06, 0
LogoLine3:
    .byte $08, $09, $02, ' ', $03, $09, $0A, 0
LogoLine4:
    .byte ' ', $0C, $0D, ' ', $0E, $0C, 0
LogoLine5:
    .byte ' ', $0F, $10, ' ', $11, $0F, 0
LogoLine6:
    .byte $13, $09, $14, ' ', $15, $09, $16, 0
LogoLine7:
    .byte $17, $14, ' ', ' ', ' ', $15, $18, 0

LogoAddrLo:
    .byte <LOGO1_ADDR, <LOGO2_ADDR, <LOGO3_ADDR, <LOGO4_ADDR
    .byte <LOGO5_ADDR, <LOGO6_ADDR, <LOGO7_ADDR
LogoAddrHi:
    .byte >LOGO1_ADDR, >LOGO2_ADDR, >LOGO3_ADDR, >LOGO4_ADDR
    .byte >LOGO5_ADDR, >LOGO6_ADDR, >LOGO7_ADDR
LogoPtrLo:
    .byte <LogoLine1, <LogoLine2, <LogoLine3, <LogoLine4
    .byte <LogoLine5, <LogoLine6, <LogoLine7
LogoPtrHi:
    .byte >LogoLine1, >LogoLine2, >LogoLine3, >LogoLine4
    .byte >LogoLine5, >LogoLine6, >LogoLine7
LogoColors:
    .byte $64, $6E, $6D, $65, $67, $68, $62

VersionPrefix:
    .asciiz "VERA MODULE FW:"
Host600XL:
    .asciiz "ATARI 600XL"
Host800XL:
    .asciiz "ATARI 800XL"
Host130XE:
    .asciiz "ATARI 130XE"

; -------------------------------------------------------------------------
; gate_code — 75 bytes copied to GATE_RAM ($0600) by INIT
;
; CIO uses the HATABS address as the base of a 12-byte dispatch table and
; picks the handler vector by command offset (OPEN=0, CLOSE=2, GET=4,
; PUT=6, STATUS=8, SPECIAL=10).
;
; OPEN/CLOSE share stub_noneed: NONEED needs no hardware access so we
; inline it (no ROM mapping). GET/PUT/STATUS/SPECIAL each have a 12-byte
; stub that maps the VERA ROM (D1FF=$80), JSRs into the ROM handler, then
; JMPs to the shared epilogue which deselects (D1FF=$00) and returns.
; LDA/STA in the epilogue leave Y and C (status/error from the handler)
; untouched, so CIO gets the correct return values.
; -------------------------------------------------------------------------
gate_code:
    ; Dispatch table (12 bytes): addr-1 for each CIO function.
    ; OPEN and CLOSE share stub_noneed (no ROM mapping needed).
    .word GATE_STUB_NONEED  - 1     ; OPEN
    .word GATE_STUB_NONEED  - 1     ; CLOSE
    .word GATE_STUB_GET     - 1     ; GET
    .word GATE_STUB_PUT     - 1     ; PUT
    .word GATE_STUB_STATUS  - 1     ; STATUS
    .word GATE_STUB_SPECIAL - 1     ; SPECIAL
    ; stub_noneed (8 bytes): OPEN/CLOSE need no ROM — inline NONEED result
    .byte $A9, $00              ; LDA #$00
    .byte $85, CRITIC           ; STA CRITIC
    .byte $A0, $01              ; LDY #1
    .byte $38                   ; SEC
    .byte $60                   ; RTS
    ; epilogue (7 bytes): shared tail — Y and C from handler are preserved
    .byte $A9, $00              ; LDA #$00
    .byte $8D, $FF, $D1         ; STA $D1FF  (deselect VERA, math pack back)
    .byte $58                   ; CLI
    .byte $60                   ; RTS
    ; stub_get (12 bytes): SEI / map ROM / JSR GETBYT / JMP epilogue
    .byte $78, $A9, DEVICE_ID_MASK, $8D, $FF, $D1, $20, <GETBYT,  >GETBYT,  $4C, <GATE_EPILOGUE, >GATE_EPILOGUE
    ; stub_put (12 bytes)
    .byte $78, $A9, DEVICE_ID_MASK, $8D, $FF, $D1, $20, <PUTBYT,  >PUTBYT,  $4C, <GATE_EPILOGUE, >GATE_EPILOGUE
    ; stub_status (12 bytes)
    .byte $78, $A9, DEVICE_ID_MASK, $8D, $FF, $D1, $20, <GETSTA,  >GETSTA,  $4C, <GATE_EPILOGUE, >GATE_EPILOGUE
    ; stub_special (12 bytes)
    .byte $78, $A9, DEVICE_ID_MASK, $8D, $FF, $D1, $20, <SPECIAL, >SPECIAL, $4C, <GATE_EPILOGUE, >GATE_EPILOGUE
GATE_CODE_LEN = * - gate_code

; Boot font: 46 chars x 10 bytes = 460 bytes.
; Only the characters actually used by the boot banner (logo tiles +
; text chars). vera_sys will overwrite these with the full 1KB font.
; Format per entry: ADDR_L, ADDR_M, d0..d7
; ADDR_H constant = $11 (VERA bank 1, inc 1), set once by LOAD_BOOT_FONT.
BOOT_FONT_COUNT = 46

boot_font_data:
    .byte $10,$F0, $00,$80,$C0,$E0,$F0,$F8,$FC,$FE  ; tile $02
    .byte $18,$F0, $00,$01,$03,$07,$0F,$1F,$3F,$7F  ; tile $03
    .byte $28,$F0, $7F,$7F,$7F,$7F,$7F,$7F,$7F,$7F  ; tile $05
    .byte $30,$F0, $FE,$FE,$FE,$FE,$FE,$FE,$FE,$FE  ; tile $06
    .byte $40,$F0, $1F,$1F,$1F,$1F,$1F,$1F,$1F,$1F  ; tile $08
    .byte $48,$F0, $FF,$FF,$FF,$FF,$FF,$FF,$FF,$FF  ; tile $09
    .byte $50,$F0, $F8,$F8,$F8,$F8,$F8,$F8,$F8,$F8  ; tile $0A
    .byte $60,$F0, $FF,$FF,$00,$00,$00,$00,$00,$00  ; tile $0C
    .byte $68,$F0, $FF,$FF,$FF,$FF,$0F,$0F,$0F,$0F  ; tile $0D
    .byte $70,$F0, $FF,$FF,$FF,$FF,$F0,$F0,$F0,$F0  ; tile $0E
    .byte $78,$F0, $00,$00,$00,$00,$00,$00,$FF,$FF  ; tile $0F
    .byte $80,$F0, $0F,$0F,$0F,$0F,$FF,$FF,$FF,$FF  ; tile $10
    .byte $88,$F0, $F0,$F0,$F0,$F0,$FF,$FF,$FF,$FF  ; tile $11
    .byte $98,$F0, $03,$03,$03,$03,$03,$03,$03,$03  ; tile $13
    .byte $A0,$F0, $FF,$FE,$FC,$F8,$F0,$E0,$C0,$80  ; tile $14
    .byte $A8,$F0, $FF,$7F,$3F,$1F,$0F,$07,$03,$01  ; tile $15
    .byte $B0,$F0, $C0,$C0,$C0,$C0,$C0,$C0,$C0,$C0  ; tile $16
    .byte $B8,$F0, $07,$07,$07,$07,$07,$07,$07,$07  ; tile $17
    .byte $C0,$F0, $E0,$E0,$E0,$E0,$E0,$E0,$E0,$E0  ; tile $18
    .byte $00,$F1, $00,$00,$00,$00,$00,$00,$00,$00  ; ' '
    .byte $70,$F1, $00,$00,$00,$00,$00,$18,$18,$00  ; '.'
    .byte $80,$F1, $3C,$66,$6E,$76,$66,$66,$3C,$00  ; '0'
    .byte $88,$F1, $18,$18,$38,$18,$18,$18,$7E,$00  ; '1'
    .byte $90,$F1, $3C,$66,$06,$0C,$30,$60,$7E,$00  ; '2'
    .byte $98,$F1, $3C,$66,$06,$1C,$06,$66,$3C,$00  ; '3'
    .byte $A0,$F1, $06,$0E,$1E,$66,$7F,$06,$06,$00  ; '4'
    .byte $A8,$F1, $7E,$60,$7C,$06,$06,$66,$3C,$00  ; '5'
    .byte $B0,$F1, $3C,$66,$60,$7C,$66,$66,$3C,$00  ; '6'
    .byte $B8,$F1, $7E,$66,$0C,$18,$18,$18,$18,$00  ; '7'
    .byte $C0,$F1, $3C,$66,$66,$3C,$66,$66,$3C,$00  ; '8'
    .byte $C8,$F1, $3C,$66,$66,$3E,$06,$66,$3C,$00  ; '9'
    .byte $D0,$F1, $00,$00,$18,$00,$00,$18,$00,$00  ; ':'
    .byte $08,$F2, $18,$3C,$66,$7E,$66,$66,$66,$00  ; 'A'
    .byte $20,$F2, $78,$6C,$66,$66,$66,$6C,$78,$00  ; 'D'
    .byte $28,$F2, $7E,$60,$60,$78,$60,$60,$7E,$00  ; 'E'
    .byte $30,$F2, $7E,$60,$60,$78,$60,$60,$60,$00  ; 'F'
    .byte $48,$F2, $3C,$18,$18,$18,$18,$18,$3C,$00  ; 'I'
    .byte $60,$F2, $60,$60,$60,$60,$60,$60,$7E,$00  ; 'L'
    .byte $68,$F2, $63,$77,$7F,$6B,$63,$63,$63,$00  ; 'M'
    .byte $78,$F2, $3C,$66,$66,$66,$66,$66,$3C,$00  ; 'O'
    .byte $90,$F2, $7C,$66,$66,$7C,$78,$6C,$66,$00  ; 'R'
    .byte $A0,$F2, $7E,$18,$18,$18,$18,$18,$18,$00  ; 'T'
    .byte $A8,$F2, $66,$66,$66,$66,$66,$66,$3C,$00  ; 'U'
    .byte $B0,$F2, $66,$66,$66,$66,$66,$3C,$18,$00  ; 'V'
    .byte $B8,$F2, $63,$63,$63,$6B,$7F,$77,$63,$00  ; 'W'
    .byte $C0,$F2, $66,$66,$3C,$18,$3C,$66,$66,$00  ; 'X'

; End of ROM
