#ifndef __RETROBIT_VIDEO_CARD_H__
#define __RETROBIT_VIDEO_CARD_H__

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>

extern void retrobit_video_card_write(UWORD addr, UBYTE byte);
extern UBYTE retrobit_video_card_read(UWORD addr);

static int retrobit_video_card_ready = 0;
static int fdfifo = -1;

static int video_card_initialize_fifo(void)
{
	char * myfifo = "/tmp/rblvfifo";
	int rval = 0;

	if (fdfifo < 0)
	{
		printf("video_card_initialize_fifo() Opening for WRITING\n");
		fdfifo = open(myfifo, O_WRONLY);

		if (fdfifo < 0) {
			rval = 1;
		} else {
			printf("video_card_initialize_fifo() READY\n");
			retrobit_video_card_ready = 1;
		}
	}

	return rval;
}

static unsigned char shadowregs[256];

void retrobit_video_card_write(UWORD addr, UBYTE byte)
{
	UBYTE data[1];
	data[0] = byte;
	if (!retrobit_video_card_ready)
	{
		if (video_card_initialize_fifo()) {
			printf("Unable to access Video Card\n");
			return;
		}
	}

	if (retrobit_video_card_ready)
	{
		UBYTE index = addr & 0x00ff;
		if (shadowregs[ index ] != data[0])
		{
			int r;
			shadowregs[ index ] = data[0];
			printf("Register $%02X - Data: $%02X\n", index, data[0]);
			r = write(fdfifo, shadowregs, 256);
			r = r;
		}
	}
}

UBYTE retrobit_video_card_read(UWORD addr)
{
	UBYTE byte = 0xEA;
	printf("retrobit_video_card_read() $%04X - $%02X\n", addr, byte);
	return byte;
}

#endif
