/**
 * ============================================================================
 * RUNCPM.C - FujiNet RunCPM Terminal with Asynchronous Buffering
 * ============================================================================
 * This program implements a terminal to interact with the CP/M emulator
 * running inside the FujiNet peripheral. It uses a Ring Buffer to decouple
 * SIO bus speed from the slower screen scrolling speed.
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <unistd.h>
#include <atari.h>

/* --- SIO Constants --- */
#define DFUJI   0x71        /* FujiNet SIO Device ID */
#define DREAD   0x40        /* SIO Read operation flag */
#define DWRITE  0x80        /* SIO Write operation flag */
#define SUCCESS 1           /* SIO Success status code */
#define E_EOF   136         /* FujiNet EOF/Disconnected status */
#define TIMEOUT 0x1F        /* Standard 30s timeout */

/* --- Ring Buffer Configuration --- */
/* The Ring Buffer absorbs SIO bursts while the screen scrolls slowly */
#define RING_SIZE 2048      /* Covers ~one 80x25 text screen */
unsigned char ring_buf[RING_SIZE];
unsigned short head = 0;    /* Write pointer (Producer) */
unsigned short tail = 0;    /* Read pointer (Consumer) */
unsigned short count = 0;   /* Number of bytes waiting in buffer */

/* --- Buffers --- */
unsigned char sio_rx_tmp[256];      /* Temp storage for SIO read chunks */
unsigned char tx_buf[64];           /* Buffer for keyboard transmissions */
char devicespec[256] = "N1:CPM:///"; /* FujiNet N: protocol specification */

/* --- State Variables --- */
unsigned char trip = 0;             /* Set to 1 by the assembly interrupt handler */
void* old_vprced;                   /* Stores original OS VPRCED vector */
unsigned char old_enabled;          /* Stores original PIA interrupt state */

/* --- External Assembly Wrappers --- */
extern void __fastcall__ siov(void);
extern void ih(void);

/* 
 * ============================================================================
 * FUJINET N: PROTOCOL WRAPPERS
 * ============================================================================
 * These functions manipulate the Atari OS DCB (Device Control Block) directly.
 */

unsigned char nopen(char* spec, unsigned char trans)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'O';      /* OPEN */
    OS.dcb.dstats = DWRITE;   /* Sending URL to FujiNet */
    OS.dcb.dbuf   = spec;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 256;      /* FujiNet expects 256 byte frame for spec */
    OS.dcb.daux1  = 0x0C;     /* Read/Write mode */
    OS.dcb.daux2  = trans;    /* Translation (0=None, 3=CR/LF) */
    siov();
    return OS.dcb.dstats;
}

unsigned char nstatus(void)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'S';      /* STATUS */
    OS.dcb.dstats = DREAD;    /* Getting status from FujiNet */
    OS.dcb.dbuf   = OS.dvstat;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = 4;        /* 4 bytes status frame */
    OS.dcb.daux1  = 0;
    OS.dcb.daux2  = 0;
    siov();
    /* Extended error/connection status is in the 4th byte of dvstat */
    return OS.dvstat[3];
}

unsigned char nread(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'R';      /* READ */
    OS.dcb.dstats = DREAD;
    OS.dcb.dbuf   = buf;
    OS.dcb.dtimlo = TIMEOUT;
    OS.dcb.dbyt   = len;
    OS.dcb.daux1  = len & 0xFF;        /* Length low byte */
    OS.dcb.daux2  = (len >> 8) & 0xFF; /* Length high byte */
    siov();
    return OS.dcb.dstats;
}

unsigned char nwrite(unsigned char* buf, unsigned short len)
{
    OS.dcb.ddevic = DFUJI;
    OS.dcb.dunit  = 1;
    OS.dcb.dcomnd = 'W';      /* WRITE */
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
    if (c == 155) return 13; /* ATASCII EOL -> ASCII CR */
    if (c == 126) return 8;  /* ATASCII Backspace -> ASCII BS */
    return c;
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

    cursor(1);
    clrscr();
    printf("CP/M Buffered Terminal (N:)\n");

    /* Initialize FujiNet session */
    if (nopen(devicespec, 3) != SUCCESS)
    {
        printf("Open Error! Check FujiNet config.\n");
        while(!kbhit());
        return 1;
    }

    /* --- Interrupt Setup --- */
    old_vprced = OS.vprced;     /* Save OS Proceed Vector */
    old_enabled = PIA.pactl & 1; /* Save PIA control state */
    
    PIA.pactl &= (~1);          /* Turn off CA1 IRQ while changing vector */
    OS.vprced = ih;             /* Point to our assembly handler */
    PIA.pactl |= 1;             /* Enable CA1 (PROCEED line) Interrupt */

    printf("Connected. Type exit to leave.\n\n");

    while (running)
    {
        /* 1. KEYBOARD -> FUJINET (High Priority) */
        /* Check if user typed anything to keep interaction responsive */
        if (kbhit())
        {
            tx_buf[0] = atascii_to_ascii(cgetc());
            nwrite(tx_buf, 1);
        }

        /* 2. PRODUCER: SIO -> RING BUFFER */
        /* Triggered by the FujiNet asserting the PROCEED line */
        if (trip)
        {
            trip = 0;           /* Reset software flag */
            status = nstatus(); /* Check connection and bytes waiting */
            
            if (status == E_EOF)
            {
                printf("\nCP/M Session Terminated.\n");
                running = 0;
            }
            else
            {
                /* Fetch waiting bytes (clamped to 256 per chunk) */
                bw = (OS.dvstat[1] << 8) | OS.dvstat[0];
                if (bw > 0)
                {
                    if (bw > 256) bw = 256;
                    if (nread(sio_rx_tmp, bw) == SUCCESS)
                    {
                        /* Quickly push data into the Ring Buffer */
                        for (i = 0; i < bw; ++i) 
                        {
                            ring_put(sio_rx_tmp[i]);
                        }
                    }
                }
            }
            /* Re-enable PIA IRQ to catch the next signal */
            PIA.pactl |= 1; 
        }

        /* 3. CONSUMER: RING BUFFER -> SCREEN */
        /* Process a small chunk of the buffer to keep keyboard alive */
        if (count > 0)
        {
            /* Print up to 64 chars before checking keyboard again */
            chunk = (count > 64) ? 64 : count;
            for (i = 0; i < chunk; ++i)
            {
                unsigned char c = ring_get();
                if (c == 13) putchar('\n');
                else if (c == 10) ; /* Skip CP/M line feeds (Atari uses EOL) */
                else if (c == 8) putchar('\b');
                else putchar(c);
            }
        }
    }

    /* --- Cleanup --- */
    /* Restore original OS state before exiting to DOS */
    PIA.pactl &= (~1);
    OS.vprced = old_vprced;
    PIA.pactl |= old_enabled;
    
    printf("Returning to DOS.\n");
    return 0;
}
