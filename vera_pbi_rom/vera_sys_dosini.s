    .setcpu "6502"

    .export _vera_dosini_asm_hook, _vera_casini_asm_hook
    .import _vera_warm_reinit, _InitVbi
    .import _vera_saved_dosini, _vera_saved_casini

    .include "atari.inc"

install_hooks:
    lda #<_vera_dosini_asm_hook
    sta DOSINI
    lda #>_vera_dosini_asm_hook
    sta DOSINI+1
    lda #<_vera_casini_asm_hook
    sta CASINI
    lda #>_vera_casini_asm_hook
    sta CASINI+1
    rts

common_reinit:
    lda #0
    sta CRITIC

    ; Ensure VBI is re-installed.
    jsr _InitVbi

    ; Keep the warm-start and cartridge vectors hooked across transitions.
    jsr install_hooks

    ; Re-run the resident VERA warm-start init directly in asm.
    jsr _vera_warm_reinit
    rts

; Standard DOS warm-start path.
_vera_dosini_asm_hook:
    jsr common_reinit
    jmp (_vera_saved_dosini)

; DOS "cartridge mode" path.
_vera_casini_asm_hook:
    jsr common_reinit
    jmp (_vera_saved_casini)
