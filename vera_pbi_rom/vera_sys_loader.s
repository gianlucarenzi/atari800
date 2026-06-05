; vera_sys_loader.s — robust bootstrap that installs the relocatable VERA.SYS body.
;
; AUTORUN.SYS layout produced by assemble_autorun.py:
;   body bytes  → loaded at BODY_SOURCE  (fixed, $4000)
;   fixup table → loaded right after body, at PATCH_FIXUP_TABLE
;   loader code → loaded at $6000 (this file)
;
; Patch constants live at fixed offsets at the top of the binary;
; assemble_autorun.py overwrites them after linking.

    .setcpu "6502"
    .include "atari.inc"

; Screen dimensions — replicated from vera_common.inc to avoid symbol clashes
.ifdef MODE_40X30
SCREEN_COLS_VIEW = 40
SCREEN_ROWS_VIEW = 30
.elseif .defined(FONT_8X8)
SCREEN_COLS_VIEW = 80
SCREEN_ROWS_VIEW = 60
.else
SCREEN_COLS_VIEW = 80
SCREEN_ROWS_VIEW = 30
.endif

; ============================================================================
; ZP scratch
; ============================================================================

dest_lo      = $80
dest_hi      = $81
src_lo       = $82
src_hi       = $83
fixup_lo     = $84
fixup_hi     = $85
target_lo    = $86
target_hi    = $87
delta_lo     = $88
delta_hi     = $89
count_lo     = $8A
count_hi     = $8B
exp_lo       = $8C
exp_hi       = $8D
saved_sdmctl = $8E
tmp_dosini   = $90
tmp_casini   = $92
orig_ramtop  = $94

; ============================================================================
; Build-time constants
; ============================================================================

BODY_SOURCE          = $4000
NOMINAL_BASE         = $A000
CIO_CALL             = $E456
MIN_DEST_HI          = $34
; OS places native screen just below RAMTOP*256 (~$9C00 for $A0 RAMTOP).
; Reserve this many pages so the driver ends safely below the screen area.
SCREEN_RESERVE_PAGES = 4

; ============================================================================
; EXPORTS table offsets
; ============================================================================

EXP_WARM_REINIT       = 0
EXP_DOSINI_HOOK       = 2
EXP_CASINI_HOOK       = 4
EXP_SAVED_DOSINI      = 6
EXP_SAVED_CASINI      = 8
EXP_VBI_HANDLER       = 10
EXP_API_SERVICE       = 12
EXP_WARM_START        = 14
EXP_VCTL_BLOCK        = 16
EXP_INIT_VBI          = 18
EXP_INSTALL_ES        = 20
EXP_SAVED_ORIG_RAMTOP = 24
EXP_SAVED_DEST_HI     = 26

; ============================================================================
; VCTL block layout
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
VCTL_SCREEN_COLS = 16   ; viewport width  in chars — read by runcpm
VCTL_SCREEN_ROWS = 17   ; viewport height in chars — read by runcpm

VCTL_FLAG_METRONOME = $01
VCTL_FLAG_API_READY = $80

    .segment "CODE"

    .export PATCH_BODY_FILE_SIZE, PATCH_BODY_TOTAL_SIZE
    .export PATCH_FIXUP_TABLE
    .export bootstrap_entry

PATCH_BODY_FILE_SIZE:    .word $0000
PATCH_BODY_TOTAL_SIZE:   .word $0000
PATCH_FIXUP_TABLE:       .word $0000

; ============================================================================
; bootstrap_entry — installer entry point
; ============================================================================

bootstrap_entry:
    ; --- 0. Save original DOSINI/CASINI vectors BEFORE hooking them. ---
    ; On warm restart DOSINI already points to our hook (re_install_hooks put it
    ; back there).  Read the TRUE original vectors from the existing installed
    ; driver to avoid a chain-to-ourselves loop on the next warm restart.
    lda WARMST
    beq @cold_dosini

    ; Warm restart: RAMTOP was restored by common_reinit before DOS re-ran us.
    ; Peek at the existing installed driver's EXPORTS to get the real saved vectors.
    ; Driver base = page_align_down(RAMTOP*256 - TOTAL_SIZE) - SCREEN_RESERVE_PAGES
    ; Must match step 1 formula exactly.
    lda RAMTOP
    sec
    sbc PATCH_BODY_TOTAL_SIZE+1
    sta dest_hi
    lda PATCH_BODY_TOTAL_SIZE
    beq @warm_no_adj
    dec dest_hi
@warm_no_adj:
    lda dest_hi
    sec
    sbc #SCREEN_RESERVE_PAGES
    sta dest_hi
    lda #0
    sta dest_lo                     ; [dest_hi : 0] = existing driver base
    ldy #EXP_SAVED_DOSINI
    lda (dest_lo),y
    sta target_lo
    iny
    lda (dest_lo),y
    sta target_hi
    ldy #0
    lda (target_lo),y
    sta tmp_dosini
    iny
    lda (target_lo),y
    sta tmp_dosini+1
    ldy #EXP_SAVED_CASINI           ; dest_hi still valid, dest_lo still 0
    lda (dest_lo),y
    sta target_lo
    iny
    lda (dest_lo),y
    sta target_hi
    ldy #0
    lda (target_lo),y
    sta tmp_casini
    iny
    lda (target_lo),y
    sta tmp_casini+1
    jmp @after_dosini

