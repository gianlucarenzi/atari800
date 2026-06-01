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
#include <stdint.h>

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

/* --- VERA keyboard input (only when the VERA *driver* is installed) --- */
#define VCTL_SIG0 'V'
#define VCTL_SIG1 'C'
#define VCTL_SIG2 'T'
#define VCTL_SIG3 'L'

#define VCTL_FLAGS     4
#define VCTL_REQUEST   5
#define VCTL_PARAM0    6
#define VCTL_ENTRY_LO  10
#define VCTL_ENTRY_HI  11

#define VCTL_FLAG_API_READY 0x80
#define VERA_REQ_GETC       0x04

static volatile unsigned char* vctl = 0;
static void (*vera_api_entry)(void) = 0;
static unsigned char vera_pending = 0xFF;

static volatile unsigned char* find_vctl_block(void)
{
    uint16_t base = (uint16_t)((uintptr_t)OS.memtop + 1u);
    uint16_t a;

    /* Sanity: VERA driver lives in RAM below ROM ($C000). */
    if (base < 0x2000u || base >= 0xC000u) return 0;

    /* Scan a small window above MEMTOP for the "VCTL" signature. */
    for (a = base; a < 0xC000u - 16u; ++a) {
        volatile unsigned char* p = (volatile unsigned char*)(uintptr_t)a;
        if (p[0] == VCTL_SIG0 && p[1] == VCTL_SIG1 && p[2] == VCTL_SIG2 && p[3] == VCTL_SIG3) {
            return p;
        }
    }

    return 0;
}

static void vera_api_init(void)
{
    uint16_t entry;

    vctl = find_vctl_block();
    vera_api_entry = 0;
    vera_pending = 0xFF;

    if (!vctl) return;
    if ((vctl[VCTL_FLAGS] & VCTL_FLAG_API_READY) == 0) {
        vctl = 0;
        return;
    }

    entry = (uint16_t)vctl[VCTL_ENTRY_LO] | ((uint16_t)vctl[VCTL_ENTRY_HI] << 8);
    if (entry < 0x2000u || entry >= 0xC000u) {
        vctl = 0;
        return;
    }

    vera_api_entry = (void (*)(void))(uintptr_t)entry;
}

static unsigned char vera_getc_nb(void)
{
    if (!vctl || !vera_api_entry) return 0xFF;

    vctl[VCTL_REQUEST] = VERA_REQ_GETC;
    vera_api_entry();
    return vctl[VCTL_PARAM0];
}

static unsigned char kb_haschar(void)
{
    if (!vctl) return kbhit();

    if (vera_pending != 0xFF) return 1;
    vera_pending = vera_getc_nb();
    return (vera_pending != 0xFF);
}

static unsigned char kb_getchar(void)
{
    unsigned char c;

    if (!vctl) return cgetc();

    if (vera_pending == 0xFF) vera_pending = vera_getc_nb();
    c = vera_pending;
    vera_pending = 0xFF;
    return c;
}

/* --- External Assembly Wrappers --- */
extern void __fastcall__ siov(void);
extern void ih(void);


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
    /* Use OS handler E: for all output */
    putchar(c);
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

    /* Init screen/terminal */
    cursor(1);
    clrscr();
    printf("CP/M Terminal\n");

    /* If the VERA 80x30 driver is loaded, use its non-blocking GETC API. */
    vera_api_init();

    /* Initialize FujiNet session */
    if (nopen() != SUCCESS)
    {
        printf("Open Error!\n");
        while(!kbhit());
        return 1;
    }

    /* --- Interrupt Setup --- */
    old_vprced = OS.vprced;
    old_enabled = PIA.pactl & 1;
    
    PIA.pactl &= (~1);
    OS.vprced = ih;
    PIA.pactl |= 1;

    printf("Connected.\n\n");

    while (running)
    {
        /* 1. KEYBOARD -> FUJINET */
        if (kb_haschar())
        {
            tx_buf[0] = atascii_to_ascii(kb_getchar());
            nwrite(tx_buf, 1);
        }

        /* 2. PRODUCER: SIO -> RING BUFFER */
        if (trip)
        {
            trip = 0;
            status = nstatus();
            
            if (status == E_EOF)
            {
                printf("\nDisconnected.\n");
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
