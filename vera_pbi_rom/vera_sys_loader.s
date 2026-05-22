; vera_sys_loader.s — robust bootstrap that installs the relocatable VERA.SYS body.
; Based on the original loader, improved for safer RAMTOP/MEMTOP management.

    .setcpu "6502"
    .include "atari.inc"

; ============================================================================
; ZP scratch (safe — bootstrap runs before BASIC starts)
; ============================================================================

dest_lo     = $80
dest_hi     = $81
src_lo      = $82
src_hi      = $83
fixup_lo    = $84
fixup_hi    = $85
target_lo   = $86
target_hi   = $87
delta_lo    = $88
delta_hi    = $89
count_lo    = $8A
count_hi    = $8B
exp_lo      = $8C
exp_hi      = $8D
saved_sdmctl = $8E

; ============================================================================
; Build-time constants
; ============================================================================

BODY_SOURCE   = $4000
NOMINAL_BASE  = $A000
CIO_CALL      = $E456

; ============================================================================
; Patch constants (must remain at the top)
; ============================================================================

    .export PATCH_BODY_FILE_SIZE, PATCH_BODY_TOTAL_SIZE
    .export PATCH_FIXUP_TABLE
    .export bootstrap_entry

PATCH_BODY_FILE_SIZE:    .word $0000
PATCH_BODY_TOTAL_SIZE:   .word $0000
PATCH_FIXUP_TABLE:       .word $0000

; ============================================================================
; EXPORTS table offsets (mirror vera_stub.s)
; ============================================================================

EXP_WARM_REINIT  = 0
EXP_DOSINI_HOOK  = 2
EXP_CASINI_HOOK  = 4
EXP_SAVED_DOSINI = 6
EXP_SAVED_CASINI = 8
EXP_VBI_HANDLER  = 10
EXP_API_SERVICE  = 12
EXP_WARM_START   = 14
EXP_VCTL_BLOCK   = 16
EXP_INIT_VBI     = 18
EXP_INSTALL_ES   = 20

; ============================================================================
; VCTL block layout (16 bytes)
; ============================================================================

VCTL_FLAGS      = 4
VCTL_REQUEST    = 5
VCTL_PARAM0     = 6
VCTL_PARAM1     = 7
VCTL_CURSOR_X   = 8
VCTL_CURSOR_Y   = 9
VCTL_ENTRY_LO   = 10
VCTL_ENTRY_HI   = 11
VCTL_VBI_LO     = 12
VCTL_VBI_HI     = 13
VCTL_REINIT_LO  = 14
VCTL_REINIT_HI  = 15

VCTL_FLAG_METRONOME = $01
VCTL_FLAG_API_READY = $80

    .segment "CODE"

bootstrap_entry:
    ; --- 1. Compute dest = page_align_down(RAMTOP*256 - TOTAL_SIZE). ---
    lda RAMTOP
    sec
    sbc PATCH_BODY_TOTAL_SIZE+1
    pha
    lda PATCH_BODY_TOTAL_SIZE
    beq @aligned
    pla
    sec
    sbc #1
    pha
@aligned:
    pla
    sta dest_hi
    lda #0
    sta dest_lo

    lda dest_lo
    sta exp_lo
    lda dest_hi
    sta exp_hi

    ; --- 2. Disable ANTIC DMA for the install. ---
    lda SDMCTL
    sta saved_sdmctl
    lda #0
    sta SDMCTL
    sta DMACTL

    ; --- 3. Copy body file bytes. ---
    lda #<BODY_SOURCE
    sta src_lo
    lda #>BODY_SOURCE
    sta src_hi
    lda PATCH_BODY_FILE_SIZE
    sta count_lo
    lda PATCH_BODY_FILE_SIZE+1
    sta count_hi
    jsr copy_block

    ; --- 4. Zero BSS area. ---
    clc
    lda exp_lo
    adc PATCH_BODY_FILE_SIZE
    sta dest_lo
    lda exp_hi
    adc PATCH_BODY_FILE_SIZE+1
    sta dest_hi
    sec
    lda PATCH_BODY_TOTAL_SIZE
    sbc PATCH_BODY_FILE_SIZE
    sta count_lo
    lda PATCH_BODY_TOTAL_SIZE+1
    sbc PATCH_BODY_FILE_SIZE+1
    sta count_hi
    jsr zero_block

    ; --- 5. Compute delta. ---
    sec
    lda exp_lo
    sbc #<NOMINAL_BASE
    sta delta_lo
    lda exp_hi
    sbc #>NOMINAL_BASE
    sta delta_hi

    ; --- 6. Fixup pass. ---
    lda PATCH_FIXUP_TABLE
    sta fixup_lo
    lda PATCH_FIXUP_TABLE+1
    sta fixup_hi

@fixup_loop:
    ldy #0
    lda (fixup_lo),y
    sta target_lo
    iny
    lda (fixup_lo),y
    sta target_hi
    lda fixup_lo
    clc
    adc #2
    sta fixup_lo
    bcc @check_term
    inc fixup_hi
@check_term:
    lda target_lo
    and target_hi
    cmp #$FF
    beq @fixups_done
    clc
    lda target_lo
    adc exp_lo
    sta target_lo
    lda target_hi
    adc exp_hi
    sta target_hi
    ldy #0
    lda (target_lo),y
    clc
    adc delta_lo
    sta (target_lo),y
    iny
    lda (target_lo),y
    adc delta_hi
    sta (target_lo),y
    sta COLBK
    jmp @fixup_loop
