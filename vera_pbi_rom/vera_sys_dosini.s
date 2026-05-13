    .setcpu "6502"

    .export _vera_dosini_asm_hook
    .import _vera_warm_reinit, _InitVbi
    .import vera_saved_zp, os_saved_zp

    .include "atari.inc"
    .include "zeropage.inc"

; This assembly hook is called by DOSINI/CASINI (the standard warm-start vector)
_vera_dosini_asm_hook:
    ; 1. Clear OS critical section flag
    lda #0
    sta CRITIC

    ; 2. Ensure VBI is re-installed
    jsr _InitVbi

    ; 3. Swap context to CC65 and run C-based warm reinit
    jsr swap_to_cc65_zp
    jsr _vera_warm_reinit
    jsr swap_to_os_zp

    ; 4. Chain to original warm-start logic (the OS expects us to return)
    jmp $C410 ; ROM_POST_DOSINI

swap_to_cc65_zp:
    ldx #$29 ; CC65_ZP_SIZE - 1
@loop:
    lda c_sp,x
    sta os_saved_zp,x
    lda vera_saved_zp,x
    sta c_sp,x
    dex
    bpl @loop
    rts

swap_to_os_zp:
    ldx #$29 ; CC65_ZP_SIZE - 1
@loop:
    lda c_sp,x
    sta vera_saved_zp,x
    lda os_saved_zp,x
    sta c_sp,x
    dex
    bpl @loop
    rts
