; vera_sys_loader.s — one-shot bootstrap that installs the relocatable VERA.SYS body.
;
; AUTORUN.SYS layout produced by assemble_autorun.py:
;   body bytes  → loaded at BODY_SOURCE  (fixed, $4000)
;   fixup table → loaded right after body, at PATCH_FIXUP_TABLE
;   loader code → loaded at $5000 (this file)
;
; At install time:
;   1. read MEMLO (current low end of free RAM)
;   2. copy body file bytes to that address
;   3. for each entry in the fixup table, add (MEMLO - NOMINAL_BASE) to the
;      16-bit pointer at body+offset — patches every internal absolute ref
;   4. populate the VCTL block (last 16 bytes of the resident image) using
;      the EXPORTS pointer table at body+0
;   5. install DOSINI/CASINI hooks; save the previous vectors into the
;      relocated _vera_saved_dosini/_vera_saved_casini slots
;   6. SETVBV(7) with the relocated _vbi_handler address
;   7. jsr the relocated _vera_warm_reinit to draw the banner
;   8. bump MEMLO and rts back to DOS
;
; Patch constants live at fixed offsets at the top of the binary;
; assemble_autorun.py overwrites them after linking.

    .setcpu "6502"

; ============================================================================
; OS equates
; ============================================================================

MEMLO       = $02E7
DOSINI      = $000C
CASINI      = $0002
SETVBV      = $E45C
COLBK       = $D01A         ; GTIA background/border colour register
COLOR4      = $02C8         ; OS shadow of COLBK (restored each VBI)

; ============================================================================
; ZP scratch (safe — bootstrap runs before BASIC starts)
; ============================================================================

dest_lo     = $80           ; mutated by copy_block; do not use afterwards
dest_hi     = $81           ; ditto — use exp_lo/hi for the saved base
src_lo      = $82
src_hi      = $83           ; mutated by copy_block
fixup_lo    = $84
fixup_hi    = $85
target_lo   = $86
target_hi   = $87
delta_lo    = $88
delta_hi    = $89
count_lo    = $8A
count_hi    = $8B
exp_lo      = $8C           ; canonical "dest_base" pointer (= MEMLO at entry)
exp_hi      = $8D

; ============================================================================
; Build-time constants
; ============================================================================

BODY_SOURCE   = $4000
NOMINAL_BASE  = $A000

; Minimum address at which the resident driver may live. DOS 2.0s / 2.5 /
; MyDOS load DUP.SYS at the fixed range $1F0C-$3305; any resident below
; $3306 gets overwritten the moment the user types DOS. Floor at $3400 to
; clear that range with a small safety margin.
; The $4000-$7FFF window is reserved by the 130XE / 320XE / 576XE / RAMBO
; bank-switching hardware, so this floor never bumps past $3FFF in
; practice — MEMLO above $3400 means DOS plus other handlers already
; consumed everything below, and we just sit right on top of MEMLO.
MIN_DEST_HI   = $34

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

; ============================================================================
; Patch constants — assemble_autorun.py overwrites these after linking. They
; MUST stay at the very start of the loader so the patcher finds them at a
; known offset (also exported via the loader map file).
; ============================================================================

    .export PATCH_BODY_FILE_SIZE, PATCH_BODY_TOTAL_SIZE
    .export PATCH_FIXUP_TABLE
    .export bootstrap_entry

PATCH_BODY_FILE_SIZE:    .word $0000
PATCH_BODY_TOTAL_SIZE:   .word $0000
PATCH_FIXUP_TABLE:       .word $0000

; ============================================================================
; bootstrap_entry — installer entry point (called via AUTORUN trailer)
; ============================================================================

bootstrap_entry:
    ; --- 1. Pick a destination address: max(MEMLO, $3400). ---
    ;       Floor justification — see MIN_DEST_HI commentary above.
    lda MEMLO
    sta dest_lo
    lda MEMLO+1
    cmp #MIN_DEST_HI
    bcs @keep_memlo             ; MEMLO_hi >= $34 → use MEMLO as-is
    lda #$00
    sta dest_lo
    lda #MIN_DEST_HI
@keep_memlo:
    sta dest_hi
    ; Stash the resolved base into exp_* for the rest of the bootstrap;
    ; copy_block clobbers dest_*.
    lda dest_lo
    sta exp_lo
    lda dest_hi
    sta exp_hi

    ; --- Safety: destination must be below BODY_SOURCE so a forward copy
    ;     doesn't smash the source mid-flight. With the $3400 floor and
    ;     the $4000-$7FFF bank-switch zone reserved, this is structurally
    ;     guaranteed; the check is belt-and-braces. ---
    lda exp_hi
    cmp #>BODY_SOURCE
    bcc @safe
    rts                         ; bail out silently

@safe:
    ; --- 2. Copy body file bytes from BODY_SOURCE to (exp_*). ---
    lda #<BODY_SOURCE
    sta src_lo
    lda #>BODY_SOURCE
    sta src_hi

    lda PATCH_BODY_FILE_SIZE
    sta count_lo
    lda PATCH_BODY_FILE_SIZE+1
    sta count_hi

    jsr copy_block
    ; src_hi/dest_hi are now trashed; use exp_lo/hi from here on.

    ; --- 3. Compute delta = exp_base - NOMINAL_BASE ---
    sec
    lda exp_lo
    sbc #<NOMINAL_BASE
    sta delta_lo
    lda exp_hi
    sbc #>NOMINAL_BASE
    sta delta_hi

    ; --- 4. Walk the fixup table, patching every recorded pointer. ---
    lda PATCH_FIXUP_TABLE
    sta fixup_lo
    lda PATCH_FIXUP_TABLE+1
    sta fixup_hi

