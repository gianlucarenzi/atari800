; vera_common.inc — Shared symbols for VERA PBI ROM and RAM driver.
; All symbol names are padded to 24 characters so the '=' falls at column 25.

; ============================================================================
; PBI hardware interface
; ============================================================================

PBI_ADDR                = $D100                     ; VERA PBI base address ($D100–$D11F)
PBI_LATCH               = $D1FF                     ; PBI device-select latch

; ============================================================================
; VERA registers (at PBI_ADDR + offset)
; ============================================================================

VERA_ADDR_L             = PBI_ADDR+$00              ; VRAM address bits  7:0 (active port)
VERA_ADDR_M             = PBI_ADDR+$01              ; VRAM address bits 15:8
VERA_ADDR_H             = PBI_ADDR+$02              ; bit[0]=A16, bits[7:4]=increment
VERA_DATA0              = PBI_ADDR+$03              ; VRAM data port 0
VERA_DATA1              = PBI_ADDR+$04              ; VRAM data port 1
VERA_CTRL_REG           = PBI_ADDR+$05              ; ADDRSEL(0) DCSEL(1) RESET(7)
VERA_CTRL               = VERA_CTRL_REG             ; alias
VERA_IEN                = PBI_ADDR+$06              ; interrupt enable register
VERA_ISR                = PBI_ADDR+$07              ; interrupt status (write 1 to clear)

; ============================================================================
; VERA display composer — DCSEL=0 (output mode, scaling, border)
; ============================================================================

VERA_DC_VIDEO           = PBI_ADDR+$09              ; output mode, layer/sprite enable
VERA_DC_HSCALE          = PBI_ADDR+$0A              ; horizontal scale (128 = 1:1)
VERA_DC_VSCALE          = PBI_ADDR+$0B              ; vertical scale   (128 = 1:1)
VERA_DC_BORDER          = PBI_ADDR+$0C              ; border colour index

; ============================================================================
; VERA display composer — DCSEL=1 (active area clipping)
; ============================================================================

VERA_DC_HSTART          = PBI_ADDR+$09              ; active area start column (/4)
VERA_DC_HSTOP           = PBI_ADDR+$0A              ; active area stop  column (/4)
VERA_DC_VSTART          = PBI_ADDR+$0B              ; active area start row    (/2)
VERA_DC_VSTOP           = PBI_ADDR+$0C              ; active area stop  row    (/2)

; ============================================================================
; VERA layer 1 registers
; ============================================================================

VERA_L1_CONFIG          = PBI_ADDR+$14              ; map mode and colour depth
VERA_L1_MAPBASE         = PBI_ADDR+$15              ; tilemap base (VRAM addr / 512)
VERA_L1_TILEBASE        = PBI_ADDR+$16              ; glyph base   (VRAM addr / 512)
VERA_L1_HSCR_L          = PBI_ADDR+$17              ; horizontal scroll low  byte
VERA_L1_HSCR_H          = PBI_ADDR+$18              ; horizontal scroll high byte
VERA_L1_VSCR_L          = PBI_ADDR+$19              ; vertical scroll low  byte
VERA_L1_VSCR_H          = PBI_ADDR+$1A              ; vertical scroll high byte

; ============================================================================
; VERA control constants and bitmasks
; ============================================================================

VERA_REG_ARRAY          = PBI_ADDR+$00              ; alias for register file base
VERA_ADDRSEL_CLEAR      = $FE                       ; AND mask: clear ADDRSEL bit in CTRL
DEVICE_ID_MASK          = $80                       ; PBI card-ID bit (bit 7 of device byte)

VERA_INC0               = $00                       ; ADDR_H: no auto-increment
VERA_INC1               = $10                       ; ADDR_H: auto-increment by 1
VERA_INC2               = $20                       ; ADDR_H: auto-increment by 2
VERA_INC4               = $30                       ; ADDR_H: auto-increment by 4
VERA_INCN1              = $18                       ; ADDR_H: auto-decrement by 1 (step -1)

