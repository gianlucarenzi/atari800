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

; PBI device management (Atari OS page 2 locations)
PDVMSK  = $0247         ; PBI device mask     (enabled-device bitmask)
PNDEVREQ = $0248        ; PBI device request  (this device's bit, set by OS)
PDIMSK  = $0249         ; PBI interrupt mask

; Atari OS routines used for CIO device registration
NEWDEV  = $E486         ; Install device handler in HATABS
GENDEV  = $E48F         ; Generic CIO device handler vector

; IOCB fields (relative to IOCB base; CIO passes X = IOCB offset)
ICAX1   = $034A         ; Auxiliary byte 1 (used here as register index)
CRITIC  = $42           ; Critical section flag (0 = deferred VBI enabled)

; ============================================================================
; VERA hardware register base and register names
; (PBI_ADDR = $D100 — matches pbi_verax16.c VERA_REG_BASE)
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

; DCSEL=0 bank (bit 1 of CTRL = 0):
VERA_DC_VIDEO   = PBI_ADDR + $09    ; Output enable, layer enable, sprites
VERA_DC_HSCALE  = PBI_ADDR + $0A    ; Horizontal scale (128 = 1:1)
VERA_DC_VSCALE  = PBI_ADDR + $0B    ; Vertical scale
VERA_DC_BORDER  = PBI_ADDR + $0C    ; Border colour index

; DCSEL=1 bank (bit 1 of CTRL = 1):
VERA_DC_HSTART  = PBI_ADDR + $09    ; Active area start column (/4)
VERA_DC_HSTOP   = PBI_ADDR + $0A    ; Active area stop  column (/4)
VERA_DC_VSTART  = PBI_ADDR + $0B    ; Active area start row    (/2)
VERA_DC_VSTOP   = PBI_ADDR + $0C    ; Active area stop  row    (/2)

; Layer 1 registers (fixed, no DCSEL mux):
VERA_L1_CONFIG  = PBI_ADDR + $14
VERA_L1_MAPBASE = PBI_ADDR + $15
VERA_L1_TILEBASE= PBI_ADDR + $16
VERA_L1_HSCR_L  = PBI_ADDR + $17
VERA_L1_HSCR_H  = PBI_ADDR + $18
VERA_L1_VSCR_L  = PBI_ADDR + $19
VERA_L1_VSCR_H  = PBI_ADDR + $1A

; Full register array base (used for indexed CIO access)
VERA_REG_ARRAY  = PBI_ADDR + $00

; ============================================================================
; VERA constants
; ============================================================================

DEVICE_ID_MASK  = $80           ; This card occupies PBI bit 7
DEVNAM          = 'V'           ; CIO device name registered in HATABS

; ADDR_H increment-selector nibbles (upper 4 bits)
VERA_INC0       = $00           ; No auto-increment
VERA_INC1       = $10           ; Auto-increment by 1

; CTRL bit patterns
VERA_DCSEL0     = $00           ; Access DC_VIDEO/HSCALE/VSCALE/BORDER bank
VERA_DCSEL1     = $02           ; Access DC_HSTART/HSTOP/VSTART/VSTOP bank

; DC_VIDEO bit flags
VERA_VIDEO_VGA  = $01           ; VGA output (640×480)
VERA_LAYER1_EN  = $20           ; Enable Layer 1

; Layer 1 config
VERA_MAP_128x64 = $60           ; 128-tile wide, 64-tile tall map

; Screen / charset layout in VERA VRAM
SCREEN_ADDR     = $01B000       ; Tilemap start (128×64 = 8 KB, in bank 1)
CHARSET_ADDR    = $01F000       ; Character glyphs (256 chars × 16 bytes)

SCREEN_MAPBASE  = $D8           ; L1_MAPBASE  = SCREEN_ADDR >> 9   ($01B000>>9=$D8)
SCREEN_TILEBASE = $FA           ; L1_TILEBASE = CHARSET_ADDR >> 9  ($01F000>>9=$F8 | height16=$02 => $FA)

SCREEN_COLS     = 80            ; visible display columns
MAP_COLS        = 128           ; VERA map width (must match L1_CONFIG bits[5:4]=10)
SCREEN_ROWS     = 25
TEXT_COLOR      = $61           ; Colour attribute: white text on blue background

; Visible display window (640×480 VGA, 1:1 scale → 160 cols/4, 240 rows/2)
DC_HSTART_VAL   = $00
DC_HSTOP_VAL    = $A0           ; 160
DC_VSTART_VAL   = $14           ; 20  (top margin)
DC_VSTOP_VAL    = $DC           ; 220 (= 20 + 200 visible rows / 2 = 120? adjust as needed)

; Banner VRAM addresses (centred rows within the 25-row text area)
; Row 9,  col 26: "**** COMMANDER X16 VERA ****"  (28 chars, centred in 80)
; Row 12, col 30: "PBI VIDEO INTERFACE"           (19 chars)
; Row 15, col 37: "READY."                        (6 chars)
; NOTE: row stride in VRAM = MAP_COLS * 2 = 256 bytes, NOT SCREEN_COLS * 2!
BANNER1_ADDR    = SCREEN_ADDR + (9  * MAP_COLS * 2) + (26 * 2)
BANNER2_ADDR    = SCREEN_ADDR + (12 * MAP_COLS * 2) + (30 * 2)
BANNER3_ADDR    = SCREEN_ADDR + (15 * MAP_COLS * 2) + (37 * 2)

; ============================================================================
; Helper macro: write a zero-terminated string to VERA VRAM at a given
; VERA address, using TEXT_COLOR as the colour attribute byte.
;   addr  — 24-bit VERA VRAM address (.define constant)
;   label — label of a .asciiz string in this ROM
; ============================================================================

.macro PRINT_LINE addr, label
    .local CopyChar, Done
    lda #<(addr)
    sta VERA_ADDR_L
    lda #>(addr)
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^(addr))     ; bank byte = A16, increment = 1
    sta VERA_ADDR_H
    ldx #0
