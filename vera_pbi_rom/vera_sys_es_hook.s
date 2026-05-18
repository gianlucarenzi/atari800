; vera_sys_es_hook.s — E:/S: PUT BYTE replace hooks + HATABS installer.
;
; Primary-display strategy (Phase 2 / XEP80-style):
;   1. install_es_hooks walks HATABS, finds 'E' and 'S' devices
;   2. copies their vector tables into local LOWBSS slots (_vera_editrv,
;      _vera_screnv)
;   3. patches the local table's PUT BYTE slot to point at vera_editor_put /
;      vera_screen_put, and OPEN slot to vera_editor_open / vera_screen_open
;   4. redirects HATABS to the local table
;
; The original OS PUT BYTE handler is NOT called; VERA is the primary display.
; On OPEN, LMARGIN/RMARGIN are set for 80 columns so the OS state machine
; and software (e.g. Atari Writer) see an 80-column device.

    .setcpu "6502"

    .export _vera_editrv, _vera_screnv
    .export _vera_orig_editor_put, _vera_orig_screen_put
    .export _VeraPutByte
    .export _vera_saved_dosini, _vera_saved_casini
    .export _install_es_hooks

    .import _CallVeraApiService
    .import _vera_ctl_block

    .include "vera_common.inc"
    .include "atari.inc"
    
; ============================================================================
; VCTL routing
; ============================================================================

VERA_CTL_REQUEST  = _vera_ctl_block + 5
VERA_CTL_PARAM0   = _vera_ctl_block + 6

; ============================================================================
; HATABS layout (33 entries × 3 bytes)
; ============================================================================

OPEN_BYTE_OFFSET = 0                ; offset of OPEN vector in handler table
GET_BYTE_OFFSET  = 4                ; offset of GET BYTE vector in handler table
PUT_BYTE_OFFSET  = 6                ; offset of PUT BYTE vector in handler table

; Keyboard / system OS equates used by the GET handler.
; NOTE: CH ($02FC) holds the raw POKEY KBCODE: bits 0-5 = key matrix pos,
; bit 6 = SHIFT, bit 7 = CTRL. NOT ATASCII. Translation is done by kbcode_table.
;CH               = $02FC            ; raw key code from keyboard IRQ ($FF = none)
;BRKKEY           = $0011            ; break key: $00 = pressed, else not pressed

; AKEY_ CTRL combos that map to cursor / edit ATASCII codes (bit 7 = CTRL set).
AKEY_UP          = $8E              ; CTRL+MINUS  → cursor up    ($1C)
AKEY_DOWN        = $8F              ; CTRL+EQUAL  → cursor down  ($1D)
AKEY_LEFT        = $86              ; CTRL+PLUS   → cursor left  ($1E)
AKEY_RIGHT       = $87              ; CTRL+ASTER  → cursor right ($1F)
AKEY_DELETE_CHAR = $B4              ; CTRL+BACKSP → delete char  ($FE)
AKEY_INSERT_CHAR = $B7              ; CTRL+>      → insert char  ($FF)

; ============================================================================
; IOCB layout — 8 IOCBs at $0340-$03BF, 16 bytes each. CIO caches the device's
; PUT BYTE pointer in ICPTL/ICPTH at OPEN time, so we must rewrite those
; cached values for every IOCB already open to E:/S: when we install. Without
; this pass, anyone who OPENed before our bootstrap ran keeps the original
; pointer in cache and bypasses our hook.
; ============================================================================

IOCB_BASE        = $0340
IOCB_ICHID       = 0
IOCB_ICPTL       = 6
IOCB_ICPTH       = 7
IOCB_STRIDE      = 16
IOCB_COUNT       = 8

; ============================================================================
; ZP scratch — saved and restored around use so we don't disturb BASIC/DOS.
; ============================================================================

HATABS_PTR       = $CB              ; 2 bytes ($CB/$CC), user-reserved area

    .segment "LOWBSS"

_vera_editrv:           .res 16     ; local copy of E: vector table
_vera_screnv:           .res 16     ; local copy of S: vector table
_vera_orig_editor_put:  .res 2      ; kept for potential future use (not chained)
_vera_orig_screen_put:  .res 2      ; kept for potential future use (not chained)

