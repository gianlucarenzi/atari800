    .setcpu "6502"

    .export _vera_dosini_asm_hook, _vera_casini_asm_hook
    .import _vera_hw_reinit, _vera_wait_and_clear, _InitVbi
    .import _vera_saved_dosini, _vera_saved_casini
    .import _vera_saved_orig_ramtop, _vera_saved_dest_hi
    .import _install_es_hooks
    .import __VERA_EXPORTS__

    .include "atari.inc"

; Offsets within __VERA_EXPORTS__ — must stay in sync with vera_stub.s.
EXP_DOSINI_HOOK = 2
EXP_CASINI_HOOK = 4

; Re-install our DOSINI/CASINI vectors into OS ZP after every warm reset.
; Uses absolute addressing so the relocator patches __VERA_EXPORTS__ correctly.
re_install_hooks:
    lda __VERA_EXPORTS__+EXP_DOSINI_HOOK
    sta DOSINI
    lda __VERA_EXPORTS__+EXP_DOSINI_HOOK+1
    sta DOSINI+1
    lda __VERA_EXPORTS__+EXP_CASINI_HOOK
    sta CASINI
    lda __VERA_EXPORTS__+EXP_CASINI_HOOK+1
    sta CASINI+1
    rts

; common_reinit — lightweight warm restart sequence.
;
; Called unconditionally from both DOSINI and CASINI hooks before any chaining.
; Must NOT call _vera_warm_reinit (which has a busy-wait + do_clear that
; disables DMACTL for ~46 ms — the cause of ANTIC screen garbage on warm reset).
common_reinit:
    lda #0
    sta CRITIC

    ; Restore RAMTOP (zeroed by OS warm-restart step D) so AUTORUN.SYS
    ; can re-install the driver at the correct address when DOS re-runs it.
    lda _vera_saved_orig_ramtop
    beq @no_ramtop_restore
    sta RAMTOP
@no_ramtop_restore:

    ; Restore MEMTOP = (driver_base - 1) so DOS doesn't allocate into the driver.
    lda _vera_saved_dest_hi
    beq @no_memtop_restore
    sec
    sbc #1
    sta MEMTOP+1
    lda #$FF
    sta MEMTOP
@no_memtop_restore:

    ; Reinstall deferred VBI so cursor blink and key repeat work after warm reset.
    jsr _InitVbi

    ; Reconfigure VERA hw and reload full font (CRITIC cleared on return).
    jsr _vera_hw_reinit

    ; Warm restart only: wait ~2 s then clear screen (CRITIC=0, VBI running).
    lda WARMST
    beq @no_wait_clear
    jsr _vera_wait_and_clear
@no_wait_clear:

    ; Keep DOSINI/CASINI pointing to our hooks — OS warm start rebuilds ZP.
    jsr re_install_hooks

    ; Re-establish E:/S: HATABS entries rebuilt to OS defaults on every warm start.
    jsr _install_es_hooks
    rts

; _vera_dosini_asm_hook — DOSINI warm-reset hook.
;
; Pattern from Altirra XEP80 handler (xep80handler.s, Reinit, lines 1007-1081):
;
;   "DOS 2.0's AUTORUN.SYS does some pretty funky things here — it jumps
;    through (DOSINI) after loading the handler, but that must NOT actually
;    invoke DOS's init, or the EXE loader hangs.  Therefore, we have to check
;    whether we're handling a warmstart, and if we're not, we have to return
;    without chaining."
;
; Rules:
;   WARMST=0  cold start or AUTORUN.SYS segment loading → reinit, skip chain
;   WARMST≠0  warm restart → reinit, then tail-call old DOSINI if non-null
_vera_dosini_asm_hook:
    jsr common_reinit

    lda WARMST              ; $0008: $FF = warm restart, $00 = cold/loading
    beq @done
    lda _vera_saved_dosini
    ora _vera_saved_dosini+1
    beq @done               ; null saved vector: skip chain (no crash)
    lda _vera_saved_dosini
    sta @jmp+1
    lda _vera_saved_dosini+1
    sta @jmp+2
@jmp:
    jmp $0000               ; tail-call old DOSINI; operand patched at runtime
@done:
    rts

; _vera_casini_asm_hook — CASINI cartridge-mode hook.  Same Altirra pattern.
_vera_casini_asm_hook:
    jsr common_reinit

    lda WARMST
    beq @done
    lda _vera_saved_casini
    ora _vera_saved_casini+1
    beq @done
    lda _vera_saved_casini
    sta @jmp+1
    lda _vera_saved_casini+1
    sta @jmp+2
@jmp:
    jmp $0000
@done:
    rts
