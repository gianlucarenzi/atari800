    .setcpu "6502"

    .export start, _vera_ctl_block
    .import __MAIN_LAST__
    .import _CallVeraApiService, _InitVbi, _vbi_handler
    .import _vera_dosini_asm_hook
    .import _vera_warm_reinit, _vera_warm_start

CASINI              = $0002
DOSINI              = $000C
MEMLO               = $02E7
VERACTL_FLAGS       = 4
VERACTL_REQUEST     = 5
VERACTL_PARAM0      = 6
VERACTL_PARAM1      = 7
VERACTL_CURSOR_X    = 8
VERACTL_CURSOR_Y    = 9
VERACTL_ENTRY_LO    = 10
VERACTL_ENTRY_HI    = 11
VERACTL_VBI_LO      = 12
VERACTL_VBI_HI      = 13
VERACTL_REINIT_LO   = 14
VERACTL_REINIT_HI   = 15
VERA_CTL_FLAG_METRONOME = $01
VERA_CTL_FLAG_API_READY = $80

    .segment "VERACTL"
_vera_ctl_block:
    .res 16

    .segment "STARTUP"
start:
    lda MEMLO+1
    cmp #>__MAIN_LAST__
    bcc reserve_resident
    bne init_control_block
    lda MEMLO
    cmp #<__MAIN_LAST__
    bcs init_control_block

reserve_resident:
    lda #<__MAIN_LAST__
    sta MEMLO
    lda #>__MAIN_LAST__
    sta MEMLO+1

init_control_block:
    lda #(VERA_CTL_FLAG_METRONOME | VERA_CTL_FLAG_API_READY)
    sta _vera_ctl_block + VERACTL_FLAGS
    lda #$00
    sta _vera_ctl_block + VERACTL_REQUEST
    sta _vera_ctl_block + VERACTL_PARAM0
    sta _vera_ctl_block + VERACTL_PARAM1
    sta _vera_ctl_block + VERACTL_CURSOR_X
    sta _vera_ctl_block + VERACTL_CURSOR_Y
    lda #<_CallVeraApiService
    sta _vera_ctl_block + VERACTL_ENTRY_LO
    lda #>_CallVeraApiService
    sta _vera_ctl_block + VERACTL_ENTRY_HI
    lda #<_vbi_handler
    sta _vera_ctl_block + VERACTL_VBI_LO
    lda #>_vbi_handler
    sta _vera_ctl_block + VERACTL_VBI_HI
    lda #<_vera_warm_start
    sta _vera_ctl_block + VERACTL_REINIT_LO
    lda #>_vera_warm_start
    sta _vera_ctl_block + VERACTL_REINIT_HI
    lda #'V'
    sta _vera_ctl_block + 0
    lda #'C'
    sta _vera_ctl_block + 1
    lda #'T'
    sta _vera_ctl_block + 2
    lda #'L'
    sta _vera_ctl_block + 3
    lda #<_vera_dosini_asm_hook
    sta DOSINI
    sta CASINI
    lda #>_vera_dosini_asm_hook
    sta DOSINI+1
    sta CASINI+1
    jsr _InitVbi
    jsr _vera_warm_reinit
    rts
