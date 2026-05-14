    .setcpu "6502"

    .import __STARTUP_LOAD__, __DATA_LOAD__, __DATA_SIZE__

    .segment "EXEHDR"
    .word $FFFF

    .segment "MAINHDR"
    .word __STARTUP_LOAD__, __DATA_LOAD__ + __DATA_SIZE__ - 1