; LOWBSS slots used by the DOSINI/CASINI chain (kept here historically because
; bootstrap and dosini.s both reference them via __VERA_EXPORTS__).
_vera_saved_dosini:     .res 2
_vera_saved_casini:     .res 2

; ZP backup so we can borrow $CB/$CC while walking HATABS.
save_zp_cb:             .res 1
save_zp_cc:             .res 1

; Scratch for the IOCB-update pass (CMP-against-register without a free reg).
iocb_match_id:          .res 1

; GET BYTE line-input state.
input_buf:              .res 81     ; 80 chars + $9B terminator
input_rd:               .res 1      ; read index (returned to caller)
input_wr:               .res 1      ; write index (built during keyboard input)
input_ready:            .res 1      ; $FF = buffer has data, $00 = need input
input_col0:             .res 1      ; cursor X at start of input (BS clamp)
caps_lock_state:        .res 1      ; $FF = CAPS active, $00 = inactive

    .segment "DATA"

; Pre-computed handler addresses (addr - 1) stored as .word so the relocator
; patches them as 16-bit pointers, dodging the #</#> immediate-byte trap.
vera_editor_put_minus1:     .word vera_editor_put - 1
vera_screen_put_minus1:     .word vera_screen_put - 1
vera_editor_open_minus1:    .word vera_editor_open - 1
vera_screen_open_minus1:    .word vera_screen_open - 1
vera_editor_get_minus1:     .word vera_editor_get - 1

; Addresses of our local vector tables (for HATABS redirection).
vera_editrv_addr:           .word _vera_editrv
vera_screnv_addr:           .word _vera_screnv

; Keyboard translation table: raw KBCODE -> ATASCII.
; Block 0: Unshifted, Block 1: SHIFT, Block 2: CTRL, Block 3: CTRL+SHIFT.
kbcode_table:
    ; Lowercase (Unshifted)
    .byte $6C, $6A, $3B, $80, $80, $6B, $2B, $2A    ; L   J   ;:  F1  F2  K   +\  *^
    .byte $6F, $80, $70, $75, $9B, $69, $2D, $3D    ; O       P   U   Ret I   -_  =|
    .byte $76, $80, $63, $80, $80, $62, $78, $7A    ; V   Hlp C   F3  F4  B   X   Z
    .byte $34, $80, $33, $36, $1B, $35, $32, $31    ; 4$      3#  6&  Esc 5%  2"  1!
    .byte $2C, $20, $2E, $6E, $80, $6D, $2F, $81    ; ,[  Spc .]  N       M   /?  Inv
    .byte $72, $80, $65, $79, $7F, $74, $77, $71    ; R       E   Y   Tab T   W   Q
    .byte $39, $80, $30, $37, $7E, $38, $3C, $3E    ; 9(      0)  7'  Bks 8@  <   >
    .byte $66, $68, $64, $80, $82, $67, $73, $61    ; F   H   D       Cps G   S   A

    ; SHIFT
    .byte $4C, $4A, $3A, $80, $80, $4B, $5C, $5E    ; L   J   ;:  F1  F2  K   +\  *^
    .byte $4F, $80, $50, $55, $9B, $49, $5F, $7C    ; O       P   U   Ret I   -_  =|
    .byte $56, $80, $43, $80, $80, $42, $58, $5A    ; V   Hlp C   F3  F4  B   X   Z
    .byte $24, $80, $23, $26, $1B, $25, $22, $21    ; 4$      3#  6&  Esc 5%  2"  1!
    .byte $5B, $20, $5D, $4E, $80, $4D, $3F, $80    ; ,[  Spc .]  N       M   /?  Inv
    .byte $52, $80, $45, $59, $9F, $54, $57, $51    ; R       E   Y   Tab T   W   Q
    .byte $28, $80, $29, $27, $9C, $40, $7D, $9D    ; 9(      0)  7'  Bks 8@  <   >
    .byte $46, $48, $44, $80, $83, $47, $53, $41    ; F   H   D       Cps G   S   A

    ; CTRL
    .byte $0C, $0A, $7B, $80, $80, $0B, $1E, $1F    ; L   J   ;:  F1  F2  K   +\  *^
    .byte $0F, $80, $10, $15, $9B, $09, $1C, $1D    ; O       P   U   Ret I   -_  =|
    .byte $16, $80, $03, $80, $80, $02, $18, $1A    ; V   Hlp C   F3  F4  B   X   Z
    .byte $80, $80, $85, $80, $1B, $80, $FD, $80    ; 4$      3#  6&  Esc 5%  2"  1!
    .byte $00, $20, $60, $0E, $80, $0D, $80, $80    ; ,[  Spc .]  N       M   /?  Inv
    .byte $12, $80, $05, $19, $9E, $14, $17, $11    ; R       E   Y   Tab T   W   Q
    .byte $80, $80, $80, $80, $FE, $80, $7D, $FF    ; 9(      0)  7'  Bks 8@  <   >
    .byte $06, $08, $04, $80, $84, $07, $13, $01    ; F   H   D       Cps G   S   A

    ; CTRL+SHIFT (maps to CTRL)
    .byte $0C, $0A, $7B, $80, $80, $0B, $1E, $1F
    .byte $0F, $80, $10, $15, $9B, $09, $1C, $1D
    .byte $16, $80, $03, $80, $80, $02, $18, $1A
    .byte $80, $80, $85, $80, $1B, $80, $FD, $80
    .byte $00, $20, $60, $0E, $80, $0D, $80, $80
    .byte $12, $80, $05, $19, $9E, $14, $17, $11
    .byte $80, $80, $80, $80, $FE, $80, $7D, $FF
    .byte $06, $08, $04, $80, $84, $07, $13, $01

    .segment "CODE"

