    .setcpu "6502"

    .export _vera_dosini_asm_hook, _vera_casini_asm_hook
    .import _vera_warm_reinit, _InitVbi
    .import _vera_saved_dosini, _vera_saved_casini
    .import __VERA_EXPORTS__

    .include "atari.inc"

; Offsets within __VERA_EXPORTS__ — must stay in sync with vera_stub.s.
EXP_DOSINI_HOOK = 2
EXP_CASINI_HOOK = 4

; Install our DOSINI/CASINI hooks. The pointer table is read with absolute
; addressing so the linker generates 3-byte instructions (opcode + 16-bit
; addr) — those addresses go through the runtime relocator, unlike the
; `#</#>` immediate-byte pattern which would corrupt the previous opcode
; when MEMLO is not page-aligned.
install_hooks:
    lda __VERA_EXPORTS__+EXP_DOSINI_HOOK
    sta DOSINI
    lda __VERA_EXPORTS__+EXP_DOSINI_HOOK+1
    sta DOSINI+1
    lda __VERA_EXPORTS__+EXP_CASINI_HOOK
    sta CASINI
    lda __VERA_EXPORTS__+EXP_CASINI_HOOK+1
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
