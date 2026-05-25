#include <stdio.h>
#include <atari.h>
#include "vera_detect.h"

int main(void) {
    int i;
    printf("VeraX16 detected (ID: 0x%04X)\n", vera_require());
    printf("Starting character test loop (ESC sequence)...\n");
    while(1) {
        for (i = 0; i < 256; i++) {
            printf("%c%c", 27, i);
        }
    }
}
