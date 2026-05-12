    .setcpu "6502"

    .export _vera_save_c_sp
    .export _vera_dosini_hook

    .import _vera_reinit
    .import _vera_saved_dosini

    .include "atari.inc"
    .include "zeropage.inc"

; ---------------------------------------------------------------------------
; vera_saved_sp — warm-start-safe copy of the cc65 software stack pointer.
;
; Placed in WARMZP at $AC-$AD:
;   - Above the cc65 zpspace range ($82-$9B, 26 bytes saved by save_cc65_zp):
;     NOT saved/restored on VBI or API calls, value persists across them.
;   - Above EXTZP ($9C-$9D used by cc65 runtime): no conflict.
;   - $82-$FF is cc65 territory, not touched by the OS on warm start.
;   - On cold start the OS zeros all of ZP; main() runs immediately after
;     and calls _vera_save_c_sp before returning, writing the correct value.
; ---------------------------------------------------------------------------
    .segment "WARMZP_VARS"
vera_saved_sp:
    .res 2

    .segment "CODE"

; _vera_save_c_sp  — called once at the end of main().
; Saves the cc65 software stack pointer (c_sp) to the warm-start-safe ZP cell.
; Must be the very last call before return 0 so the stack is fully unwound.
_vera_save_c_sp:
    lda c_sp
    sta vera_saved_sp
    lda c_sp + 1
    sta vera_saved_sp + 1
    rts

; _vera_dosini_hook  — installed into DOSINI ($000C) by main().
; The OS calls DOSINI (via JSR) on every cold/warm start, after PBI INIT has
; already run INIT_VERA_SCREEN.
;
; Sequence:
;   1. Restore c_sp from vera_saved_sp ($AC).  On warm start c_sp/$82 holds
;      whatever it was when Reset was pressed; vera_saved_sp/$AC still has
;      the clean value saved by main().
;   2. Call C vera_reinit(): writes "DEVICE HANDLER READY", re-hooks E:/S:,
;      re-installs VBI.
;   3. Chain to _vera_saved_dosini (e.g. DOS 2.0 reinit) via JMP-indirect.
;      The OS called us with JSR, so its return address is on the hardware
;      stack; the chained routine's RTS goes back to the OS correctly.
_vera_dosini_hook:
    pha
    txa
    pha
    tya
    pha

    lda vera_saved_sp
    sta c_sp
    lda vera_saved_sp + 1
    sta c_sp + 1

    jsr _vera_reinit

    pla
    tay
    pla
    tax
    pla

    ; Chain to the DOSINI that was active when main() ran (e.g. DOS 2.0 init).
    lda _vera_saved_dosini
    ora _vera_saved_dosini + 1
    beq @done
    jmp (_vera_saved_dosini)
@done:
    rts
