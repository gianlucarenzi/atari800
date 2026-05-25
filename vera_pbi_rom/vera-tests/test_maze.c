#include <stdio.h>
#include <atari.h>
#include <stdlib.h>
#include "vera_detect.h"

int main(void)
{
    int maze;
    int do_rand = 1;
    int counter = 80 * 25;
	unsigned char *rndgen = (unsigned char *) 0xD20A;

    printf("VeraX16 detected (ID: 0x%04X)\n", vera_require());
    printf("Starting character test maze loop (ESC sequence)...\n");
    srand(12345);

    while(1)
    {
		if (--counter > 1)
		{
			if (do_rand)
			{
				// Prima facciamo una pagina di valori randomizzati con la funzione
				// rand() del C
				maze = rand() % 2;
			}
			else
			{
				// Usiamo i random number generator POKEY
				maze = *(rndgen) & 0x01;
			}
			// Stampiamo il maze
			//printf("%c%c", 27, maze + 6);
			printf("%c", maze + 6);
        }
        else
        {
			counter = 80 * 25;
			do_rand = !do_rand;
		}
    }
    return 0;
}
