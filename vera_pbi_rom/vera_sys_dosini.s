    .setcpu "6502"

    .export _vera_dosini_asm_hook
    .import _vera_warm_reinit, _InitVbi

    .include "atari.inc"

; This assembly hook is called by DOSINI/CASINI (the standard warm-start vector)
_vera_dosini_asm_hook:
    ; 1. Clear OS critical section flag
    lda #0
    sta CRITIC

    ; 2. Ensure VBI is re-installed
    jsr _InitVbi

    ; 3. Re-run the resident VERA warm-start init directly in asm.
    jsr _vera_warm_reinit

    ; 4. Chain to original warm-start logic (the OS expects us to return)
    jmp $C410 ; ROM_POST_DOSINI
