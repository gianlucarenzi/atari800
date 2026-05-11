    .setcpu "6502"

    .export __AUTOSTART__
    .import start

    .include "atari.inc"

__AUTOSTART__ = $0100

    .segment "AUTOSTRT"
    .word RUNAD
    .word RUNAD + 1
    .word start

