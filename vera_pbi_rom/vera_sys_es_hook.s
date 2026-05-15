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

; ============================================================================
; VCTL routing
; ============================================================================

VERA_CTL_REQUEST = _vera_ctl_block + 5
VERA_CTL_PARAM0  = _vera_ctl_block + 6
VERA_REQ_PUTC    = $03

CRITIC           = $42

; ============================================================================
; HATABS layout (33 entries × 3 bytes)
; ============================================================================

HATABS           = $031A
HATABS_SIZE      = 99
OPEN_BYTE_OFFSET = 0                ; offset of OPEN vector in handler table
PUT_BYTE_OFFSET  = 6                ; offset of PUT BYTE vector in handler table

LMARGIN          = $52              ; Atari OS left margin
RMARGIN          = $53              ; Atari OS right margin

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

    .segment "DATA"

; Pre-computed handler addresses (addr - 1) stored as .word so the relocator
; patches them as 16-bit pointers, dodging the #</#> immediate-byte trap.
vera_editor_put_minus1:     .word vera_editor_put - 1
vera_screen_put_minus1:     .word vera_screen_put - 1
vera_editor_open_minus1:    .word vera_editor_open - 1
vera_screen_open_minus1:    .word vera_screen_open - 1

; Addresses of our local vector tables (for HATABS redirection).
vera_editrv_addr:           .word _vera_editrv
vera_screnv_addr:           .word _vera_screnv

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
    sta LMARGIN
    lda #79
    sta RMARGIN
    ldy #1
    rts

; ============================================================================
; vera_screen_open — S: OPEN handler, same margin setup as E:.
; ============================================================================

vera_screen_open:
    lda #0
    sta LMARGIN
    lda #79
    sta RMARGIN
    ldy #1
    rts

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