@cold_dosini:
    lda DOSINI
    sta tmp_dosini
    lda DOSINI+1
    sta tmp_dosini+1
    lda CASINI
    sta tmp_casini
    lda CASINI+1
    sta tmp_casini+1
@after_dosini:

    ; --- 1. Compute dest = page_align_down(RAMTOP*256 - TOTAL_SIZE) - SCREEN_RESERVE_PAGES. ---
    ; SCREEN_RESERVE_PAGES leaves a gap so OS ScreenInit/CIO OPEN E: (which runs
    ; before jmp(DOSINI)) places the native screen above the driver, not inside it.
    lda RAMTOP
    sta orig_ramtop                 ; save pre-installation RAMTOP for warm restarts
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
    sec
    sbc #SCREEN_RESERVE_PAGES
    sta dest_hi
    lda #0
    sta dest_lo

    lda dest_lo
    sta exp_lo
    lda dest_hi
    sta exp_hi

    ; Safety: dest must be above MIN_DEST_HI.
    lda exp_hi
    cmp #MIN_DEST_HI
    bcs @safe
    rts

@safe:
    ; --- 2. Disable ANTIC DMA. ---
    lda SDMCTL
    sta saved_sdmctl
    lda #0
    sta SDMCTL
    sta DMACTL

    ; --- 3. Copy body file bytes from BODY_SOURCE to (exp_*). ---
    lda #<BODY_SOURCE
    sta src_lo
    lda #>BODY_SOURCE
    sta src_hi
    lda PATCH_BODY_FILE_SIZE
    sta count_lo
    lda PATCH_BODY_FILE_SIZE+1
    sta count_hi
    jsr copy_block

    ; --- 3b. Zero LOWBSS + VCTL (BSS area). ---
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

    ; --- 4. Compute delta = exp_base - NOMINAL_BASE ---
    sec
    lda exp_lo
    sbc #<NOMINAL_BASE
    sta delta_lo
    lda exp_hi
    sbc #>NOMINAL_BASE
    sta delta_hi

    ; --- 5. Walk the fixup table, patching every recorded pointer. ---
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

    ; --- 6. Initialize VCTL block at body[EXP_VCTL_BLOCK]. ---
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
    lda #$61            ; default text color: fg=white(1) bg=blue(6)
    sta (target_lo),y
    lda #0              ; restore zero for subsequent fields
    
    ldy #VCTL_CURSOR_X
    sta (target_lo),y

    ldy #VCTL_CURSOR_Y
    sta (target_lo),y

    ; Screen dimensions — runcpm reads these to handle 40x24 / 80x30 / 80x60
    ldy #VCTL_SCREEN_COLS
    lda #SCREEN_COLS_VIEW
    sta (target_lo),y

    ldy #VCTL_SCREEN_ROWS
    lda #SCREEN_ROWS_VIEW
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

    ; --- 6b. Transfer original vectors to driver BSS. ---
    ldy #EXP_SAVED_DOSINI
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda tmp_dosini
    sta (target_lo),y
    iny
    lda tmp_dosini+1
    sta (target_lo),y
    
    ldy #EXP_SAVED_CASINI
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda tmp_casini
    sta (target_lo),y
    iny
    lda tmp_casini+1
    sta (target_lo),y

    ; --- 6c. Transfer original RAMTOP to driver BSS. ---
    ldy #EXP_SAVED_ORIG_RAMTOP
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda orig_ramtop
    sta (target_lo),y

    ; --- 6d. Transfer installed driver base page (exp_hi) to driver BSS.
    ;         Used by common_reinit to restore MEMTOP = exp_hi*256 - 1. ---
    ldy #EXP_SAVED_DEST_HI
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ldy #0
    lda exp_hi
    sta (target_lo),y

    ; --- 7. Install DOSINI/CASINI hooks. ---
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

    ; --- 8. Run relocated _InitVbi. ---
    ldy #EXP_INIT_VBI
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_INIT_VBI+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- 9. Lower RAMTOP to protect the driver. ---
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

    ; --- 10. Force CIO CLOSE #0, then OPEN E: ---
    ldx #0
    lda #$0C
    sta ICCOM,x
    jsr CIO_CALL
    ldx #0
    lda #$03
    sta ICCOM,x
    lda #<e_device_name
    sta ICBAL,x
    lda #>e_device_name
    sta ICBAH,x
    lda #$0C
    sta ICAX1,x
    lda #0
    sta ICAX2,x
    jsr CIO_CALL

    ; --- 10b. Force OS screen re-init (Graphics 0) ---
    lda #0
    tax
    jsr $E453

    ; --- 11. Restore ANTIC DMA ---
    lda saved_sdmctl
    sta SDMCTL
    sta DMACTL

    ; --- 12. Black out ANTIC border. ---
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