VERA_DCSEL0             = $00                       ; CTRL: select display composer bank 0
VERA_DCSEL1             = $02                       ; CTRL: select display composer bank 1
VERA_DCSEL2             = $04                       ; CTRL: select FX bank 2 (FX_CTRL … FX_MULT)
VERA_DCSEL3             = $06                       ; CTRL: select FX bank 3 (FX_X/Y_INCR)
VERA_DCSEL4             = $08                       ; CTRL: select FX bank 4 (FX_X/Y_POS integer)
VERA_DCSEL5             = $0A                       ; CTRL: select FX bank 5 (FX_X/Y_POS_S, poly fill)
VERA_DCSEL6             = $0C                       ; CTRL: select FX bank 6 (FX_CACHE)

VERA_VIDEO_VGA          = $01                       ; DC_VIDEO: VGA output mode (640×480)
VERA_LAYER1_EN          = $20                       ; DC_VIDEO: enable layer 1
VERA_MAP_128x64         = $60                       ; L1_CONFIG: 128-wide × 64-tall tilemap

; ============================================================================
; FX coprocessor — register addresses at $D109-$D10C, muxed by CTRL[DCSEL]
; Usage pattern: sta VERA_CTRL_REG with the DCSEL value, then access the
; physical address.  All four addresses are shared across DCSEL banks 2-6.
; ============================================================================

; DCSEL=2 — control and geometry setup
VERA_FX_CTRL            = PBI_ADDR+$09              ; FX_CTRL:     Addr1 mode, cache flags, 4-bit, transparency
VERA_FX_TILEBASE        = PBI_ADDR+$0A              ; FX_TILEBASE: tile-data base (addr>>9)[5:0], clip, 2-bit poly
VERA_FX_MAPBASE         = PBI_ADDR+$0B              ; FX_MAPBASE:  tilemap base  (addr>>9)[5:0], map size[1:0]
VERA_FX_MULT            = PBI_ADDR+$0C              ; FX_MULT:     multiplier / accumulator control

; DCSEL=3 — X and Y increments (6.9 fixed point, signed 15-bit + 32× flag)
VERA_FX_X_INCR_L        = PBI_ADDR+$09              ; FX_X_INCR_L: X_Incr[-2:-9]
VERA_FX_X_INCR_H        = PBI_ADDR+$0A              ; FX_X_INCR_H: [7]=32× flag, [6:0]=X_Incr[5:-1]
VERA_FX_Y_INCR_L        = PBI_ADDR+$0B              ; FX_Y_INCR_L: Y_Incr[-2:-9]
VERA_FX_Y_INCR_H        = PBI_ADDR+$0C              ; FX_Y_INCR_H: [7]=32× flag, [6:0]=Y_Incr[5:-1]

; DCSEL=4 — X and Y integer position (11.9 fixed point, integer part)
VERA_FX_X_POS_L         = PBI_ADDR+$09              ; FX_X_POS_L: X_Pos[7:0]
VERA_FX_X_POS_H         = PBI_ADDR+$0A              ; FX_X_POS_H: [7]=X_Pos[-9], [2:0]=X_Pos[10:8]
VERA_FX_Y_POS_L         = PBI_ADDR+$0B              ; FX_Y_POS_L: Y_Pos[7:0]
VERA_FX_Y_POS_H         = PBI_ADDR+$0C              ; FX_Y_POS_H: [7]=Y_Pos[-9], [2:0]=Y_Pos[10:8]

; DCSEL=5 — subpixel fraction and polygon fill length
VERA_FX_X_POS_S         = PBI_ADDR+$09              ; FX_X_POS_S:     X_Pos[-1:-8] (subpixel fraction)
VERA_FX_Y_POS_S         = PBI_ADDR+$0A              ; FX_Y_POS_S:     Y_Pos[-1:-8] (subpixel fraction)
VERA_FX_POLY_FILL_L     = PBI_ADDR+$0B              ; FX_POLY_FILL_L: fill span low  (read-only)
VERA_FX_POLY_FILL_H     = PBI_ADDR+$0C              ; FX_POLY_FILL_H: fill span high (read-only)

