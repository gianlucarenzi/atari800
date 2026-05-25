#ifndef VERA_DETECT_H
#define VERA_DETECT_H

#include <stdio.h>
#include <stdlib.h>

/*
 * Unique identifier for the VeraX16 PBI video card.
 * 'V' (0x56) | 'X' (0x58) — returned by vera_detect() on success.
 */
#define VERA_CARD_ID  ((unsigned int)0x5658)

/*
 * vera_detect() — probe the VeraX16 PBI video card at $D100.
 *
 * Uses the same write-read-back pattern as WAIT_VERA in the PBI handler ROM:
 * two distinct sentinel values are written to VERA_ADDR_L and verified to
 * rule out stale bus-capacitance false positives.
 *
 * Returns VERA_CARD_ID (0x5658, 'VX') when the card responds correctly,
 * 0 when the register does not hold the written value (card absent or
 * emulator not started with -verax16).
 */
static unsigned int vera_detect(void)
{
    volatile unsigned char * const addr_l = (volatile unsigned char *)0xD100;

    *addr_l = 0x2A;
    if (*addr_l != 0x2A) return 0;
    *addr_l = 0xD5;
    if (*addr_l != 0xD5) return 0;

    /* Restore VRAM address registers to a safe state */
    *addr_l                              = 0x00;
    *(volatile unsigned char *)0xD101    = 0x00;
    *(volatile unsigned char *)0xD102    = 0x00;

    return VERA_CARD_ID;
}

/*
 * vera_require() — fatal guard for programs that need the VeraX16 card.
 *
 * Prints an error and calls exit(1) when the card is absent.
 * Returns VERA_CARD_ID on success.
 */
static unsigned int vera_require(void)
{
    unsigned int id = vera_detect();
    if (!id) {
        printf("ERROR: VeraX16 PBI card not found ($D100 not responding).\n");
        printf("Launch with: -verax16 -verax16-rom vera_pbi_handler.rom\n");
        exit(1);
    }
    return id;
}

#endif /* VERA_DETECT_H */
