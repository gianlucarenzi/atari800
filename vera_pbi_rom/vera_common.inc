; vera_common.inc — Shared symbols for VERA PBI ROM and RAM driver.
; All symbol names are padded to 24 characters so the '=' falls at column 25.

; ============================================================================
; PBI hardware interface
; ============================================================================

PBI_ADDR                = $D100             ; VERA PBI base address ($D100–$D11F)
PBI_LATCH               = $D1FF             ; PBI device-select latch

; ============================================================================
; VERA registers (at PBI_ADDR + offset)
; ============================================================================

VERA_ADDR_L             = PBI_ADDR+$00      ; VRAM address bits  7:0 (active port)
VERA_ADDR_M             = PBI_ADDR+$01      ; VRAM address bits 15:8
VERA_ADDR_H             = PBI_ADDR+$02      ; bit[0]=A16, bits[7:4]=increment
VERA_DATA0              = PBI_ADDR+$03      ; VRAM data port 0
VERA_DATA1              = PBI_ADDR+$04      ; VRAM data port 1
VERA_CTRL_REG           = PBI_ADDR+$05      ; ADDRSEL(0) DCSEL(1) RESET(7)
VERA_CTRL               = VERA_CTRL_REG     ; alias
VERA_IEN                = PBI_ADDR+$06      ; interrupt enable register
VERA_ISR                = PBI_ADDR+$07      ; interrupt status (write 1 to clear)

; ============================================================================
; VERA display composer — DCSEL=0 (output mode, scaling, border)
; ============================================================================

VERA_DC_VIDEO           = PBI_ADDR+$09      ; output mode, layer/sprite enable
VERA_DC_HSCALE          = PBI_ADDR+$0A      ; horizontal scale (128 = 1:1)
VERA_DC_VSCALE          = PBI_ADDR+$0B      ; vertical scale   (128 = 1:1)
VERA_DC_BORDER          = PBI_ADDR+$0C      ; border colour index

; ============================================================================
; VERA display composer — DCSEL=1 (active area clipping)
; ============================================================================

VERA_DC_HSTART          = PBI_ADDR+$09      ; active area start column (/4)
VERA_DC_HSTOP           = PBI_ADDR+$0A      ; active area stop  column (/4)
VERA_DC_VSTART          = PBI_ADDR+$0B      ; active area start row    (/2)
VERA_DC_VSTOP           = PBI_ADDR+$0C      ; active area stop  row    (/2)

; ============================================================================
; VERA layer 1 registers
; ============================================================================

VERA_L1_CONFIG          = PBI_ADDR+$14      ; map mode and colour depth
VERA_L1_MAPBASE         = PBI_ADDR+$15      ; tilemap base (VRAM addr / 512)
VERA_L1_TILEBASE        = PBI_ADDR+$16      ; glyph base   (VRAM addr / 512)
VERA_L1_HSCR_L          = PBI_ADDR+$17      ; horizontal scroll low  byte
VERA_L1_HSCR_H          = PBI_ADDR+$18      ; horizontal scroll high byte
VERA_L1_VSCR_L          = PBI_ADDR+$19      ; vertical scroll low  byte
VERA_L1_VSCR_H          = PBI_ADDR+$1A      ; vertical scroll high byte

; ============================================================================
; VERA control constants and bitmasks
; ============================================================================

VERA_REG_ARRAY          = PBI_ADDR+$00      ; alias for register file base
VERA_ADDRSEL_CLEAR      = $FE               ; AND mask: clear ADDRSEL bit in CTRL
DEVICE_ID_MASK          = $80               ; PBI card-ID bit (bit 7 of device byte)

VERA_INC0               = $00               ; ADDR_H: no auto-increment
VERA_INC1               = $10               ; ADDR_H: auto-increment by 1

VERA_DCSEL0             = $00               ; CTRL: select display composer bank 0
VERA_DCSEL1             = $02               ; CTRL: select display composer bank 1

VERA_VIDEO_VGA          = $01               ; DC_VIDEO: VGA output mode (640×480)
VERA_LAYER1_EN          = $20               ; DC_VIDEO: enable layer 1
VERA_MAP_128x64         = $60               ; L1_CONFIG: 128-wide × 64-tall tilemap

; ============================================================================
; VRAM layout — 128 KB address space; text display lives in bank 1
; ============================================================================

SCREEN_ADDR             = $01B000           ; tilemap start in VRAM
CHARSET_ADDR            = $01F000           ; character glyph data in VRAM

SCREEN_MAPBASE          = <(SCREEN_ADDR>>9) ; L1_MAPBASE  register value (addr/512)
SCREEN_TILEBASE         = <(CHARSET_ADDR>>9); L1_TILEBASE register value (addr/512)

