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

    ; Re-establish E:/S: HATABS hooks — the OS rebuilds HATABS to defaults
    ; on every warm start, so without this every reset loses VERA mirror.
    jsr _install_es_hooks
    rts

; Standard DOS warm-start path.
_vera_dosini_asm_hook:
    jsr common_reinit
    jmp (_vera_saved_dosini)

; DOS "cartridge mode" path.
; Run common_reinit first (patches HATABS, installs VBI), then call saved CASINI
; only if it is non-null — a null ($0000) saved pointer means no previous handler
; was installed and jumping through it would execute garbage in page 0.
_vera_casini_asm_hook:
    jsr common_reinit
    lda _vera_saved_casini
    ora _vera_saved_casini+1
    beq @done                   ; skip if null
    jmp (_vera_saved_casini)
@done:
    rts