; ============================================================================
; _VeraPutByte — direct entry kept for callers that bypass HATABS.
; Entry: A = char. Exit: Y = 1 (success), CRITIC cleared.
; ============================================================================

.proc _VeraPutByte
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    lda #$00
    sta CRITIC
    ldy #1
    rts
.endproc

; ============================================================================
; vera_editor_put — E: PUT BYTE. VERA is the primary display; the OS handler
; is NOT called.
; Entry: A = char, X = IOCB index * 16.
; Exit:  Y = 1 (success), CRITIC cleared.
; ============================================================================

vera_editor_put:
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    lda #$00
    sta CRITIC
    ldy #1
    rts

; ============================================================================
; vera_screen_put — S: PUT BYTE, identical to vera_editor_put.
; ============================================================================

vera_screen_put:
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    lda #$00
    sta CRITIC
    ldy #1
    rts

; ============================================================================
; vera_editor_open — E: OPEN handler. Sets LMARGIN/RMARGIN for 80 columns so
; software that reads these OS variables (e.g. Atari Writer) sees 80 cols.
; Entry: A/X as per CIO convention. Exit: Y = 1 (success).
; ============================================================================

vera_editor_open:
    lda #0
    sta LMARGN
    lda #79
    sta RMARGN
    ldy #1
    rts

; ============================================================================
; vera_screen_open — S: OPEN handler, same margin setup as E:.
; ============================================================================

vera_screen_open:
    lda #0
    sta LMARGN
    lda #79
    sta RMARGN
    ldy #1
    rts

; ============================================================================
; vera_editor_get — E: GET BYTE handler. One call = one ATASCII byte.
;
; If the internal line buffer has data (input_ready=$FF), returns the next
; byte and advances input_rd. When the $9B EOL is returned, clears the buffer.
;
; If the buffer is empty (input_ready=$00), enters keyboard input mode:
;   - polls CH ($02FC) for each keystroke
;   - echoes printable chars and control codes to VERA in real time
;   - handles BACKSPACE ($7E): erases last char from buffer and VERA
;   - on RETURN ($9B): terminates buffer, echoes newline, returns first char
;   - on BREAK (BRKKEY=$00): returns CIO break error (Y=$80)
;
; Entry: X = IOCB index * 16 (unused).
; Exit:  A = character, Y = 1 (success) or Y = $80 (break error).
; ============================================================================

vera_editor_get:
    lda input_ready
    beq @need_input

    ; Buffer has data: return next byte.
    ldy input_rd
    lda input_buf, y
    inc input_rd
    cmp #ATASCII_EOL
    bne @got_char
    lda #0
    sta input_ready         ; EOL returned → buffer exhausted
    lda #ATASCII_EOL        ; restore A (sta clobbered flags only, lda needed)
@got_char:
    ldy #1
    rts

@need_input:
    ; Save cursor column at input start for BACKSPACE clamping.
    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta input_col0
    lda #0
    sta input_wr
    sta input_rd

