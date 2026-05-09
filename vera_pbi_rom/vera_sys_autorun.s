    .setcpu "6502"

    .export __AUTOSTART__
    .import start

    .include "atari.inc"

__AUTOSTART__ = $0100

    .segment "AUTOINIT"
    .word INITAD
    .word INITAD + 1
    .word start

    .segment "AUTOSTRT"
    .word RUNAD
    .word RUNAD + 1
    .word _run_stub

    .segment "CODE"
_run_stub:
    jmp $A000
