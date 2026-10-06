Atari800 emulator version 7.2.1
-------------------------------

About this fork
---------------

This repository (https://github.com/gianlucarenzi/atari800) is a fork of
Atari800 by Gianluca Renzi. It started from Atari800 5.2.0 and has been
brought up to date with the upstream releases since then, up to version
7.2.1 today.

What the fork adds to upstream Atari800:

  * VERA X16 PBI card (-verax16): emulation of a Commander X16 VERA video
    chip on the Atari Parallel Bus Interface, registers at $D100-$D11F,
    with 128KB of VRAM, two layers, sprites, FX, PSG and PCM audio, an SD
    card through VERA SPI (-verax16-sdcard) and the 2KB OS handler ROM at
    $D800-$DFFF (-verax16-rom). It follows firmware 48.0.1. Run
    "atari800 -help" for all -verax16-* options.
  * Enabling the VERA card also enables a RAMbo 256K memory expansion
    banked through PORTB (320KB in all) on XL/XE machines.
  * util/x16-hardware-tests: X16 VERA test programs adapted to run on the
    Atari with the VERA card.

This is a free and portable Atari 8-bit computer and 5200 console emulator,
originally written by David Firth and now developed by the Atari800
Development Team (please see DOC/CREDITS). This program is copyrighted and
released under the GPL (see COPYING).

Please read DOC/README, DOC/INSTALL and DOC/USAGE and eventual platform
specific versions of the above mentioned files.

For news in this release please read DOC/NEWS.

If you have a problem running this emulator please read DOC/FAQ first.
If the problem persists then please ask in newsgroup comp.sys.atari.8bit
or in the atari800-users mailing list (see below).

Look at DOC/BUGS before reporting something you consider a bug.

If you want to help developing Atari800 emulator you can subscribe to our
mailing list (http://lists.sourceforge.net/lists/listinfo/atari800-users).
DOC/TODO might point you in the right direction.

This version, and all previous versions back to v0.8.2 have been released Petr
Stehlik and are available at internet (current project URL is below). Thanks
go to original author David Firth and all the people that contributed since then.

EnJoy!

Petr Stehlik

Project homepage: http://atari800.github.io/