@fixup_loop:
    ldy #0
    lda (fixup_lo),y
    sta target_lo               ; offset LO
    iny
    lda (fixup_lo),y
    sta target_hi               ; offset HI

    ; Advance fixup_ptr by 2.
    lda fixup_lo
    clc
    adc #2
    sta fixup_lo
    bcc @check_term
    inc fixup_hi
@check_term:
    ; Terminator = $FFFF.
    lda target_lo
    and target_hi
    cmp #$FF
    beq @fixups_done

    ; target_addr = exp_base + offset (offset currently in target_lo/hi).
    clc
    lda target_lo
    adc exp_lo
    sta target_lo
    lda target_hi
    adc exp_hi
    sta target_hi

    ; Read 16-bit value at *target, add delta, write back.
    ldy #0
    lda (target_lo),y
    clc
    adc delta_lo
    sta (target_lo),y
    iny
    lda (target_lo),y
    adc delta_hi
    sta (target_lo),y
    sta COLBK                 ; raster bar continues during fixup pass

    jmp @fixup_loop

@fixups_done:
    ; --- 5. Initialize VCTL block at body[EXP_VCTL_BLOCK]. ---
    ldy #EXP_VCTL_BLOCK
    lda (exp_lo),y
    sta target_lo
    iny
    lda (exp_lo),y
    sta target_hi
    ; (target_lo,target_hi) now points at the 16-byte VCTL slot.

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

    ; EXP_API_SERVICE → VCTL_ENTRY
    ldy #EXP_API_SERVICE
    lda (exp_lo),y
    ldy #VCTL_ENTRY_LO
    sta (target_lo),y
    ldy #EXP_API_SERVICE+1
    lda (exp_lo),y
    ldy #VCTL_ENTRY_HI
    sta (target_lo),y

    ; EXP_VBI_HANDLER → VCTL_VBI
    ldy #EXP_VBI_HANDLER
    lda (exp_lo),y
    ldy #VCTL_VBI_LO
    sta (target_lo),y
    ldy #EXP_VBI_HANDLER+1
    lda (exp_lo),y
    ldy #VCTL_VBI_HI
    sta (target_lo),y

    ; EXP_WARM_START → VCTL_REINIT
    ldy #EXP_WARM_START
    lda (exp_lo),y
    ldy #VCTL_REINIT_LO
    sta (target_lo),y
    ldy #EXP_WARM_START+1
    lda (exp_lo),y
    ldy #VCTL_REINIT_HI
    sta (target_lo),y

    ; --- 6. Save current DOSINI/CASINI into the relocated saved-vector slots,
    ;       then install our hooks. ---
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

    ; Install DOSINI = relocated _vera_dosini_asm_hook.
    ldy #EXP_DOSINI_HOOK
    lda (exp_lo),y
    sta DOSINI
    iny
    lda (exp_lo),y
    sta DOSINI+1

    ; Install CASINI = relocated _vera_casini_asm_hook.
    ldy #EXP_CASINI_HOOK
    lda (exp_lo),y
    sta CASINI
    iny
    lda (exp_lo),y
    sta CASINI+1

    ; --- 7. Run relocated _InitVbi — it does SETVBV(7) AND initializes the
    ;        cursor/metronome counters in LOWBSS. Skipping the counter init
    ;        is what makes the cursor "wake up" only after a random number
    ;        of frames (whatever garbage cursor_frames happened to hold). ---
    ldy #EXP_INIT_VBI
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_INIT_VBI+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- 8. Bump MEMLO past the resident block. ---
    clc
    lda exp_lo
    adc PATCH_BODY_TOTAL_SIZE
    sta MEMLO
    lda exp_hi
    adc PATCH_BODY_TOTAL_SIZE+1
    sta MEMLO+1

    ; --- 9. Call relocated _vera_warm_reinit (banner + font upload). ---
    ldy #EXP_WARM_REINIT
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_WARM_REINIT+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- 10. Install E:/S: HATABS hooks so PRINT mirrors to VERA. ---
    ldy #EXP_INSTALL_ES
    lda (exp_lo),y
    sta jmp_vec
    ldy #EXP_INSTALL_ES+1
    lda (exp_lo),y
    sta jmp_vec+1
    jsr trampoline

    ; --- Restore the border to whatever colour the OS expects, otherwise
    ;     the last raster-bar byte stays on screen for ~1 frame before the
    ;     next VBI copies COLOR4 → COLBK on its own. ---
    lda COLOR4
    sta COLBK

    rts

; ============================================================================
; trampoline — indirect jsr through jmp_vec.
; ============================================================================

trampoline:
    jmp (jmp_vec)

jmp_vec:
    .word $0000

; ============================================================================
; copy_block — copy (count_lo,count_hi) bytes from (src_lo,src_hi) to
; (dest_lo,dest_hi). Forward copy; trashes dest_hi/src_hi/X/Y.
; ============================================================================

copy_block:
    ldx count_hi
    beq @tail
@page:
    ldy #0
@inner:
    lda (src_lo),y
    sta (dest_lo),y
    sta COLBK                 ; decruncher-style raster bar
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