; DCSEL=6 — 32-bit write cache (and accumulator side-effects on read)
; Reading $D109 (FX_ACCUM_RESET) resets fx_mult_accumulator, returns cache[7:0]
; Reading $D10A (FX_ACCUM)       triggers accumulate step,    returns cache[15:8]
VERA_FX_CACHE_L         = PBI_ADDR+$09              ; FX_CACHE_L / FX_ACCUM_RESET (DCSEL=6)
VERA_FX_CACHE_M         = PBI_ADDR+$0A              ; FX_CACHE_M / FX_ACCUM       (DCSEL=6)
VERA_FX_CACHE_H         = PBI_ADDR+$0B              ; FX_CACHE_H / mult_b[7:0]    (DCSEL=6)
VERA_FX_CACHE_U         = PBI_ADDR+$0C              ; FX_CACHE_U / mult_b[15:8]   (DCSEL=6)

; ============================================================================
; FX_CTRL (DCSEL=2, $D109) bit masks and Addr1 mode constants
; ============================================================================

VERA_FX_TRANSP          = $80                       ; bit 7: transparency — skip zero bytes/nibbles on write
VERA_FX_CACHE_WR_EN     = $40                       ; bit 6: write 32-bit cache to VRAM on each DATA write
VERA_FX_CACHE_FILL_EN   = $20                       ; bit 5: fill cache from DATA0/DATA1 reads
VERA_FX_ONE_BYTE_CACHE  = $10                       ; bit 4: one-byte cache cycling (vs full 4-byte)
VERA_FX_16BIT_HOP       = $08                       ; bit 3: 16-bit hop mode on address port 1
VERA_FX_4BIT_MODE       = $04                       ; bit 2: 4-bit (nibble) pixel mode
VERA_FX_ADDR1_NORMAL    = $00                       ; bits[1:0]=00: Addr1 normal (no FX helper)
VERA_FX_ADDR1_LINE      = $01                       ; bits[1:0]=01: Addr1 line-draw helper
VERA_FX_ADDR1_POLY      = $02                       ; bits[1:0]=10: Addr1 polygon-fill helper
VERA_FX_ADDR1_AFFINE    = $03                       ; bits[1:0]=11: Addr1 affine-transform helper

; ============================================================================
; FX_MULT (DCSEL=2, $D10C) bit masks
; ============================================================================

VERA_FX_MULT_RESET_ACCUM = $80                      ; bit 7: reset accumulator to 0 (write-only trigger)
VERA_FX_MULT_ACCUM      = $40                       ; bit 6: trigger one accumulate step on write
VERA_FX_MULT_SUB_EN     = $20                       ; bit 5: subtract (vs add) in accumulate
VERA_FX_MULT_EN         = $10                       ; bit 4: use multiplier result for VRAM write

; ============================================================================
; VRAM layout — 128 KB address space; text display lives in bank 1
; ============================================================================

SCREEN_ADDR             = $01B000                   ; tilemap start in VRAM
CHARSET_ADDR            = $01F000                   ; character glyph data in VRAM

SCREEN_MAPBASE          = <(SCREEN_ADDR>>9)         ; L1_MAPBASE  register value (addr/512)
SCREEN_TILEBASE         = <(CHARSET_ADDR>>9)        ; L1_TILEBASE register value (addr/512)

SCREEN_ADDR_M           = >SCREEN_ADDR              ; tilemap VRAM address mid  byte
SCREEN_ADDR_BANK        = ^SCREEN_ADDR              ; tilemap VRAM address bank byte
VERA_SCREEN_BASE_M      = SCREEN_ADDR_M             ; alias used in vera_driver.s
VERA_ADDR_H_BASE        = (VERA_INC1|^SCREEN_ADDR)  ; ADDR_H preset for tilemap port
VERA_ADDR_H_BASE_N1     = (VERA_INCN1|^SCREEN_ADDR) ; ADDR_H preset with step -1

CHARSET_VRAM_L          = <CHARSET_ADDR             ; glyph VRAM address byte 0
CHARSET_VRAM_M          = >CHARSET_ADDR             ; glyph VRAM address byte 1
CHARSET_VRAM_H          = (VERA_INC1|^CHARSET_ADDR) ; glyph ADDR_H with increment