CopyChar:
    lda label,x
    beq Done
    sta VERA_DATA0                  ; character byte
    lda #TEXT_COLOR
    sta VERA_DATA0                  ; colour attribute byte
    inx
    bne CopyChar
Done:
.endmacro

; ============================================================================
; ROM starts here — $D800
; The linker config places the CODE segment at $D800.
; ============================================================================

    .segment "CODE"

; --------------------------------------------------------------------------
; PBI ROM header ($D800-$D81C)
; --------------------------------------------------------------------------

    .word $0000             ; $D800: checksum (not verified by OS, set to 0)
    .byte $00               ; $D802: revision
    .byte DEVICE_ID_MASK    ; $D803: PBI device ID (must match $D1FF value)
    .byte $00               ; $D804: device flags / type

    jmp IOVECTOR            ; $D805: low-level I/O handler
    jmp IRQVECTOR           ; $D808: interrupt handler

    .byte $91               ; $D80B: manufacturer code (Atari-compatible)
    .byte DEVNAM            ; $D80C: CIO device name for HATABS

    .word NONEED-1          ; $D80D: OPEN  vector (address − 1)
    .word NONEED-1          ; $D80F: CLOSE vector
    .word GETBYT-1          ; $D811: GET BYTE vector
    .word PUTBYT-1          ; $D813: PUT BYTE vector
    .word GETSTA-1          ; $D815: GET STATUS vector
    .word NONEED-1          ; $D817: SPECIAL vector

    jmp INIT                ; $D819: INIT handler (cold/warm start)
    .byte $00               ; $D81C: reserved

; --------------------------------------------------------------------------
; Low-level I/O handler — not used, return carry clear
; --------------------------------------------------------------------------

IOVECTOR:
    clc
    rts

; --------------------------------------------------------------------------
; IRQ handler — currently no interrupt processing
; --------------------------------------------------------------------------

IRQVECTOR:
    rts

; --------------------------------------------------------------------------
; INIT — called by the OS at cold / warm start with X = IOCB offset,
;        $0248 (PNDEVREQ) holds this device's bit mask.
; --------------------------------------------------------------------------

INIT:
    ; Register this device's bit in the PBI device-mask so the OS knows
    ; the card is present.
    lda PDVMSK
    ora PNDEVREQ
    sta PDVMSK

    ; Register the CIO device handler using the Roland Scholz / FJC method.
    ; NEWDEV adds an entry to HATABS; already-present entries are left intact.
    ldx #DEVNAM
    lda #>GENDEV
    ldy #<GENDEV
    jsr NEWDEV              ; N=1 → failed; C=0 → success; C=1 → already exists

    ; Initialise the VERA chip and display the boot banner.
    jsr INIT_VERA_SCREEN
    rts

; --------------------------------------------------------------------------
; INIT_VERA_SCREEN — configure VERA for 80×25 text mode and show banner
; --------------------------------------------------------------------------

