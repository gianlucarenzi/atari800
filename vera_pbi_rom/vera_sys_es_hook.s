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
    .export _vera_kbd_irq_handler
    .export _vera_kbd_repeat_tick

    .import _CallVeraApiService
    .import _vera_ctl_block
    .import _InitVbi
    .import _vera_trigger_click
    .import _vera_cursor_invalidate
    .import __VERA_EXPORTS__


    .include "vera_common.inc"
    .include "atari.inc"

; Offset of _vbi_handler address within __VERA_EXPORTS__ — must match vera_sys_vbi.s.
EXP_VBI_HANDLER = 10
    
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
input_buf:              .res INPUT_LINE_MAX + 1 ; max chars + $9B terminator
input_rd:               .res 1                  ; read index (returned to caller)
input_ready:            .res 1                  ; $FF = buffer has data, $00 = need input
caps_lock_state:        .res 1                  ; $FF = CAPS active, $00 = inactive
input_start_row:        .res 1                  ; CURSOR_Y when input started
input_on_row2:          .res 1                  ; $FF = logical line extends to input_start_row+1
input_full:             .res 1                  ; $FF = INPUT_LINE_MAX chars typed, only RETURN/BS allowed

; POKEY keyboard IRQ ring buffer — holds translated ATASCII chars, 16 slots.
; Indices wrap modulo 16 (and #$0F). Full: (wr+1)&$0F == rd. Empty: wr == rd.
kbd_ring_buf:           .res 16
kbd_ring_wr:            .res 1      ; write index — updated only by IRQ handler
kbd_ring_rd:            .res 1      ; read index  — updated only by GET handler

; Key-repeat state — maintained by _vera_kbd_irq_handler / _vera_kbd_repeat_tick.
kbd_repeat_raw:         .res 1      ; raw KBCODE of last pressed key ($FF = none)
kbd_repeat_cnt:         .res 1      ; countdown to next repeat event

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
; _vera_kbd_irq_handler — POKEY keyboard IRQ (VKEYBD vector).
;
; Called by the OS IRQ dispatcher on every new (debounced) keypress.
; Reads raw KBCODE, translates via kbcode_table, applies CAPS LOCK, then
; pushes the resulting ATASCII char into kbd_ring_buf (if not full).
; Also resets the key-repeat countdown.
; Does NOT call the original VKEYBD handler — we own the keyboard pipeline.
; Interrupts are disabled by the CPU for the duration (standard 6502 IRQ).
; ============================================================================

_vera_kbd_irq_handler:
    ; OS IRQ handler did: pha (save A), then jmp (vkeybd).
    ; Stack on entry: [A_saved, P_irq, PC_lo, PC_hi, ...]
    ; X and Y are NOT saved by the OS — we save them here.
    txa
    pha                     ; save X
    tya
    pha                     ; save Y

    lda KBCODE              ; raw: bits 5-0=scan, bit6=SHIFT, bit7=CTRL
    tay                     ; Y = raw code kept for repeat tracking

    lda kbcode_table, y     ; A = ATASCII translation

    ; CAPS toggle keys ($82/$83/$84): flip state, push nothing.
    cmp #$82
    beq @caps_tog
    cmp #$83
    beq @caps_tog
    cmp #$84
    beq @caps_tog
    jmp @apply_caps

@caps_tog:
    lda caps_lock_state
    eor #$FF
    sta caps_lock_state
    jmp @rts_exit

@apply_caps:
    ; Flip case for a-z / A-Z when CAPS is active.
    pha
    and #$DF                ; convert to uppercase for range check
    cmp #'A'
    bcc @no_flip
    cmp #'Z' + 1
    bcs @no_flip
    bit caps_lock_state
    bpl @no_flip
    pla
    eor #$20                ; swap case
    jmp @push_char
@no_flip:
    pla

@push_char:
    ; A = final char, Y = raw KBCODE. X free (saved on stack at entry).
    tax                         ; X = char — preserve before full-check clobbers A
    lda kbd_ring_wr
    clc
    adc #1
    and #$0F                    ; A = (wr+1)&$0F
    cmp kbd_ring_rd
    beq @rts_exit               ; buffer full — drop key

    pha                         ; save new wr
    txa                         ; A = char
    ldx kbd_ring_wr
    sta kbd_ring_buf, x         ; store char at current slot
    pla                         ; A = new wr
    sta kbd_ring_wr             ; advance wr

    ; Prime repeat: save raw code and load initial delay.
    sty kbd_repeat_raw
    lda KRPDEL
    sta kbd_repeat_cnt

@rts_exit:
    pla
    tay                     ; restore Y
    pla
    tax                     ; restore X
    pla                     ; restore A (the one OS pushed before jmp (vkeybd))
    rti


; ============================================================================
; _install_kbd_irq — point VKEYBD at our handler using the EXPORTS table
; (direct label refs would generate 2-byte immediates that the relocator
; cannot patch; EXPORTS uses 3-byte absolute addressing).
; Safe to call multiple times (idempotent).
; ============================================================================

_install_kbd_irq:
    lda __VERA_EXPORTS__ + EXP_KBD_HANDLER
    sta VKEYBD
    lda __VERA_EXPORTS__ + EXP_KBD_HANDLER + 1
    sta VKEYBD + 1
    rts


; ============================================================================
; _vera_kbd_repeat_tick — called from the deferred VBI every frame.
;
; If kbd_repeat_raw is not KEY_NONE and SKSTAT bit 2 shows a key still
; pressed with the same raw code, decrements kbd_repeat_cnt. When the
; counter reaches zero, pushes a repeat event to kbd_ring_buf and resets
; to KEYREP interval.
;
; Uses SEI/CLI around the ring push because the deferred VBI runs with
; interrupts enabled — the keyboard IRQ could otherwise race on kbd_ring_wr.
; Clobbers A, X, Y (caller — the VBI handler — already saves them).
; ============================================================================

_vera_kbd_repeat_tick:
    lda kbd_repeat_raw
    cmp #KEY_NONE           ; $FF means no key tracked
    beq @done

    ; Is a key currently pressed? SKSTAT bit 2 = 0 → yes (active low).
    lda SKSTAT
    and #$04
    bne @key_gone           ; bit 2 set → no key held → cancel repeat

    ; Still the same physical key?
    lda KBCODE
    cmp kbd_repeat_raw
    bne @key_gone

    ; Decrement repeat counter.
    dec kbd_repeat_cnt
    bne @done

    ; Counter expired — push a repeat char.
    tay                     ; Y = current raw KBCODE
    lda kbcode_table, y

    ; Apply CAPS LOCK for repeat char too.
    pha
    and #$DF
    cmp #'A'
    bcc @no_caps_rep
    cmp #'Z' + 1
    bcs @no_caps_rep
    bit caps_lock_state
    bpl @no_caps_rep
    pla
    eor #$20
    jmp @push_rep
@no_caps_rep:
    pla

@push_rep:
    ; A = repeat char. Protect ring write against concurrent keyboard IRQ.
    sei
    tax                         ; X = char — preserve before full-check clobbers A
    lda kbd_ring_wr
    clc
    adc #1
    and #$0F                    ; A = (wr+1)&$0F
    cmp kbd_ring_rd
    beq @full_rep               ; buffer full — skip push but still reset counter
    pha                         ; save new wr
    txa                         ; A = char
    ldx kbd_ring_wr
    sta kbd_ring_buf, x         ; store char at current slot
    pla                         ; A = new wr
    sta kbd_ring_wr             ; advance wr
@full_rep:
    cli

    ; Reset to inter-key repeat interval.
    lda KEYREP
    sta kbd_repeat_cnt
    rts

@key_gone:
    lda #KEY_NONE
    sta kbd_repeat_raw
@done:
    rts


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
; ensure_vbi — called at the start of every PUT BYTE handler.
; Compares VVBLKD against our relocated handler address in the EXPORTS table.
; If they differ (e.g. DOS replaced the vector and exited via DOSVEC without
; going through DOSINI/CASINI), reinstalls the deferred VBI handler.
; Clobbers A; preserves X.
; ============================================================================

ensure_vbi:
    lda VVBLKD
    cmp __VERA_EXPORTS__+EXP_VBI_HANDLER
    bne @reinstall
    lda VVBLKD+1
    cmp __VERA_EXPORTS__+EXP_VBI_HANDLER+1
    beq @ok
@reinstall:
    sei
    lda NMIEN
    pha
    and #$BF                ; disable NMI VBI (bit 6)
    sta NMIEN
    jsr _InitVbi            ; reinstalls VVBLKD; does cli at end
    pla
    sta NMIEN               ; restore NMI enables
@ok:
    rts


; ============================================================================
; vera_editor_put — E: PUT BYTE. VERA is the primary display; the OS handler
; is NOT called.
; Entry: A = char, X = IOCB index * 16.
; Exit:  Y = 1 (success), CRITIC cleared.
; ============================================================================

vera_editor_put:
    sta VERA_CTL_PARAM0
    jsr ensure_vbi
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
    jsr ensure_vbi
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
    sei
    lda NMIEN
    pha                         ; save current NMI enables
    and #$BF                    ; disable NMI VBI (bit 6)
    sta NMIEN
    jsr _InitVbi                ; reinstalls deferred VBI; also does cli
    pla
    sta NMIEN                   ; restore NMI enables
    lda #0
    sta LMARGN
    lda #SCREEN_COLS_VIEW - 1
    sta RMARGN
    ldy #1
    rts

; ============================================================================
; vera_screen_open — S: OPEN handler, same margin setup as E:.
; ============================================================================

vera_screen_open:
    lda #0
    sta LMARGN
    lda #SCREEN_COLS_VIEW - 1
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
    lda #0
    sta input_rd
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sta input_start_row
    lda #0
    sta input_on_row2
    sta input_full

@key_loop:
    ; Allow deferred VBI (cursor blink + key repeat) while waiting.
    lda #0
    sta CRITIC

@poll:
    ; Spin until the POKEY IRQ handler deposits a char in the ring buffer.
    lda kbd_ring_rd
    cmp kbd_ring_wr
    beq @poll               ; ring empty — wait

    ; Drain one char from the ring buffer.
    tax                     ; X = read index
    lda kbd_ring_buf, x     ; A = char
    tay                     ; Y = char (save before advancing rd)
    inx
    txa
    and #$0F
    sta kbd_ring_rd         ; advance read index
    tya                     ; A = char (restore)

    ; A = final ATASCII char — dispatch on special codes.
    cmp #ATASCII_EOL
    beq @got_return
    cmp #ATASCII_BACKSPACE
    beq @got_backspace

    ; Cursor-movement codes $1C-$1F: move VERA cursor only, no input tracking.
    cmp #ATASCII_CURSOR_UP
    bcc @store_char
    cmp #ATASCII_CURSOR_RIGHT+1
    bcs @store_char
    jsr echo_to_vera
    jmp @key_loop

@store_char:
    ; If buffer full, suppress printable chars (only RETURN/BS allowed).
    pha                     ; save char — input_full check would clobber A
    lda input_full
    beq @store_ok
    pla                     ; discard suppressed char
    jmp @key_loop
@store_ok:
    pla                     ; restore char
    jsr echo_to_vera        ; A = char — writes to VERA, advances cursor

    ; After echo, check if cursor wrapped onto a new row.
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    sec
    sbc input_start_row     ; A = rows advanced (0 = still on row 1)
    beq @no_row_change      ; still on row 1, nothing more to do

    cmp #1
    beq @now_on_row2        ; wrapped normally to row 2

    ; Wrapped past row 2 (cr_lf fired on row input_start_row+1).
    ; Pin cursor to last cell of row 2 and mark buffer full.
    lda input_start_row
    clc
    adc #1
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS              ; keep OS shadow in sync or VBI cursor_tick reverts us
    lda #(SCREEN_COLS_VIEW - 1)
    sta _vera_ctl_block + VERACTL_CURSOR_X
    sta COLCRS
    lda #$FF
    sta input_full
    jmp @key_loop

@now_on_row2:
    lda #$FF
    sta input_on_row2
@no_row_change:
    jmp @key_loop

@got_backspace:
    jsr _vera_cursor_invalidate     ; erase cursor tile BEFORE shifting VRAM
    jsr do_logical_backspace
    jmp @key_loop

@got_return:
    ; Screen-editor model: read logical line from VRAM (1 or 2 physical rows).
    lda #1
    sta CRITIC
    lda #$00
    sta VERA_CTRL
    ; Read row input_start_row into input_buf[0..79].
    lda #0
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    ldx #0
@vram_read1:
    lda VERA_DATA0          ; char
    sta input_buf, x
    lda VERA_DATA0          ; color (discard)
    inx
    cpx #SCREEN_COLS_VIEW
    bcc @vram_read1
    ; If logical line spans row 2, read it into input_buf[80..159].
    lda input_on_row2
    beq @strip
    lda #0
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
@vram_read2:
    lda VERA_DATA0
    sta input_buf, x
    lda VERA_DATA0
    inx
    cpx #(SCREEN_COLS_VIEW * INPUT_LINE_ROWS)
    bcc @vram_read2
@strip:
    lda #0
    sta CRITIC
    ; Strip trailing spaces and null tiles from the right.
    dex                     ; X = last index written
@strip_trail:
    lda input_buf, x
    beq @strip_more
    cmp #' '
    bne @found_end
@strip_more:
    dex
    bpl @strip_trail
    inx                     ; X underflowed → empty line → index 0
    jmp @write_eol
@found_end:
    inx
@write_eol:
    lda #ATASCII_EOL
    sta input_buf, x
    ; Echo RETURN to advance cursor to next row on screen.
    lda #ATASCII_EOL
    jsr echo_to_vera
    ; Mark buffer ready and return first char.
    lda #$FF
    sta input_ready
    lda #0
    sta input_rd
    jmp vera_editor_get



; ============================================================================
; do_logical_backspace — 2-row aware destructive backspace with horizontal
; shift. Moves the cursor one logical position back (with cross-row wrap),
; then shifts all characters from the new cursor position+1 to the end of
; the logical line leftward by one cell, blanking the last position.
;
; Clobbers A, X, Y. Preserves nothing (called only from @key_loop).
; ============================================================================

do_logical_backspace:
    ; Determine the logical position before cursor: same row or cross-row wrap.
    lda _vera_ctl_block + VERACTL_CURSOR_X
    cmp LMARGN
    bne @bs_same_row

    ; cursor_x == LMARGN: can we cross to row 1?
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp input_start_row
    beq @bs_nop             ; already at very start of logical line

    ; Cross-row: move cursor to (input_start_row, SCREEN_COLS_VIEW-1).
    lda input_start_row
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    sta ROWCRS              ; keep OS shadow in sync or VBI cursor_tick reverts us
    lda #(SCREEN_COLS_VIEW - 1)
    sta _vera_ctl_block + VERACTL_CURSOR_X
    sta COLCRS
    lda #0
    sta input_full          ; no longer full after a deletion
    jsr bs_shift_and_blank
    rts

@bs_same_row:
    dec _vera_ctl_block + VERACTL_CURSOR_X
    lda _vera_ctl_block + VERACTL_CURSOR_X
    sta COLCRS              ; keep OS shadow in sync
    lda #0
    sta input_full
    jsr bs_shift_and_blank
    rts

@bs_nop:
    rts


; ============================================================================
; bs_shift_and_blank — starting from (CURSOR_X, CURSOR_Y), shift all cells
; of the logical line one position to the left, then blank the last cell.
;
; Cases:
;   cursor on row 1 (cursor_y == input_start_row):
;     shift row1[cursor_x+1..79] → row1[cursor_x..78]
;     if input_on_row2:
;       carry row2[0] → row1[79]
;       shift row2[1..79] → row2[0..78]
;       blank row2[79]
;     else:
;       blank row1[79]
;
;   cursor on row 2 (cursor_y == input_start_row+1):
;     shift row2[cursor_x+1..79] → row2[cursor_x..78]
;     blank row2[79]
;
; Uses DATA0 (source) and DATA1 (destination) for streaming.
; Clobbers A, X, Y.
; ============================================================================

bs_shift_and_blank:
    lda #1
    sta CRITIC
    lda _vera_ctl_block + VERACTL_CURSOR_Y
    cmp input_start_row
    beq @bs_row1_entry
    jmp @on_row2
@bs_row1_entry:

    ; ---- Cursor on row 1 ----
    ; Count of cells to shift on row 1: (SCREEN_COLS_VIEW-1 - cursor_x).
    lda #(SCREEN_COLS_VIEW - 1)
    sec
    sbc _vera_ctl_block + VERACTL_CURSOR_X
    beq @row1_done          ; cursor at col 79: skip row1 shift

    ; DATA1 = dest: (input_start_row, cursor_x).
    lda #$01
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; DATA0 = src: (input_start_row, cursor_x+1).
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    clc
    adc #1
    asl a
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; Stream (SCREEN_COLS_VIEW-1-cursor_x) cells × 2 bytes.
    lda #(SCREEN_COLS_VIEW - 1)
    sec
    sbc _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    tay
@r1_shift:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @r1_shift

@row1_done:
    lda input_on_row2
    bne @bs_has_row2
    jmp @blank_row1_end
@bs_has_row2:

    ; Carry row2[0] → row1[79]: bridge the row boundary.
    ; DATA0 = (input_start_row+1, col 0)
    lda #$00
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    lda VERA_DATA0          ; char from row2 col0
    pha
    lda VERA_DATA0          ; color from row2 col0 (advances DATA0 past col0)
    tax                     ; save color

    ; DATA1 = (input_start_row, col 79)
    lda #$01
    sta VERA_CTRL
    lda #((SCREEN_COLS_VIEW - 1) * 2)
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    pla                     ; char
    sta VERA_DATA1
    txa                     ; color
    sta VERA_DATA1

    ; Shift row2[1..79] → row2[0..78].
    ; DATA1 = (input_start_row+1, col 0)
    lda #$01
    sta VERA_CTRL
    lda #0
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; DATA0 = (input_start_row+1, col 1)
    lda #$00
    sta VERA_CTRL
    lda #2
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ldy #((SCREEN_COLS_VIEW - 1) * 2)   ; 79 cells × 2 bytes
@r2_shift:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @r2_shift

    ; Blank row2[79].
    lda #$00
    sta VERA_CTRL
    lda #((SCREEN_COLS_VIEW - 1) * 2)
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #TEXT_COLOR
    sta VERA_DATA0
    lda #0
    sta CRITIC
    rts

@blank_row1_end:
    ; Blank row1[79].
    lda #$00
    sta VERA_CTRL
    lda #((SCREEN_COLS_VIEW - 1) * 2)
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #VERA_SCREEN_BASE_M
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #TEXT_COLOR
    sta VERA_DATA0
    lda #0
    sta CRITIC
    rts

@on_row2:
    ; ---- Cursor on row 2 ----
    ; Shift row2[cursor_x+1..79] → row2[cursor_x..78], blank row2[79].
    lda #(SCREEN_COLS_VIEW - 1)
    sec
    sbc _vera_ctl_block + VERACTL_CURSOR_X
    beq @blank_row2_end     ; cursor at col 79: just blank it

    ; DATA1 = (input_start_row+1, cursor_x)
    lda #$01
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    ; DATA0 = (input_start_row+1, cursor_x+1)
    lda #$00
    sta VERA_CTRL
    lda _vera_ctl_block + VERACTL_CURSOR_X
    clc
    adc #1
    asl a
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H

    lda #(SCREEN_COLS_VIEW - 1)
    sec
    sbc _vera_ctl_block + VERACTL_CURSOR_X
    asl a
    tay
@r2_only_shift:
    lda #$00
    sta VERA_CTRL
    lda VERA_DATA0
    pha
    lda #$01
    sta VERA_CTRL
    pla
    sta VERA_DATA1
    dey
    bne @r2_only_shift

@blank_row2_end:
    lda #$00
    sta VERA_CTRL
    lda #((SCREEN_COLS_VIEW - 1) * 2)
    sta VERA_ADDR_L
    lda input_start_row
    clc
    adc #(VERA_SCREEN_BASE_M + 1)
    sta VERA_ADDR_M
    lda #VERA_ADDR_H_BASE
    sta VERA_ADDR_H
    lda #' '
    sta VERA_DATA0
    lda #TEXT_COLOR
    sta VERA_DATA0
    lda #0
    sta CRITIC
    rts


; ============================================================================
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
    ; Reset GET line-buffer and ring-buffer state.
    lda #0
    sta input_ready
    sta input_rd
    sta input_start_row
    sta input_on_row2
    sta input_full
    sta kbd_ring_wr
    sta kbd_ring_rd
    lda #KEY_NONE
    sta kbd_repeat_raw
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

    ; Install POKEY keyboard IRQ handler (replaces VKEYBD).
    jsr _install_kbd_irq
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

    ; Install our OPEN handler (sets LMARGIN/RMARGIN = 0/SCREEN_COLS_VIEW).
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