SCREEN_ADDR_M           = >SCREEN_ADDR      ; tilemap VRAM address mid  byte
SCREEN_ADDR_BANK        = ^SCREEN_ADDR      ; tilemap VRAM address bank byte
VERA_SCREEN_BASE_M      = SCREEN_ADDR_M     ; alias used in vera_driver.s
VERA_ADDR_H_BASE        = (VERA_INC1|^SCREEN_ADDR)  ; ADDR_H preset for tilemap port

CHARSET_VRAM_L          = <CHARSET_ADDR     ; glyph VRAM address byte 0
CHARSET_VRAM_M          = >CHARSET_ADDR     ; glyph VRAM address byte 1
CHARSET_VRAM_H          = (VERA_INC1|^CHARSET_ADDR) ; glyph ADDR_H with increment

; ============================================================================
; Viewport and tilemap dimensions
; ============================================================================

MAP_COLS                = 128               ; tilemap width  in cells (256 bytes/row stride)
MAP_ROWS                = 64                ; tilemap height in cells
SCREEN_COLS             = 80                ; visible columns (viewport width)
SCREEN_ROWS             = 60                ; visible rows    (viewport height)
SCREEN_COLS_VIEW        = SCREEN_COLS       ; alias (retained for source compatibility)
SCREEN_ROWS_VIEW        = SCREEN_ROWS       ; alias (retained for source compatibility)
TEXT_COLOR              = $61               ; default text colour byte (fg/bg nibbles)

; ============================================================================
; Display composer initialisation values (640×480 VGA active area)
; ============================================================================

DC_HSTART_VAL           = $00               ; HSTART: active area starts at pixel 0
DC_HSTOP_VAL            = $A0               ; HSTOP:  active area ends   at pixel 640
DC_VSTART_VAL           = $00               ; VSTART: active area starts at line 0
DC_VSTOP_VAL            = $F0               ; VSTOP:  active area ends   at line 480

; ============================================================================
; VBI and timing constants
; ============================================================================

mSETVBV                 = $E45C             ; OS ROM SETVBV entry point
VBI_RATE                = 10                ; metronome tick period  (frames)
VBI_CURSOR_RATE         = 20                ; cursor blink period    (frames)
VBI_FREQ                = $08               ; PSG audio tick rate
VBI_VOLUME              = $AF               ; startup audio volume

; ============================================================================
; VCTL block offsets — must stay in sync with vera_stub.s and vera_sys_loader.s
; ============================================================================

VERACTL_FLAGS           = 4                 ; driver state flags byte
VERACTL_REQUEST         = 5                 ; API request opcode
VERACTL_PARAM0          = 6                 ; API parameter 0 (character or command)
VERACTL_PARAM1          = 7                 ; API parameter 1
VERACTL_CURSOR_X        = 8                 ; cursor column (0-based)
VERACTL_CURSOR_Y        = 9                 ; cursor row    (0-based)

VCTL_FLAG_METRONOME     = $01               ; metronome tick enabled
VCTL_FLAG_ESCAPE        = $02               ; next byte treated as literal (ATASCII ESC)
VCTL_FLAG_API_READY     = $80               ; driver fully initialised and ready

VERA_CTL_FLAG_METRONOME = VCTL_FLAG_METRONOME ; alias
VERA_CTL_FLAG_API_READY = VCTL_FLAG_API_READY ; alias

VERA_REQ_PUTC           = $03               ; VCTL_REQUEST opcode: write character

; ============================================================================
; Atari OS addresses used by the driver
; ============================================================================

ROWCRS_OS               = $54               ; OS cursor row    (ZP shadow, E:/S:)
COLCRS_OS               = $55               ; OS cursor column (ZP shadow, low byte only)
HATABS_SIZE             = 99                ; size in bytes of the HATABS table
READY_ROW               = 8                 ; screen row where warm_reinit prints banner

; ============================================================================
; ATASCII control codes
; ============================================================================

ATASCII_ESC             = $1B               ; escape — treat next char as literal
ATASCII_CURSOR_UP       = $1C               ; move cursor up one row
ATASCII_CURSOR_DOWN     = $1D               ; move cursor down one row
ATASCII_CURSOR_LEFT     = $1E               ; move cursor left one column
ATASCII_CURSOR_RIGHT    = $1F               ; move cursor right one column
ATASCII_CLEAR           = $7D               ; clear screen and home cursor
ATASCII_BACKSPACE       = $7E               ; delete character to the left
ATASCII_TAB             = $7F               ; advance to next tab stop
ATASCII_EOL             = $9B               ; end of line (carriage return)
ATASCII_DELETE_LINE     = $9C               ; delete line, scroll lines below up
ATASCII_INSERT_LINE     = $9D               ; insert blank line, scroll lines below down
ATASCII_BELL            = $FD               ; audible bell
ATASCII_DELETE_CHAR     = $FE               ; delete char under cursor, shift row left
ATASCII_INSERT_CHAR     = $FF               ; insert blank char, shift row right