INIT_VERA_SCREEN:
    jsr WAIT_VERA

    ; Reset VERA state (clears registers, not VRAM)
    lda #VERA_DCSEL0
    sta VERA_CTRL_REG
    lda #$00
    sta VERA_IEN
    sta VERA_ISR

    ; Upload the full Atari font (128 characters)
    jsr LOAD_FULL_FONT

    ; Configure Layer 1: 128×64 tilemap, 8×8 tiles
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

    ; Display Composer — secondary window (DCSEL=1: HSTART/HSTOP/VSTART/VSTOP)
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

    ; Display Composer — primary (DCSEL=0: VIDEO/HSCALE/VSCALE/BORDER)
    lda #VERA_DCSEL0
    sta VERA_CTRL_REG
    lda #(VERA_VIDEO_VGA | VERA_LAYER1_EN)
    sta VERA_DC_VIDEO
    lda #$80                ; HSCALE = 128 → 1:1 horizontal
    sta VERA_DC_HSCALE
    lda #$80                ; VSCALE = 128 → 1:1 vertical
    sta VERA_DC_VSCALE
    lda #$00                ; border colour = palette entry 0
    sta VERA_DC_BORDER

    jsr CLEAR_SCREEN

    ; Force palette entry 1 = white (#FFF): byte0=GGGGBBBB=$FF, byte1=0000RRRR=$0F
    ; VRAM address: $1FA00 + 1*2 = $1FA02
    lda #$02
    sta VERA_ADDR_L
    lda #$FA
    sta VERA_ADDR_M
    lda #(VERA_INC1 | $01)      ; increment=1, bank=1
    sta VERA_ADDR_H
    lda #$FF
    sta VERA_DATA0               ; G=$F, B=$F
    lda #$0F
    sta VERA_DATA0               ; R=$F

    PRINT_LINE BANNER1_ADDR, BannerLine1
    PRINT_LINE BANNER2_ADDR, BannerLine2
    PRINT_LINE BANNER3_ADDR, BannerLine3
    rts

; --------------------------------------------------------------------------
; WAIT_VERA — verify VERA is responding by reading back a register
; --------------------------------------------------------------------------

WAIT_VERA:
    ldx #$FF
@Loop:
    lda #$2A                ; test value
    sta VERA_ADDR_L
    lda VERA_ADDR_L
    cmp #$2A
    beq @Done
    dex
    bne @Loop
@Done:
    rts

; --------------------------------------------------------------------------
; CLEAR_SCREEN — fill the 80×25 tilemap with space + TEXT_COLOR
; --------------------------------------------------------------------------

CLEAR_SCREEN:
    lda #<SCREEN_ADDR
    sta VERA_ADDR_L
    lda #>SCREEN_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^SCREEN_ADDR)
    sta VERA_ADDR_H
    ldy #SCREEN_ROWS
@Row:
    ldx #MAP_COLS           ; write full map width (128), not just visible 80
@Col:
    lda #' '
    sta VERA_DATA0          ; character
    lda #TEXT_COLOR
    sta VERA_DATA0          ; colour attribute
    dex
    bne @Col
    dey
    bne @Row
    rts

; --------------------------------------------------------------------------
; LOAD_FULL_FONT — copy the full 128-char Atari font into VERA VRAM.
; Each charset slot is 16 bytes (8-row bitmap written twice).
; --------------------------------------------------------------------------

LOAD_FULL_FONT:
    lda #<CHARSET_ADDR
    sta VERA_ADDR_L
    lda #>CHARSET_ADDR
    sta VERA_ADDR_M
    lda #(VERA_INC1 | ^CHARSET_ADDR)
    sta VERA_ADDR_H

    ; Setup pointer to FontData in zero-page ($43-$44)
    lda #<FontData
    sta $43
    lda #>FontData
    sta $44

    ldx #0                  ; X = character index (0-127)
@NextChar:
    ldy #0
@CopyRows:
    lda ($43),y
    sta VERA_DATA0          ; write row twice for 16-pixel height
    sta VERA_DATA0
    iny
    cpy #8
    bne @CopyRows

    ; Advance FontData pointer by 8
    lda $43
    clc
    adc #8
    sta $43
    lda $44
    adc #0
    sta $44

    inx
    cpx #128
    bne @NextChar
    rts


; --------------------------------------------------------------------------
; CIO GET BYTE — read VERA register at index ICAX1
;   X = IOCB offset
;   Returns: A = register value, Y = 1, C = 1 (success)
; --------------------------------------------------------------------------

GETBYT:
    lda #$00
    sta CRITIC              ; enable deferred VBI
    ldy ICAX1,x             ; Y = register index from IOCB AUX1
    lda VERA_REG_ARRAY,y    ; read VERA register
    ldy #1
    sec
    rts

; --------------------------------------------------------------------------
; CIO PUT BYTE — write VERA register at index ICAX1
;   X = IOCB offset, A = byte to write
;   Returns: Y = 1, C = 1 (success)
; --------------------------------------------------------------------------

PUTBYT:
    pha
    lda #$00
    sta CRITIC              ; enable deferred VBI
    ldy ICAX1,x             ; Y = register index from IOCB AUX1
    pla
    sta VERA_REG_ARRAY,y    ; write VERA register
    ldy #1
    sec
    rts

; --------------------------------------------------------------------------
; CIO GET STATUS — return VERA CTRL register
;   Returns: A = CTRL value, Y = 1, C = 1 (success)
; --------------------------------------------------------------------------

GETSTA:
    lda #$00
    sta CRITIC
    lda VERA_CTRL_REG
    ldy #1
    sec
    rts

; --------------------------------------------------------------------------
; NONEED — no-op handler for OPEN / CLOSE / SPECIAL
;   Returns: Y = 1, C = 1 (success / not needed)
; --------------------------------------------------------------------------

NONEED:
    ldy #1
    sec
    rts

; --------------------------------------------------------------------------
; String data — zero-terminated banner lines
; --------------------------------------------------------------------------

BannerLine1:
    .asciiz "**** COMMANDER X16 VERA ****"

BannerLine2:
    .asciiz "PBI VIDEO INTERFACE"

BannerLine3:
    .asciiz "READY."

; --------------------------------------------------------------------------
; FontData — Full 128-character Atari font
; --------------------------------------------------------------------------

FontData:
    .incbin "atari_font.bin"

; End of ROM
