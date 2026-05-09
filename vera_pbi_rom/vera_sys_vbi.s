    .setcpu "6502"

    .export _InitVbi, _vera_vbi_end
    .import _VBI

    .include "atari.inc"

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
    jsr _VBI
    pla
    tay
    pla
    tax
    pla
    jmp XITVBV

_vera_vbi_end:
