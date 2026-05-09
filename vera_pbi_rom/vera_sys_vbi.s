    .setcpu "6502"

    .export _InitVbi, _CallVeraApiService, _vera_vbi_end
    .import _VBI, _VeraApiService

    .include "atari.inc"
    .include "zeropage.inc"

    .segment "LOWBSS"
vbi_zp_save:
    .res zpspace

    .segment "CODE"

_InitVbi:
    sei
    ldy #<_vbi_handler
    ldx #>_vbi_handler
    lda #7
    jsr SETVBV
    cli
    rts

_vbi_handler:
    pha
    txa
    pha
    tya
    pha
    lda CRITIC
    bne @skip_vbi
    jsr save_cc65_zp
    jsr _VBI
    jsr restore_cc65_zp
@skip_vbi:
    pla
    tay
    pla
    tax
    pla
    jmp XITVBV

_CallVeraApiService:
    pha
    txa
    pha
    tya
    pha
    jsr save_cc65_zp
    jsr _VeraApiService
    jsr restore_cc65_zp
    pla
    tay
    pla
    tax
    pla
    rts

save_cc65_zp:
    ldx #zpspace - 1
@save_zp:
    lda sp,x
    sta vbi_zp_save,x
    dex
    bpl @save_zp
    rts

restore_cc65_zp:
    ldx #zpspace - 1
@restore_zp:
    lda vbi_zp_save,x
    sta sp,x
    dex
    bpl @restore_zp
    rts

_vera_vbi_end:
