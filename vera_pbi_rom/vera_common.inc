; vera_common.inc — Shared symbols for VERA PBI ROM and RAM driver.

    .include "atari.inc"

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

; ============================================================================
; VERA constants
; ============================================================================

DEVICE_ID_MASK  = $80           ; This card occupies PBI bit 7

VERA_INC0       = $00           ; No auto-increment
VERA_INC1       = $10           ; Auto-increment by 1

VERA_DCSEL0     = $00           ; Access DC_VIDEO/HSCALE/VSCALE/BORDER bank
VERA_DCSEL1     = $02           ; Access DC_HSTART/HSTOP/VSTART/VSTOP bank

VERA_VIDEO_VGA  = $01           ; VGA output (640x480)
VERA_LAYER1_EN  = $20           ; Enable Layer 1

VERA_MAP_128x64 = $60           ; 128-tile wide, 64-tile tall map

SCREEN_ADDR     = $01B000       ; Tilemap start (128x64 = 8 KB, in bank 1)
SCREEN_ADDR_M   = >SCREEN_ADDR
CHARSET_ADDR    = $01F000       ; Character glyphs (256 chars x 8 bytes)

SCREEN_MAPBASE  = $D8           ; L1_MAPBASE  = SCREEN_ADDR >> 9
SCREEN_TILEBASE = $F8           ; L1_TILEBASE = CHARSET_ADDR >> 9, 8x8 tiles

VERA_ADDRSEL_CLEAR  = $FE

MAP_COLS        = 128
MAP_ROWS        = 64
SCREEN_COLS     = 80
SCREEN_ROWS     = 25
TEXT_COLOR      = $61           ; White on blue

; Legacy aliases for existing driver code
VERA_CTRL           = VERA_CTRL_REG
VERA_SCREEN_BASE_M  = >SCREEN_ADDR
VERA_ADDR_H_BASE    = (VERA_INC1 | ^SCREEN_ADDR)
VERA_TEXT_COLOR     = TEXT_COLOR

CHARSET_VRAM_L      = <CHARSET_ADDR
CHARSET_VRAM_M      = >CHARSET_ADDR
CHARSET_VRAM_H      = (VERA_INC1 | ^CHARSET_ADDR)

; ============================================================================
; ATASCII control codes
; ============================================================================

ATASCII_EOL         = $9B
ATASCII_CLEAR       = $7D
ATASCII_BACKSPACE   = $7E
ATASCII_TAB         = $7F
ATASCII_BELL        = $FD
ATASCII_ESC         = $1B
ATASCII_CURSOR_UP   = $1C
ATASCII_CURSOR_DOWN = $1D
ATASCII_CURSOR_LEFT = $1E
ATASCII_CURSOR_RIGHT = $1F
ATASCII_DELETE_LINE = $9C
ATASCII_INSERT_LINE = $9D
ATASCII_DELETE_CHAR = $FE
ATASCII_INSERT_CHAR = $FF

VERA_INVERSE_COLOR  = $16           ; swap nibbles of $61: BG=1 white, FG=6 blue

; ============================================================================
; Viewport constants
; ============================================================================

SCREEN_COLS_VIEW    = 80
SCREEN_ROWS_VIEW    = 60
READY_ROW           = 8             ; row used by warm_reinit's banner
LMARGIN             = $52           ; Atari OS left margin

; ============================================================================
; PBI equates
; ============================================================================

PNDEVREQ    = $0248         ; PBI device request  (this device's bit, set by OS)

; ============================================================================
; VCTL block offsets
; ============================================================================

VERACTL_FLAGS       = 4
VERACTL_REQUEST     = 5
VERACTL_PARAM0      = 6
VERACTL_PARAM1      = 7
VERACTL_CURSOR_X    = 8
VERACTL_CURSOR_Y    = 9

VCTL_FLAG_METRONOME = $01
VCTL_FLAG_ESCAPE    = $02
VCTL_FLAG_API_READY = $80

VERA_REQ_PUTC       = $03
