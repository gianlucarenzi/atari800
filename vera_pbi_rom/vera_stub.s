; vera_stub.s — body interface for the relocator.
;
; Defines:
;   - EXPORTS: 18-byte pointer table at offset 0 of the body. The bootstrap
;     loader reads it after relocation to find every entry/data symbol it
;     needs to wire up.
;   - VCTL: 16-byte block at the very end of the body. The bootstrap fills it
;     with the signature 'VCTL' plus the relocated pointers. PBI ROM looks
;     here at MEMLO-16 to find the driver.

    .setcpu "6502"

    .export _vera_ctl_block
    .export __VERA_EXPORTS__

    .import _vera_warm_reinit
    .import _vera_dosini_asm_hook
    .import _vera_casini_asm_hook
    .import _vera_saved_dosini
    .import _vera_saved_casini
    .import _vbi_handler
    .import _CallVeraApiService
    .import _vera_warm_start
    .import _InitVbi
    .import _install_es_hooks
    .import _vera_kbd_irq_handler
    .import _vera_saved_orig_ramtop
    .import _vera_saved_dest_hi

    .segment "EXPORTS"
__VERA_EXPORTS__:
    .word _vera_warm_reinit       ; +$00
    .word _vera_dosini_asm_hook   ; +$02
    .word _vera_casini_asm_hook   ; +$04
    .word _vera_saved_dosini      ; +$06
    .word _vera_saved_casini      ; +$08
    .word _vbi_handler            ; +$0A
    .word _CallVeraApiService     ; +$0C
    .word _vera_warm_start        ; +$0E
    .word _vera_ctl_block         ; +$10
    .word _InitVbi                ; +$12
    .word _install_es_hooks       ; +$14
    .word _vera_kbd_irq_handler   ; +$16  EXP_KBD_HANDLER
    .word _vera_saved_orig_ramtop ; +$18  EXP_SAVED_ORIG_RAMTOP
    .word _vera_saved_dest_hi     ; +$1A  EXP_SAVED_DEST_HI

    .segment "VCTL"
_vera_ctl_block:
    .res 32
