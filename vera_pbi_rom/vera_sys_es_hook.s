; vera_sys_es_hook.s — chained E:/S: PUT BYTE hooks + HATABS installer.
;
; Mirror strategy (Phase 1B):
;   1. install_es_hooks walks HATABS, finds 'E' and 'S' devices
;   2. copies their vector tables into local LOWBSS slots (_vera_editrv,
;      _vera_screnv) so the original handler stays reachable
;   3. saves the original PUT BYTE pointer (addr-1 format) for chaining
;   4. patches the local table's PUT BYTE slot to point at our chained_*
;      hook, then redirects HATABS to the local table
;
; chained_editor_put / chained_screen_put:
;   - run the original PUT BYTE first (ANTIC sees the byte as before)
;   - then mirror the same byte through the VERA putc state machine
;   - preserve the handler's Y return code and CIO calling convention
;
; The chain uses the classic RTS-trampoline: push (continue-1), push the
; saved (orig-1), RTS to original. When original RTSes it lands at the
; continue label, which performs the VERA mirror and returns to CIO.

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
PUT_BYTE_OFFSET  = 6                ; offset inside a handler vector table

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
_vera_orig_editor_put:  .res 2      ; original (addr-1) for E: PUT BYTE
_vera_orig_screen_put:  .res 2      ; original (addr-1) for S: PUT BYTE

; LOWBSS slots used by the DOSINI/CASINI chain (kept here historically because
; bootstrap and dosini.s both reference them via __VERA_EXPORTS__).
_vera_saved_dosini:     .res 2
_vera_saved_casini:     .res 2

; Per-call scratch for the chained hook — survives across the RTS trampoline.
chain_saved_char:       .res 1
chain_saved_y:          .res 1

; ZP backup so we can borrow $CB/$CC while walking HATABS.
save_zp_cb:             .res 1
save_zp_cc:             .res 1

; Scratch for the IOCB-update pass (CMP-against-register without a free reg).
iocb_match_id:          .res 1

    .segment "DATA"

; Pre-computed (chained_*_put - 1) — these are the values we write into
; HATABS PUT BYTE slots. Storing them as .word lets the relocator patch them
; as normal 16-bit pointers, dodging the #</#> immediate-byte trap.
chained_editor_put_minus1:  .word chained_editor_put - 1
chained_screen_put_minus1:  .word chained_screen_put - 1

; Continuation addresses for the RTS-trampoline chain. Same reasoning —
; pushed onto the stack one byte at a time, but stored as a .word so the
; relocator patches them as 16-bit pointers rather than triggering the
; immediate-byte trap on `#</#>`.
after_editor_orig_minus1:   .word after_editor_orig - 1
after_screen_orig_minus1:   .word after_screen_orig - 1

; Same trick for the addresses of our local vector tables.
vera_editrv_addr:           .word _vera_editrv
vera_screnv_addr:           .word _vera_screnv

    .segment "CODE"

; ============================================================================
; _VeraPutByte — legacy direct entry kept for callers that bypass HATABS
; (e.g. an XIO test stub). CIO sets CRITIC=1; we clear it on exit.
; ============================================================================

.proc _VeraPutByte
    pha
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    lda #$00
    sta CRITIC
    pla
    ldy #1
    rts
.endproc

; ============================================================================
; chained_editor_put — E: PUT BYTE hook. Runs original ANTIC handler first,
; then mirrors to VERA.
;
; Entry: A = char, X = IOCB index * 16.
; Exit:  Y = result code from original handler, A preserved, CRITIC cleared.
; ============================================================================

chained_editor_put:
    sta chain_saved_char

    ; Build chain stack so RTS lands on original handler, original's RTS
    ; lands on after_editor_orig. HI/LO of (continuation - 1) come from a
    ; relocatable .word slot to dodge the immediate-byte trap.
    lda after_editor_orig_minus1 + 1
    pha
    lda after_editor_orig_minus1
    pha
    lda _vera_orig_editor_put + 1
    pha
    lda _vera_orig_editor_put
    pha

    lda chain_saved_char            ; restore A for the handler
    rts                              ; → original E: PUT BYTE

after_editor_orig:
    sty chain_saved_y                ; preserve handler's Y result

    ; Mirror byte to VERA.
    lda chain_saved_char
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService

    lda #$00
    sta CRITIC                       ; CIO never clears this for us

    lda chain_saved_char
    ldy chain_saved_y
    rts

; ============================================================================
; chained_screen_put — S: PUT BYTE hook, structurally identical to the E:
; chain.
; ============================================================================

chained_screen_put:
    sta chain_saved_char

    lda after_screen_orig_minus1 + 1
    pha
    lda after_screen_orig_minus1
    pha
    lda _vera_orig_screen_put + 1
    pha
    lda _vera_orig_screen_put
    pha

    lda chain_saved_char
    rts

after_screen_orig:
    sty chain_saved_y

    lda chain_saved_char
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService

    lda #$00
    sta CRITIC

    lda chain_saved_char
    ldy chain_saved_y
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

    ; Capture the original PUT BYTE pointer (addr-1 format).
    lda _vera_editrv + PUT_BYTE_OFFSET
    sta _vera_orig_editor_put
    lda _vera_editrv + PUT_BYTE_OFFSET + 1
    sta _vera_orig_editor_put + 1

    ; Install our hook in the local table's PUT BYTE slot.
    lda chained_editor_put_minus1
    sta _vera_editrv + PUT_BYTE_OFFSET
    lda chained_editor_put_minus1 + 1
    sta _vera_editrv + PUT_BYTE_OFFSET + 1

    ; Redirect HATABS at the local table.
    lda vera_editrv_addr
    sta HATABS+1, x
    lda vera_editrv_addr + 1
    sta HATABS+2, x

    ; Now patch every IOCB whose ICHID matches this HATABS offset — those
    ; were OPENed against the original vector table and cache the original
    ; PUT BYTE pointer in ICPTL/ICPTH. Without this pass, BASIC's IOCB #0
    ; (opened during cart INIT before our bootstrap runs) keeps the old
    ; pointer and PRINT bypasses our hook until a CLOSE+OPEN cycle.
    stx iocb_match_id
    ldy #0
@iocb_loop_e:
    lda IOCB_BASE + IOCB_ICHID, y
    cmp iocb_match_id
    bne @next_iocb_e
    lda chained_editor_put_minus1
    sta IOCB_BASE + IOCB_ICPTL, y
    lda chained_editor_put_minus1 + 1
    sta IOCB_BASE + IOCB_ICPTH, y
@next_iocb_e:
    tya
    clc
    adc #IOCB_STRIDE
    tay
    cpy #(IOCB_STRIDE * IOCB_COUNT)
    bne @iocb_loop_e

    ldx iocb_match_id                ; restore X for the outer scan loop
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

    lda chained_screen_put_minus1
    sta _vera_screnv + PUT_BYTE_OFFSET
    lda chained_screen_put_minus1 + 1
    sta _vera_screnv + PUT_BYTE_OFFSET + 1

    lda vera_screnv_addr
    sta HATABS+1, x
    lda vera_screnv_addr + 1
    sta HATABS+2, x

    ; Refresh cached PUT BYTE pointers in any IOCB already OPEN to S:.
    stx iocb_match_id
    ldy #0
@iocb_loop_s:
    lda IOCB_BASE + IOCB_ICHID, y
    cmp iocb_match_id
    bne @next_iocb_s
    lda chained_screen_put_minus1
    sta IOCB_BASE + IOCB_ICPTL, y
    lda chained_screen_put_minus1 + 1
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
