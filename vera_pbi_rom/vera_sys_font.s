    .setcpu "6502"

    .include "vera_common.inc"

    .export _vera_x16_font

    .segment "RODATA"

_vera_x16_font:
.ifdef FONT_8X8
    .incbin "font8x8.bin"
.else
    .incbin "font8x16.bin"
.endif
