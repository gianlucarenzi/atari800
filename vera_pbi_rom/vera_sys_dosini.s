    .setcpu "6502"

    .export _vera_save_c_sp
    .export _vera_dosini_hook
    .export vera_saved_zp
    .export os_saved_zp
    .import _vera_saved_dosini
    .import _CallVeraApiService
    .import _InitVbi
    .import _vera_reinit
    .import _vera_ctl_block

    .include "atari.inc"
    .include "zeropage.inc"

; ---------------------------------------------------------------------------
; vera_saved_sp — warm-start-safe copy of the cc65 software stack pointer.
;
; Placed in WARMZP at $AC-$AD:
;   - Above the cc65 zpspace range ($82-$9B, 26 bytes saved by save_cc65_zp):
;     NOT saved/restored on VBI or API calls, value persists across them.
;   - Above EXTZP ($9C-$9D used by cc65 runtime): no conflict.
;   - $82-$FF is cc65 territory, not touched by the OS on warm start.
;   - On cold start the OS zeros all of ZP; main() runs immediately after
;     and calls _vera_save_c_sp before returning, writing the correct value.
; ---------------------------------------------------------------------------
CC65_ZP_SIZE = $2A
ROM_POST_DOSINI = $C410
VERA_ADDR_L = $D100
VERA_ADDR_M = $D101
VERA_ADDR_H = $D102
VERA_DATA0  = $D103
VERA_TEXT_COLOR = $61
VERA_SCREEN_BANK = $11
VERA_SCREEN_BASE_M = $B0
VERA_CTL_FLAG_METRONOME = $01
VERA_CTL_FLAG_API_READY = $80

VERACTL_SIG0 = 0
VERACTL_SIG1 = 1
VERACTL_SIG2 = 2
VERACTL_SIG3 = 3
VERACTL_FLAGS = 4
VERACTL_REQUEST = 5
VERACTL_PARAM0 = 6
VERACTL_PARAM1 = 7
VERACTL_CURSOR_X = 8
VERACTL_CURSOR_Y = 9
VERACTL_ENTRY_LO = 10
VERACTL_ENTRY_HI = 11
VERACTL_VBI_LO = 12
VERACTL_VBI_HI = 13

    .segment "LOWBSS"
vera_saved_zp:
    .res CC65_ZP_SIZE

os_saved_zp:
    .res CC65_ZP_SIZE

    .segment "CODE"

; _vera_save_c_sp  — called once at the end of main().
; Saves the entire cc65 zero-page window ($82-$AB), including c_sp and the
; runtime temporaries used by generated C code. Must be the very last call
; before return 0 so the saved C state is fully unwound and stable.
_vera_save_c_sp:
    ldx #CC65_ZP_SIZE - 1
@save_zp:
    lda c_sp,x
    sta vera_saved_zp,x
    dex
    bpl @save_zp
    rts

; _vera_dosini_hook  — installed into DOSINI ($000C) by main().
; DOSINI stays permanently pointed at this resident hook.
;   1. DOSINI stays permanently pointed at this resident hook.
;   2. The original DOSINI target is saved in _vera_saved_dosini.
;   3. On every cold/warm start we patch the current RTS return address so the
;      original DOSINI target returns into our local post-DOSINI stub.
;   4. We JMP to the original DOSINI target, preserving its normal call shape.
;   5. The post-DOSINI stub performs the minimal resident VERA reinit directly
;      in assembly, then resumes the ROM warm-start path at $C410.
_vera_dosini_hook:
    lda #<_vera_dosini_hook
    sta DOSINI
    lda #>_vera_dosini_hook
    sta DOSINI + 1

    tsx
    lda #<(post_dosini - 1)
    sta $0101,x
    lda #>(post_dosini - 1)
    sta $0102,x

    lda _vera_saved_dosini
    ora _vera_saved_dosini + 1
    beq post_dosini
    jmp (_vera_saved_dosini)

post_dosini:
    pha
    txa
    pha
    tya
    pha

    ; 1. Re-init VERA hardware state and OS critical state in assembly
    jsr asm_reinit_vera
    jsr _InitVbi

    ; 2. Swap to cc65 ZP to call C reinit (hooks etc)
    jsr swap_to_cc65_zp
    jsr _vera_reinit
    jsr swap_to_os_zp

    pla
    tay
    pla
    tax
    pla
    jmp ROM_POST_DOSINI

asm_reinit_vera:
    lda #0
    sta CRITIC

    lda _vera_ctl_block + VERACTL_FLAGS
    and #VERA_CTL_FLAG_METRONOME
    ora #VERA_CTL_FLAG_API_READY
    sta _vera_ctl_block + VERACTL_FLAGS

    lda #'V'
    sta _vera_ctl_block + VERACTL_SIG0
    lda #'C'
    sta _vera_ctl_block + VERACTL_SIG1
    lda #'T'
    sta _vera_ctl_block + VERACTL_SIG2
    lda #'L'
    sta _vera_ctl_block + VERACTL_SIG3

    lda #$00
    sta _vera_ctl_block + VERACTL_REQUEST
    sta _vera_ctl_block + VERACTL_PARAM0
    sta _vera_ctl_block + VERACTL_PARAM1
    sta _vera_ctl_block + VERACTL_CURSOR_X
    lda #10
    sta _vera_ctl_block + VERACTL_CURSOR_Y

    lda #<_CallVeraApiService
    sta _vera_ctl_block + VERACTL_ENTRY_LO
    lda #>_CallVeraApiService
    sta _vera_ctl_block + VERACTL_ENTRY_HI

    lda #$00
    sta VERA_ADDR_L
    lda #(VERA_SCREEN_BASE_M + 8)
    sta VERA_ADDR_M
    lda #VERA_SCREEN_BANK
    sta VERA_ADDR_H

    ldx #0
@write_ready:
    lda ready_text,x
    beq @done_ready
    sta VERA_DATA0
    lda #VERA_TEXT_COLOR
    sta VERA_DATA0
    inx
    bne @write_ready
@done_ready:
    rts

swap_to_cc65_zp:
    ldx #CC65_ZP_SIZE - 1
@loop:
    lda c_sp,x
    sta os_saved_zp,x
    lda vera_saved_zp,x
    sta c_sp,x
    dex
    bpl @loop
    rts

swap_to_os_zp:
    ldx #CC65_ZP_SIZE - 1
@loop:
    lda c_sp,x
    sta vera_saved_zp,x
    lda os_saved_zp,x
    sta c_sp,x
    dex
    bpl @loop
    rts

ready_text:
    .asciiz "DEVICE HANDLER READY"
