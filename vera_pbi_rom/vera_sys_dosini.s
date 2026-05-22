    .setcpu "6502"

    .export _vera_dosini_asm_hook, _vera_casini_asm_hook
    .import _vera_warm_reinit, _InitVbi
    .import _vera_saved_dosini, _vera_saved_casini
    .import _install_es_hooks
    .import __VERA_EXPORTS__

    .include "atari.inc"

; Offsets within __VERA_EXPORTS__ — must stay in sync with vera_stub.s.
EXP_DOSINI_HOOK = 2
EXP_CASINI_HOOK = 4

; Re-install our hooks into HATABS/VECTORS. 
; Safe to call at both cold and warm start.
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

common_reinit:
    lda #0
    sta CRITIC

    ; Ensure VBI is re-installed.
    jsr _InitVbi

    ; Re-run the resident VERA warm-start init.
    jsr _vera_warm_reinit

    ; Always re-install hooks to cover the vectors destroyed by reset.
    jsr re_install_hooks

    ; Re-establish E:/S: HATABS hooks.
    jsr _install_es_hooks
    rts

; Standard DOS warm-start path.
warmst = $09

_vera_dosini_asm_hook:
    ; Always re-init (video/hooks) but avoid infinite recursion by not
    ; chaining through saved vectors during the re-init process itself.
    jsr common_reinit

@chain:
    lda _vera_saved_dosini
    sta @jmp+1
    lda _vera_saved_dosini+1
    sta @jmp+2
@jmp:
    jmp $0000                   ; operand patched at runtime

; DOS "cartridge mode" path.
; Run common_reinit first (patches HATABS, installs VBI), then tail-call saved
; CASINI only if non-null — a null ($0000) pointer means no previous handler.
_vera_casini_asm_hook:
    jsr common_reinit
    lda _vera_saved_casini
    ora _vera_saved_casini+1
    beq @done                   ; skip if null
    lda _vera_saved_casini
    sta @jmp+1
    lda _vera_saved_casini+1
    sta @jmp+2
@jmp:
    jmp $0000                   ; operand patched at runtime
@done:
    rts