@key_loop:
    ; Allow deferred VBI (cursor blink) while waiting for keystroke.
    lda #0
    sta CRITIC

@poll:
    lda CH                  ; $02FC: $FF = no key
    cmp #$FF
    beq @poll

    ; Key available: consume it, then translate raw KBCODE -> ATASCII via kbcode_table.
    ; CH holds kbcode | (SHIFT << 6) | (CTRL << 7) — not ATASCII.
    tay                     ; Y = raw code
    lda #$FF
    sta CH                  ; consume keypress
    lda kbcode_table, y     ; A = ATASCII translation of raw key code

    ; Handle CAPS toggle
    cmp #$82                ; CAPS
    beq @toggle_caps
    cmp #$83                ; SHIFT + CAPS
    beq @toggle_caps
    cmp #$84                ; CTRL + CAPS
    beq @toggle_caps

    ; Apply CAPS LOCK only for letters 'a'-'z' and 'A'-'Z'
    pha
    and #$DF                ; convert to uppercase for range check
    cmp #'A'
    bcc @no_caps_swap
    cmp #'Z'+1
    bcs @no_caps_swap
    ; It's a letter! Check CAPS state.
    bit caps_lock_state
    bpl @no_caps_swap       ; CAPS is off
    ; CAPS is on: swap case (bit 5)
    pla
    eor #$20
    jmp @done_caps
@no_caps_swap:
    pla
@done_caps:

    cmp #ATASCII_EOL
    beq @got_return
    cmp #ATASCII_BACKSPACE
    beq @got_backspace
    jmp @not_special        ; avoid falling into toggle_caps

@toggle_caps:
    lda caps_lock_state
    eor #$FF
    sta caps_lock_state
    jmp @poll               ; wait for next key

@not_special:

    ; Printable / control char: store in buffer if not full, echo to VERA.
    ldx input_wr
    cpx #80
    bcs @key_loop
    sta input_buf, x
    inc input_wr
    jsr echo_to_vera        ; A = char
    jmp @key_loop

@got_backspace:
    lda input_wr
    beq @key_loop           ; nothing to erase
    ; Prevent backspacing before start of input area.
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp input_col0
    beq @key_loop
    dec input_wr
    lda #ATASCII_BACKSPACE
    jsr echo_to_vera
    jmp @key_loop

@got_return:
    ; Terminate buffer with EOL, echo newline to VERA.
    ldx input_wr
    lda #ATASCII_EOL
    sta input_buf, x
    jsr echo_to_vera        ; A = ATASCII_EOL
    ; Mark buffer ready and re-enter to return first char.
    lda #$FF
    sta input_ready
    lda #0
    sta input_rd
    jmp vera_editor_get     ; tail-call



; echo_to_vera — tail-call helper: write A through the VERA putc state machine.
; Uses jmp so _CallVeraApiService's rts returns directly to our caller.
echo_to_vera:
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jmp _CallVeraApiService


