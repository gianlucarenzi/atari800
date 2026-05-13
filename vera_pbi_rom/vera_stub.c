#include <atari.h>

typedef struct VeraCtl {
    unsigned char sig0;
    unsigned char sig1;
    unsigned char sig2;
    unsigned char sig3;
    unsigned char flags;
    unsigned char request;
    unsigned char param0;
    unsigned char param1;
    unsigned char cursor_x;
    unsigned char cursor_y;
    unsigned char entry_lo;
    unsigned char entry_hi;
    unsigned char vbi_lo;
    unsigned char vbi_hi;
    unsigned char reinit_lo;
    unsigned char reinit_hi;
} VeraCtl;

#pragma bss-name(push, "VERACTL")
volatile VeraCtl vera_ctl_block;
#pragma bss-name(pop)

int main(void)
{
    return 0;
}
