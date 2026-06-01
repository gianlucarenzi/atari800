/**
 * ============================================================================
 * RUNCPM.C - FujiNet RunCPM Terminal with Asynchronous Buffering
 * Supports Direct VERA 80x30 and Standard Atari 40-col fallback.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <unistd.h>
#include <atari.h>
#include "vera_detect.h"

/* --- SIO Constants --- */
#define DFUJI   0x71
#define DREAD   0x40
#define DWRITE  0x80
#define SUCCESS 1
#define E_EOF   136
#define TIMEOUT 0x1F

/* --- Ring Buffer Configuration --- */
#define RING_SIZE 2048
unsigned char ring_buf[RING_SIZE];
unsigned short head = 0;
unsigned short tail = 0;
unsigned short count = 0;

/* --- SIO Buffers --- */
unsigned char sio_rx_tmp[256];
unsigned char tx_buf[64];
/* FujiNet expects exactly 256 bytes for Open command spec */
unsigned char devicespec[256];

/* --- State Variables --- */
unsigned char trip = 0;
void* old_vprced;
unsigned char old_enabled;
unsigned int  vera_present = 0;

/* --- External Assembly Wrappers --- */
extern void __fastcall__ siov(void);
extern void ih(void);

/* --- VERA Direct Hardware Driver --- */
extern void v_init(void);
extern void v_cls(void);
extern void __fastcall__ v_putc(unsigned char c);

/* 
 * ============================================================================
 * FUJINET N: PROTOCOL WRAPPERS
 * ============================================================================
 */

unsigned char nopen(void)
{
    /* Clear and prepare devicespec buffer */
    memset(devicespec, 0, 256);
    strcpy((char*)devicespec, "N1:CPM:///");
    /* FujiNet expects ATASCII EOL ($9B) as terminator */
    devicespec[strlen((char*)devicespec)] = 0x9B;

    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'O';
    OS.dcb.dstats = DWRITE;
    OS.dcb.dbuf   = devicespec;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 256;      /* Fixed 256 byte payload */
    OS.dcb.daux1  = 0x0C;
    OS.dcb.daux2  = 3;        /* Translation CRLF */
    siov();
    return OS.dcb.dstats;
}

unsigned char nstatus(void)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'S';
    OS.dcb.dstats = DREAD;
    OS.dcb.dbuf   = OS.dvstat;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 4;
    OS.dcb.daux1  = 0;
    OS.dcb.daux2  = 0;
    siov();
    return OS.dvstat[3];
}

unsigned char nread(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'R';
    OS.dcb.dstats = DREAD;
    OS.dcb.dbuf   = buf;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = len;
    OS.dcb.daux1  = len & 0xFF;
    OS.dcb.daux2  = (len >> 8) & 0xFF;
    siov();
    return OS.dcb.dstats;
}

unsigned char nwrite(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'W';
    OS.dcb.dstats = DWRITE;
    OS.dcb.dbuf   = buf;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = len;
    OS.dcb.daux1  = len & 0xFF;
    OS.dcb.daux2  = (len >> 8) & 0xFF;
    siov();
    return OS.dcb.dstats;
}

/* 
 * ============================================================================
 * RING BUFFER LOGIC
 * ============================================================================
 */

void ring_put(unsigned char c)
{
    if (count < RING_SIZE)
    {
        ring_buf[head] = c;
        head = (head + 1) % RING_SIZE;
        count++;
    }
}

unsigned char ring_get(void)
{
    unsigned char c = 0;
    if (count > 0)
    {
        c = ring_buf[tail];
        tail = (tail + 1) % RING_SIZE;
        count--;
    }
    return c;
}

/* 
 * ============================================================================
 * UTILITIES
 * ============================================================================
 */

unsigned char atascii_to_ascii(unsigned char c)
{
    if (c == 155) return 13;
    if (c == 126) return 8;
    return c;
}

void terminal_putc(unsigned char c)
{
    if (vera_present)
    {
        v_putc(c);
    }
    else
    {
        if (c == 13) putchar('\n');
        else if (c == 10) ; /* Skip LF */
        else if (c == 8) putchar('\b');
        else putchar(c);
    }
}

/* 
 * ============================================================================
 * MAIN TERMINAL LOOP
 * ============================================================================
 */

int main(void)
{
    unsigned char status;
    unsigned short bw, i, chunk;
    int running = 1;

    /* Silence unused warning */
    (void)vera_require;

    /* Detect and Init VERA if present */
    vera_present = (vera_detect() == VERA_CARD_ID);

    if (vera_present)
    {
        v_init();
    }
    else
    {
        cursor(1);
        clrscr();
        printf("CP/M Terminal (40-col fallback)\n");
    }

    /* Initialize FujiNet session */
    if (nopen() != SUCCESS)
    {
        if (!vera_present) printf("Open Error!\n");
        while(!kbhit());
        return 1;
    }

    /* --- Interrupt Setup --- */
    old_vprced = OS.vprced;
    old_enabled = PIA.pactl & 1;
    
    PIA.pactl &= (~1);
    OS.vprced = ih;
    PIA.pactl |= 1;

    if (!vera_present) printf("Connected.\n\n");

    while (running)
    {
        /* 1. KEYBOARD -> FUJINET */
        if (kbhit())
        {
            tx_buf[0] = atascii_to_ascii(cgetc());
            nwrite(tx_buf, 1);
        }

        /* 2. PRODUCER: SIO -> RING BUFFER */
        if (trip)
        {
            trip = 0;
            status = nstatus();
            
            if (status == E_EOF)
            {
                if (!vera_present) printf("\nDisconnected.\n");
                running = 0;
            }
            else
            {
                bw = (OS.dvstat[1] << 8) | OS.dvstat[0];
                if (bw > 0)
                {
                    if (bw > 256) bw = 256;
                    if (nread(sio_rx_tmp, bw) == SUCCESS)
                    {
                        for (i = 0; i < bw; ++i) ring_put(sio_rx_tmp[i]);
                    }
                }
            }
            PIA.pactl |= 1; 
        }

        /* 3. CONSUMER: RING BUFFER -> SCREEN */
        if (count > 0)
        {
            chunk = (count > 128) ? 128 : count;
            for (i = 0; i < chunk; ++i)
            {
                terminal_putc(ring_get());
            }
        }
    }

    /* --- Cleanup --- */
    PIA.pactl &= (~1);
    OS.vprced = old_vprced;
    PIA.pactl |= old_enabled;
    
    return 0;
}