; ============================================================================
; _install_es_hooks — find E: and S: in HATABS, copy their vector tables to
; our LOWBSS slots, redirect HATABS to those local tables with PUT BYTE
; pointing at our chained hooks.
;
; Idempotent: calling again after the OS rebuilds HATABS at warm-start
; re-establishes the hook without leaking the orig pointer (we always
; overwrite from whatever's currently in HATABS).
; ============================================================================

_install_es_hooks:
    ; Reset GET line-buffer state so we never read stale LOWBSS on first call.
    lda #0
    sta input_ready
    sta input_rd
    sta input_wr
    lda #$FF
    sta caps_lock_state

    ; Stash $CB/$CC — these are user-reserved ZP but BASIC FMS touches them.
    lda HATABS_PTR
    sta save_zp_cb
    lda HATABS_PTR+1
    sta save_zp_cc

    ldx #0
@scan:
    lda HATABS,x
    beq @next
    cmp #'E'
    bne @not_e
    jsr install_e
    jmp @next
@not_e:
    cmp #'S'
    bne @next
    jsr install_s
@next:
    inx
    inx
    inx
    cpx #HATABS_SIZE
    bne @scan

    lda save_zp_cb
    sta HATABS_PTR
    lda save_zp_cc
    sta HATABS_PTR+1
    rts

; ----------------------------------------------------------------------------
; install_e — X = byte offset of 'E' entry in HATABS.
; ----------------------------------------------------------------------------

install_e:
    lda HATABS+1, x
    sta HATABS_PTR
    lda HATABS+2, x
    sta HATABS_PTR+1

    ldy #15
@copy:
    lda (HATABS_PTR), y
    sta _vera_editrv, y
    dey
    bpl @copy

    ; Save original PUT BYTE pointer (kept for reference, not used for chaining).
    lda _vera_editrv + PUT_BYTE_OFFSET
    sta _vera_orig_editor_put
    lda _vera_editrv + PUT_BYTE_OFFSET + 1
    sta _vera_orig_editor_put + 1

    ; Install our PUT BYTE handler (replace, not chain).
    lda vera_editor_put_minus1
    sta _vera_editrv + PUT_BYTE_OFFSET
    lda vera_editor_put_minus1 + 1
    sta _vera_editrv + PUT_BYTE_OFFSET + 1

    ; Install our GET BYTE handler (real-time keyboard echo to VERA).
    lda vera_editor_get_minus1
    sta _vera_editrv + GET_BYTE_OFFSET
    lda vera_editor_get_minus1 + 1
    sta _vera_editrv + GET_BYTE_OFFSET + 1

    ; Install our OPEN handler (sets LMARGIN/RMARGIN = 0/79).
    lda vera_editor_open_minus1
    sta _vera_editrv + OPEN_BYTE_OFFSET
    lda vera_editor_open_minus1 + 1
    sta _vera_editrv + OPEN_BYTE_OFFSET + 1

    ; Redirect HATABS at the local table.
    lda vera_editrv_addr
    sta HATABS+1, x
    lda vera_editrv_addr + 1
    sta HATABS+2, x

    ; Patch every IOCB whose ICHID matches this HATABS offset — those were
    ; OPENed before our bootstrap and cache the old PUT BYTE in ICPTL/ICPTH.
    stx iocb_match_id
    ldy #0
@iocb_loop_e:
    lda IOCB_BASE + IOCB_ICHID, y
    cmp iocb_match_id
    bne @next_iocb_e
    lda vera_editor_put_minus1
    sta IOCB_BASE + IOCB_ICPTL, y
    lda vera_editor_put_minus1 + 1
    sta IOCB_BASE + IOCB_ICPTH, y
@next_iocb_e:
    tya
    clc
    adc #IOCB_STRIDE
    tay
    cpy #(IOCB_STRIDE * IOCB_COUNT)
    bne @iocb_loop_e

    ldx iocb_match_id
    rts

; ----------------------------------------------------------------------------
; install_s — same as install_e for the S: device.
; ----------------------------------------------------------------------------

install_s:
    lda HATABS+1, x
    sta HATABS_PTR
    lda HATABS+2, x
    sta HATABS_PTR+1

    ldy #15
@copy:
    lda (HATABS_PTR), y
    sta _vera_screnv, y
    dey
    bpl @copy

    lda _vera_screnv + PUT_BYTE_OFFSET
    sta _vera_orig_screen_put
    lda _vera_screnv + PUT_BYTE_OFFSET + 1
    sta _vera_orig_screen_put + 1

    lda vera_screen_put_minus1
    sta _vera_screnv + PUT_BYTE_OFFSET
    lda vera_screen_put_minus1 + 1
    sta _vera_screnv + PUT_BYTE_OFFSET + 1

    lda vera_screen_open_minus1
    sta _vera_screnv + OPEN_BYTE_OFFSET
    lda vera_screen_open_minus1 + 1
    sta _vera_screnv + OPEN_BYTE_OFFSET + 1

    lda vera_screnv_addr
    sta HATABS+1, x
    lda vera_screnv_addr + 1
    sta HATABS+2, x

    stx iocb_match_id
    ldy #0
@iocb_loop_s:
    lda IOCB_BASE + IOCB_ICHID, y
    cmp iocb_match_id
    bne @next_iocb_s
    lda vera_screen_put_minus1
    sta IOCB_BASE + IOCB_ICPTL, y
    lda vera_screen_put_minus1 + 1
    sta IOCB_BASE + IOCB_ICPTH, y
@next_iocb_s:
    tya
    clc
    adc #IOCB_STRIDE
    tay
    cpy #(IOCB_STRIDE * IOCB_COUNT)
    bne @iocb_loop_s

    ldx iocb_match_id
    rts
