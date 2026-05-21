#include <stdio.h>
#include <atari.h>

int main(void) {
    int i;
    printf("Starting character test loop (ESC sequence)...\n");
    while(1) {
        for (i = 0; i < 256; i++) {
            printf("%c%c", 27, i);
        }
    }
    return 0;
}