; ============================================================================
; Screen / viewport configuration — select at build time:
;   make SCREEN=80x60   →  80×60 viewport (8-pixel-tall tiles, 640×480 VGA)
;   make SCREEN=80x30   →  80×30 viewport (16-pixel-tall tiles, default)
;   make SCREEN=40x30   →  40×30 viewport (8-pixel tiles, 2× scale, 320×240)
;
; Passing MODE_40X30=1 activates the 320×240 path with 2x hardware scaling.
; Passing FONT_8X8=1 selects the 8-pixel tall character set (font8x8.bin).
; ============================================================================

.ifdef MODE_40X30
TILE_HEIGHT             = 8                         ; tile height in pixels
FONT_PAGES              = 4                         ; 128 chars ×  8 bytes = 1024 = 4 × 256
SCREEN_COLS             = 40                        ; visible columns (320 ÷ 8)
SCREEN_ROWS             = 30                        ; visible rows    (240 ÷ 8)
SCREEN_TILEBASE_REG     = <(CHARSET_ADDR>>9)        ; L1_TILEBASE: bit1=0 → 8px tile height
HSCALE_VAL              = $40                       ; horizontal scale (64 = 2:1 magnification)
VSCALE_VAL              = $40                       ; vertical scale   (64 = 2:1 magnification)
INPUT_LINE_ROWS         = 3                         ; max logical line length (3 physical rows)
.else
SCREEN_COLS             = 80                        ; visible columns (640 ÷ 8)
HSCALE_VAL              = $80                       ; horizontal scale (128 = 1:1)
VSCALE_VAL              = $80                       ; vertical scale   (128 = 1:1)
INPUT_LINE_ROWS         = 2                         ; max logical line length (2 physical rows)
.ifdef FONT_8X8
TILE_HEIGHT             = 8                         ; tile height in pixels
FONT_PAGES              = 4                         ; 128 chars ×  8 bytes = 1024 = 4 × 256
SCREEN_ROWS             = 60                        ; visible rows  (480 ÷  8)
SCREEN_TILEBASE_REG     = <(CHARSET_ADDR>>9)        ; L1_TILEBASE: bit1=0 → 8px tile height
.else
TILE_HEIGHT             = 16                        ; tile height in pixels
FONT_PAGES              = 8                         ; 128 chars × 16 bytes = 2048 = 8 × 256
SCREEN_ROWS             = 30                        ; visible rows  (480 ÷ 16)
SCREEN_TILEBASE_REG     = <(CHARSET_ADDR>>9) | 2    ; L1_TILEBASE: bit1=1 → 16px tile height
.endif
.endif

; ============================================================================
; Viewport and tilemap dimensions
; ============================================================================

MAP_COLS                = 128                                  ; tilemap width  in cells (256 bytes/row stride)
MAP_ROWS                = 64                                   ; tilemap height in cells
SCREEN_COLS_VIEW        = SCREEN_COLS                          ; current viewport width in characters
SCREEN_ROWS_VIEW        = SCREEN_ROWS                          ; current viewport height in characters
TEXT_COLOR              = $61                                  ; default text colour byte (fg/bg nibbles)
INPUT_LINE_MAX          = SCREEN_COLS_VIEW * INPUT_LINE_ROWS   ; max chars in one logical input line
BELL_COL                = 5                                    ; trigger bell click 5 chars before EOL of last row

; ============================================================================
; Display composer initialisation values (640×480 VGA active area)
; ============================================================================

DC_HSTART_VAL           = $00                       ; HSTART: active area starts at pixel 0
DC_HSTOP_VAL            = $A0                       ; HSTOP:  active area ends   at pixel 640
DC_VSTART_VAL           = $00                       ; VSTART: active area starts at line 0
DC_VSTOP_VAL            = $F0                       ; VSTOP:  active area ends   at line 480

; ============================================================================
; VBI and timing constants
; ============================================================================

mSETVBV                 = $E45C                     ; OS ROM SETVBV entry point
VBI_RATE                = 10                        ; metronome tick period  (frames)
VBI_FREQ                = $08                       ; POKEY click frequency  (AUDF4)
VBI_VOLUME              = $AF                       ; POKEY click volume+distortion (AUDC4: poly+noise, vol=max)
CLICK_DURATION          = 1                         ; click length in VBI frames (~16 ms at 60 Hz)

