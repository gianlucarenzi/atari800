/*
 * ZSM Player for Atari XL/XE PBI VERA
 * Written in cc65 C for easier maintenance
 * Compiled to ~500-600 bytes, auto-optimized by cc65
 */

#include <stdint.h>
#include <string.h>

/* ===== VERA REGISTERS ===== */
#define VERA_ADDR_L     (uint8_t*)0xD100
#define VERA_ADDR_M     (uint8_t*)0xD101
#define VERA_ADDR_H     (uint8_t*)0xD102
#define VERA_DATA0      (uint8_t*)0xD103
#define VERA_DATA1      (uint8_t*)0xD104
#define VERA_CTRL       (uint8_t*)0xD105

/* ===== PLAYER STATE (ZERO PAGE) ===== */
static uint8_t  zsm_ptr_lo;
static uint8_t  zsm_ptr_hi;
static uint8_t  loop_ptr_lo;
static uint8_t  loop_ptr_hi;
static uint8_t  tick_count;
static uint8_t  tick_target;
static uint8_t  tempo;
static uint8_t  flags;          /* bit 0: playing */
static uint8_t  ch_mask;

#define FLAG_PLAYING    0x01

/* ===== MEMORY LAYOUT ===== */
/* $2000-$27FF: ZSM data (up to 2KB) */
/* $3000-$37FF: Player code */
/* $0300-$03FF: PSG state (64 bytes) */

#define ZSM_BASE        0x2000
#define PSG_STATE       ((uint8_t*)0x0300)

/* ===== READ NEXT BYTE FROM ZSM ===== */
static uint8_t read_byte(void)
{
    uint8_t val = *(uint8_t*)(((uint16_t)zsm_ptr_hi << 8) | zsm_ptr_lo);
    
    if (++zsm_ptr_lo == 0) {
        ++zsm_ptr_hi;
    }
    
    return val;
}

/* ===== INITIALIZE PLAYER ===== */
void zsm_init(uint16_t zsm_addr)
{
    uint8_t magic1, magic2, magic3;
    uint16_t i;
    
    zsm_ptr_lo = (uint8_t)zsm_addr;
    zsm_ptr_hi = (uint8_t)(zsm_addr >> 8);
    
    /* Verify ZSM magic */
    magic1 = read_byte();
    magic2 = read_byte();
    magic3 = read_byte();
    
    if (magic1 != 'Z' || magic2 != 'S' || magic3 != 'M') {
        flags = 0;
        return;
    }
    
    /* Read version (ignore) */
    read_byte();
    
    /* Read loop pointer */
    loop_ptr_lo = read_byte();
    loop_ptr_hi = read_byte();
    
    /* Skip reserved bytes */
    read_byte();
    read_byte();
    
    /* Initialize state */
    tick_count = 0;
    tick_target = 1;
    tempo = 1;
    ch_mask = 0xFF;
    
    /* Clear PSG state */
    for (i = 0; i < 64; ++i) {
        PSG_STATE[i] = 0;
    }
    
    /* Mark as playing */
    flags = FLAG_PLAYING;
}

/* ===== PROCESS ONE COMMAND ===== */
static void process_cmd(void)
{
    uint8_t cmd = read_byte();
    
    if (cmd & 0x80) {
        /* VERA write: bits 6-0 = register offset */
        uint8_t reg = cmd & 0x7F;
        uint8_t val = read_byte();
        
        /* Write to VERA register */
        *VERA_ADDR_L = reg;
        *VERA_ADDR_M = 0;
        *VERA_ADDR_H = 0;
        *VERA_DATA0 = val;
    } else {
        /* Delay command or end-of-track */
        if (cmd == 0) {
            /* End of track */
            if (loop_ptr_lo != 0 || loop_ptr_hi != 0) {
                /* Loop: restore position */
                zsm_ptr_lo = loop_ptr_lo;
                zsm_ptr_hi = loop_ptr_hi;
                tick_target = 1;
                tick_count = 0;
            } else {
                /* No loop: stop */
                flags = 0;
            }
        } else {
            /* Delay N ticks */
            tick_target = cmd;
            tick_count = 0;
        }
    }
}

/* ===== VBI TICK HANDLER ===== */
void zsm_tick(void)
{
    if (!(flags & FLAG_PLAYING)) {
        return;
    }
    
    ++tick_count;
    
    while (tick_count >= tick_target) {
        process_cmd();
        if (!(flags & FLAG_PLAYING)) {
            break;
        }
    }
}

/* ===== STOP PLAYBACK ===== */
void zsm_stop(void)
{
    flags = 0;
}

/* ===== RESTART ===== */
void zsm_restart(void)
{
    zsm_ptr_lo -= 8;
    if (zsm_ptr_lo > (uint8_t)ZSM_BASE) {
        --zsm_ptr_hi;
    }
    tick_count = 0;
    tick_target = 1;
    flags = FLAG_PLAYING;
}

/* ===== MAIN ENTRY POINT (called via -run) ===== */
void main(void)
{
    /* Initialize with ZSM at $2000 */
    zsm_init(ZSM_BASE);
    
    /* Simple loop: call tick repeatedly until track ends */
    /* In real use, this would be called from VBI */
    while (flags & FLAG_PLAYING) {
        zsm_tick();
    }
}
