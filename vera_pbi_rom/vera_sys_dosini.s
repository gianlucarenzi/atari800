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

    ; 3. Keep the warm-start vectors hooked across DOS/BASIC/cartridge transitions.
    lda #<_vera_dosini_asm_hook
    sta DOSINI
    sta CASINI
    lda #>_vera_dosini_asm_hook
    sta DOSINI+1
    sta CASINI+1

    ; 4. Re-run the resident VERA warm-start init directly in asm.
    jsr _vera_warm_reinit

    ; 5. Chain to original warm-start logic (the OS expects us to return)
    jmp $C410 ; ROM_POST_DOSINI