; ============================================================================
; VCTL block offsets — must stay in sync with vera_stub.s and vera_sys_loader.s
; ============================================================================

VERACTL_FLAGS           = 4                         ; driver state flags byte
VERACTL_REQUEST         = 5                         ; API request opcode
VERACTL_PARAM0          = 6                         ; API parameter 0 (character or command)
VERACTL_PARAM1          = 7                         ; API parameter 1
VERACTL_CURSOR_X        = 8                         ; cursor column (0-based)
VERACTL_CURSOR_Y        = 9                         ; cursor row    (0-based)
VERACTL_SCREEN_COLS     = 16                        ; viewport width  in chars (set by loader)
VERACTL_SCREEN_ROWS     = 17                        ; viewport height in chars (set by loader)

VCTL_FLAG_METRONOME     = $01                       ; metronome tick enabled
VCTL_FLAG_ESCAPE        = $02                       ; next byte treated as literal (ATASCII ESC)
VCTL_FLAG_API_READY     = $80                       ; driver fully initialised and ready

VERA_CTL_FLAG_METRONOME = VCTL_FLAG_METRONOME       ; alias
VERA_CTL_FLAG_API_READY = VCTL_FLAG_API_READY       ; alias

VERA_REQ_PUTC           = $03                       ; VCTL_REQUEST opcode: write character
VERA_REQ_GETC           = $04                       ; VCTL_REQUEST opcode: read character (non-blocking)
VERA_REQ_FLUSH_KBD      = $05                       ; VCTL_REQUEST opcode: flush ring + cancel repeat

; ============================================================================
; Atari OS addresses used by the driver
; ============================================================================

ROWCRS_OS               = $54                       ; OS cursor row    (ZP shadow, E:/S:)
COLCRS_OS               = $55                       ; OS cursor column (ZP shadow, low byte only)
HATABS_SIZE             = 99                        ; size in bytes of the HATABS table
READY_ROW               = 5                         ; screen row where warm_reinit prints banner

; EXPORTS table offset for kbd IRQ handler — must stay in sync with vera_stub.s.
EXP_KBD_HANDLER         = 22                        ; _vera_kbd_irq_handler address in EXPORTS

; Keyboard repeat timing — written by the driver at init; user can POKE to override.
; KRPDEL ($02D9) and KEYREP ($02DA) are XL/XE OS RAM variables (defined in atari.inc).
; At 60 Hz: KBD_KRPDEL_FAST = 24 frames ≈ 0.4 s, KBD_KEYREP_FAST = 3 frames ≈ 0.05 s (20 cps)
; OS defaults:                 $30 = 48 frames ≈ 0.8 s,              $06 = 6 frames ≈ 0.1 s
KBD_KRPDEL_FAST         = $18                       ; initial delay before autorepeat starts
KBD_KEYREP_FAST         = $03                       ; interval between repeated keystrokes

; ============================================================================
; ATASCII control codes
; ============================================================================

ATASCII_ESC             = $1B                       ; escape — treat next char as literal
ATASCII_CURSOR_UP       = $1C                       ; move cursor up one row
ATASCII_CURSOR_DOWN     = $1D                       ; move cursor down one row
ATASCII_CURSOR_LEFT     = $1E                       ; move cursor left one column
ATASCII_CURSOR_RIGHT    = $1F                       ; move cursor right one column
ATASCII_CLEAR           = $7D                       ; clear screen and home cursor
ATASCII_BACKSPACE       = $7E                       ; delete character to the left
ATASCII_TAB             = $7F                       ; advance to next tab stop
ATASCII_EOL             = $9B                       ; end of line (carriage return)
ATASCII_DELETE_LINE     = $9C                       ; delete line, scroll lines below up
ATASCII_INSERT_LINE     = $9D                       ; insert blank line, scroll lines below down
ATASCII_BELL            = $FD                       ; audible bell
ATASCII_DELETE_CHAR     = $FE                       ; delete char under cursor, shift row left
ATASCII_INSERT_CHAR     = $FF                       ; insert blank char, shift row right