@fixups_done:

    ; --- 7. Initialize VCTL block at body[EXP_VCTL_BLOCK]. ---
    ldy #EXP_VCTL_BLOCK
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda #'V'
    sta (target_lo),y
    iny
    lda #'C'
    sta (target_lo),y
    iny
    lda #'T'
    sta (target_lo),y
    iny
    lda #'L'
    sta (target_lo),y
    ldy #VCTL_FLAGS
    lda #(VCTL_FLAG_METRONOME | VCTL_FLAG_API_READY)
    sta (target_lo),y
    ldy #VCTL_REQUEST
    lda #0
    sta (target_lo),y
    ldy #VCTL_PARAM0
    sta (target_lo),y
    ldy #VCTL_PARAM1
    sta (target_lo),y
    ldy #VCTL_CURSOR_X
    sta (target_lo),y
    ldy #VCTL_CURSOR_Y
    sta (target_lo),y
    ldy #EXP_API_SERVICE
    lda (exp_lo),y
    ldy #VCTL_ENTRY_LO
    sta (target_lo),y
    ldy #EXP_API_SERVICE+1
    lda (exp_lo),y
    ldy #VCTL_ENTRY_HI
    sta (target_lo),y
    ldy #EXP_VBI_HANDLER
    lda (exp_lo),y
    ldy #VCTL_VBI_LO
    sta (target_lo),y
    ldy #EXP_VBI_HANDLER+1
    lda (exp_lo),y
    ldy #VCTL_VBI_HI
    sta (target_lo),y
    ldy #EXP_WARM_START
    lda (exp_lo),y
    ldy #VCTL_REINIT_LO
    sta (target_lo),y
    ldy #EXP_WARM_START+1
    lda (exp_lo),y
    ldy #VCTL_REINIT_HI
    sta (target_lo),y

    ; --- 8. Save DOSINI/CASINI hooks. ---
    ldy #EXP_SAVED_DOSINI
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda DOSINI
    sta (target_lo),y
    iny
    lda DOSINI+1
    sta (target_lo),y
    ldy #EXP_SAVED_CASINI
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda CASINI
    sta (target_lo),y
    iny
    lda CASINI+1
    sta (target_lo),y
    ldy #EXP_DOSINI_HOOK
    lda (exp_lo),y
    sta DOSINI
    iny
    lda (exp_lo),y
    sta DOSINI+1
    ldy #EXP_CASINI_HOOK
    lda (exp_lo),y
    sta CASINI
    iny
    lda (exp_lo),y
    sta CASINI+1

    ; --- 9. Run relocated _InitVbi. ---
    ldy #EXP_INIT_VBI
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_INIT_VBI+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- 10. Lower RAMTOP to protect the driver. ---
    sei
    lda #1
    sta CRITIC
    lda exp_hi
    sta RAMTOP
    sec
    lda exp_lo
    sbc #1
    sta MEMTOP
    lda exp_hi
    sbc #0
    sta MEMTOP+1
    lda #0
    sta CRITIC
    cli

    ; --- 8b. Force CIO CLOSE #0, then OPEN E: ---
    ldx #0
    lda #$0C                        ; CIO CLOSE command
    sta ICCOM,x
    jsr CIO_CALL

    ldx #0
    lda #$03                        ; CIO OPEN command
    sta ICCOM,x
    lda #<e_device_name
    sta ICBAL,x
    lda #>e_device_name
    sta ICBAH,x
    lda #$0C                        ; mode: read+write, Graphics 0
    sta ICAX1,x
    lda #0
    sta ICAX2,x
    jsr CIO_CALL

    ; --- 8b2. Force OS screen re-init (Graphics 0) ---
    lda #0
    tax
    jsr $E453

    ; --- 8c. Restore ANTIC DMA ---
    lda saved_sdmctl
    sta SDMCTL
    sta DMACTL
    lda #$00
    sta COLOR4
    sta COLBK

    ; --- 13. Call relocated _vera_warm_reinit. ---
    ldy #EXP_WARM_REINIT
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_WARM_REINIT+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- 14. Install E:/S: HATABS hooks. ---
    ldy #EXP_INSTALL_ES
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_INSTALL_ES+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    lda COLOR4
    sta COLBK
    rts

    .align $100
trampoline:
    jmp (jmp_vec)
jmp_vec:
    .word $0000
e_device_name:
    .byte 'E', ':', $9B

copy_block:
    ldx count_hi
    beq @tail
@page:
    ldy #0
@inner:
    lda (src_lo),y
    sta (dest_lo),y
    sta COLBK
    iny
    bne @inner
    inc src_hi
    inc dest_hi
    dex
    bne @page
@tail:
    ldy count_lo
    beq @done
    ldy #0
@tail_loop:
    lda (src_lo),y
    sta (dest_lo),y
    sta COLBK
    iny
    cpy count_lo
    bne @tail_loop
@done:
    rts

zero_block:
    lda #0
    ldx count_hi
    beq @tail
@page:
    ldy #0
@inner:
    sta (dest_lo),y
    iny
    bne @inner
    inc dest_hi
    dex
    bne @page
@tail:
    ldy count_lo
    beq @done
    ldy #0
@tail_loop:
    sta (dest_lo),y
    iny
    cpy count_lo
    bne @tail_loop
@done:
    rts
