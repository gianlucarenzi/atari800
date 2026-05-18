    .setcpu "6502"

    .export _vera_x16_font

    .segment "RODATA"

_vera_x16_font:
    .incbin "x16_font16.bin"
