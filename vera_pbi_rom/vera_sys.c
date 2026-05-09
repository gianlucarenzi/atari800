#include <atari.h>

#define MEMLO_ADDR (*(unsigned*)0x02E7)
#define AUDF1_ADDR (*(unsigned char*)0xD200)
#define AUDC1_ADDR (*(unsigned char*)0xD201)
#define VERA_CTL_SIG0 (*(volatile unsigned char*)0x0480)
#define VERA_CTL_SIG1 (*(volatile unsigned char*)0x0481)
#define VERA_CTL_SIG2 (*(volatile unsigned char*)0x0482)
#define VERA_CTL_SIG3 (*(volatile unsigned char*)0x0483)
#define VERA_CTL_FLAGS (*(volatile unsigned char*)0x0484)

#define FREQ   0x08
#define VOLUME 0xAF
#define RATE   10
#define VERA_CTL_FLAG_METRONOME 0x01

extern void InitVbi(void);
extern char vera_vbi_end;

unsigned char frames_until_click = RATE;
unsigned char click_active;
unsigned char resident_end_marker;

static void reserve_resident(void)
{
    unsigned resident_end = (unsigned)&resident_end_marker + 1u;
    unsigned vbi_end = (unsigned)&vera_vbi_end;

    if (resident_end < vbi_end) {
        resident_end = vbi_end;
    }

    if (MEMLO_ADDR < resident_end) {
        MEMLO_ADDR = resident_end;
    }
}

static void init_control_block(void)
{
    if (VERA_CTL_SIG0 != 'V' ||
        VERA_CTL_SIG1 != 'C' ||
        VERA_CTL_SIG2 != 'T' ||
        VERA_CTL_SIG3 != 'L') {
        VERA_CTL_FLAGS = VERA_CTL_FLAG_METRONOME;
    }

    VERA_CTL_SIG0 = 'V';
    VERA_CTL_SIG1 = 'C';
    VERA_CTL_SIG2 = 'T';
    VERA_CTL_SIG3 = 'L';
}

void VBI(void)
{
    if ((VERA_CTL_FLAGS & VERA_CTL_FLAG_METRONOME) == 0) {
        if (click_active) {
            AUDC1_ADDR = 0x00;
            click_active = 0;
        }
        frames_until_click = RATE;
        return;
    }

    if (click_active) {
        AUDC1_ADDR = 0x00;
        click_active = 0;
    }

    if (--frames_until_click == 0) {
        AUDF1_ADDR = FREQ;
        AUDC1_ADDR = VOLUME;
        frames_until_click = RATE;
        click_active = 1;
    }
}

int main(void)
{
    reserve_resident();
    init_control_block();
    InitVbi();
    ANTIC.nmien = NMIEN_VBI;

    for (;;) {
    }

    return 0;
}
