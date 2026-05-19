    .setcpu "6502"

    .export _vera_x16_font

    .segment "RODATA"

_vera_x16_font:
    .incbin "font8x16.bin"
