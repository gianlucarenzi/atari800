; vera_common.inc — Shared symbols for VERA PBI ROM and RAM driver.

; ============================================================================
; VERA hardware register base and register names
; ============================================================================

PBI_ADDR        = $D100             ; VERA PBI Base Address

VERA_ADDR_L     = PBI_ADDR + $00    ; VRAM address bits  7:0  (active port)
VERA_ADDR_M     = PBI_ADDR + $01    ; VRAM address bits 15:8
VERA_ADDR_H     = PBI_ADDR + $02    ; bit[0]=A16  bits[7:4]=INCR
VERA_DATA0      = PBI_ADDR + $03    ; VRAM data port 0
VERA_DATA1      = PBI_ADDR + $04    ; VRAM data port 1
VERA_CTRL_REG   = PBI_ADDR + $05    ; CTRL: ADDRSEL(0) DCSEL(1) RESET(7)
VERA_CTRL       = VERA_CTRL_REG
VERA_ADDRSEL_CLEAR = $FE
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

VERA_L1_CONFIG  = PBI_ADDR + $14    ; Layer 1 configuration
VERA_L1_MAPBASE = PBI_ADDR + $15    ; Layer 1 Mapbase
VERA_L1_TILEBASE= PBI_ADDR + $16    ; Layer 1 Tilebase
VERA_L1_HSCR_L  = PBI_ADDR + $17    ; Layer 1 HScroll Low
VERA_L1_HSCR_H  = PBI_ADDR + $18    ; Layer 1 HScroll High
VERA_L1_VSCR_L  = PBI_ADDR + $19    ; Layer 1 VScroll Low
VERA_L1_VSCR_H  = PBI_ADDR + $1A    ; Layer 1 VScroll High

VERA_REG_ARRAY  = PBI_ADDR + $00    ; Alias for VERA base

DEVICE_ID_MASK  = $80               ; Card ID bit (PBI bit 7)

VERA_INC0       = $00               ; No VRAM auto-increment
VERA_INC1       = $10               ; Auto-increment VRAM by 1

VERA_DCSEL0     = $00               ; Bank 0 for VERA DC registers
VERA_DCSEL1     = $02               ; Bank 1 for VERA DC registers

VERA_VIDEO_VGA  = $01               ; VGA output (640x480)
VERA_LAYER1_EN  = $20               ; Enable Layer 1

VERA_MAP_128x64 = $60               ; 128-tile wide, 64-tile tall map

SCREEN_ADDR     = $01B000           ; VRAM tilemap start
CHARSET_ADDR    = $01F000           ; VRAM character glyphs start

SCREEN_MAPBASE  = $D8               ; L1_MAPBASE derived from SCREEN_ADDR
SCREEN_TILEBASE = $F8               ; L1_TILEBASE derived from CHARSET_ADDR

SCREEN_COLS     = 80                ; Viewport columns
MAP_COLS        = 128               ; VRAM map width
MAP_ROWS        = 64                ; VRAM map height
SCREEN_ROWS     = 25                ; Viewport rows
TEXT_COLOR      = $61               ; Default text color

DC_HSTART_VAL   = $00               ; Horizontal start register value
DC_HSTOP_VAL    = $A0               ; Horizontal stop register value
DC_VSTART_VAL   = $00               ; Vertical start register value
DC_VSTOP_VAL    = $F0               ; Vertical stop register value

PBI_LATCH       = $D1FF             ; PBI latch address

; VBI/Screen Timing and Constants
VBI_RATE            = 10            ; frames between metronome ticks
VBI_CURSOR_RATE     = 20            ; frames between cursor blink toggles
SCREEN_COLS_VIEW    = 80            ; Viewport columns
SCREEN_ROWS_VIEW    = 60            ; Viewport rows

; ============================================================================
; Screen layout — must stay in sync with vera_pbi_handler.s
;   SCREEN_ADDR  = $01B000    (bank 1, mid byte $B0)
;   MAP_COLS     = 128        → per-row stride = 256 bytes
;   visible 80x25 inside a 128x64 tilemap
; ============================================================================

SCREEN_ADDR_M       = $B0
SCREEN_ADDR_BANK    = $01
VERA_ADDR_H_BASE    = VERA_INC1 | SCREEN_ADDR_BANK  ; $11

; OS Editor cursor shadow — driven by both PUT BYTE *and* arrow-key handling.
; The VBI snapshots these into VCTL so the VERA cursor follows arrow-key moves
; that never invoke CIO.
ROWCRS_OS           = $54
COLCRS_OS           = $55           ; LO byte only (HI is for graphics modes)

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

VBI_FREQ            = $08
VBI_VOLUME          = $AF

; ==================================
; OS Equate as a workaround for VBI
; ==================================

mSETVBV = $E45C
