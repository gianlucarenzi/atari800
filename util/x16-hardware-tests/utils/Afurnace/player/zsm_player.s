; ZSM Player for Atari PBI VERA
; Minimal footprint (~2KB) 6502 assembler
; Loaded at $3000, plays PSG from ZSM file in memory

.feature labels_without_colons

; ===== MEMORY MAP =====
; $0000-$00FF:  ZP (available: $80-$FF for us, ~128 bytes)
; $0300-$03FF:  Player state
; $2000-$3FFF:  ZSM data (music)
; $3000-$37FF:  Player code (max 2KB)

; ===== ZERO PAGE =====
ZSM_PTR_LO      = $80   ; Current read position in ZSM data
ZSM_PTR_HI      = $81
LOOP_PTR_LO     = $82   ; Loop start position (0=no loop)
LOOP_PTR_HI     = $83
TICK_COUNT      = $84   ; Ticks elapsed since last delay
TICK_TARGET     = $85   ; Target ticks until next command
TEMPO           = $86   ; Tempo divider (default 1, use for slow/fast)
FLAGS           = $87   ; Player state flags (bit 0=playing, 1=looped)
CH_MASK         = $88   ; Active channels mask (to mute channels)
; $89-$8F: free

; ===== STATE PAGE ($0300) =====
PSG_STATE       = $0300 ; 64 bytes: PSG channel state (freq, vol, waveform)

; ===== VERA REGISTERS =====
VERA_ADDR_L     = $D100
VERA_ADDR_M     = $D101
VERA_ADDR_H     = $D102
VERA_DATA0      = $D103
VERA_DATA1      = $D104
VERA_CTRL       = $D105

; PSG register base
PSG_BASE        = $D100

; ===== CODE =====

.segment "CODE"
.org $3000

; Entry point: Initialize player
; Input: A=low byte of ZSM data address, X=high byte
INIT_PLAYER
	sta ZSM_PTR_LO
	stx ZSM_PTR_HI
	
	; Verify ZSM magic ("ZSM")
	jsr READ_BYTE
	cmp #$7A
	bne BadMagic
	jsr READ_BYTE
	cmp #$53
	bne BadMagic
	jsr READ_BYTE
	cmp #$4D
	bne BadMagic
	
	; Read version (ignore for now)
	jsr READ_BYTE
	
	; Read loop pointer
	jsr READ_BYTE
	sta LOOP_PTR_LO
	jsr READ_BYTE
	sta LOOP_PTR_HI
	
	; Skip reserved bytes
	jsr READ_BYTE
	jsr READ_BYTE
	
	; Initialize state
	lda #0
	sta TICK_COUNT
	sta FLAGS
	lda #1
	sta TEMPO
	lda #$FF
	sta CH_MASK
	
	; Clear PSG state
	ldx #63
	lda #0
ClrState
	sta PSG_STATE,x
	dex
	bpl ClrState
	
	; Mark as playing
	lda #$01
	sta FLAGS
	rts

BadMagic
	lda #$00
	sta FLAGS
	rts

; ===== VBI TICK HANDLER =====
; Call this once per VBI (50/60 Hz)
; Processes next commands due this tick
TICK
	bit FLAGS
	bpl NotPlaying
	
	inc TICK_COUNT
	lda TICK_COUNT
	cmp TICK_TARGET
	bcc NotDue
	
	; Process next command
	jsr PROCESS_CMD
	jmp TICK

NotDue
NotPlaying
	rts

; ===== PROCESS ONE COMMAND =====
PROCESS_CMD
	jsr READ_BYTE
	bmi IsWrite     ; Bit 7 set = VERA write
	
	; Delay command
	beq EndTrack
	; Mask off command bit (bit 7=0), remaining 7 bits = delay count
	and #$7F
	sta TICK_TARGET
	lda #0
	sta TICK_COUNT
	rts

EndTrack
	; End of track
	bit LOOP_PTR_LO
	bpl NoLoop
	; Loop: restore position
	lda LOOP_PTR_LO
	sta ZSM_PTR_LO
	lda LOOP_PTR_HI
	sta ZSM_PTR_HI
	lda #$01
	sta FLAGS
	lda #1
	sta TICK_TARGET
	rts

NoLoop
	; No loop: stop playback
	lda #$00
	sta FLAGS
	rts

IsWrite
	; VERA write: remaining 7 bits = register offset
	and #$7F
	
	; Read value byte
	pha              ; Save register offset
	jsr READ_BYTE
	sta VERA_DATA0   ; Temporarily store value in A
	pla
	
	; Convert offset to VERA address
	; Offset 0x00-0x3F = PSG $D100-$D1FF (scaled)
	; Offset 0x40-0x7F = other registers
	cmp #$40
	bcs OtherReg
	
	; PSG register: write to $D100 + offset
	pha
	lda #0
	sta VERA_ADDR_M
	sta VERA_ADDR_H
	pla
	sta VERA_ADDR_L
	
	; Write value (in A from stack)
	lda VERA_DATA0   ; Restore value
	sta VERA_DATA0
	rts

OtherReg
	; Other register: handle specially if needed
	; For now, just write to $D1XX where XX = offset
	pha
	lda #0
	sta VERA_ADDR_M
	lda #0           ; Bank 0
	sta VERA_ADDR_H
	pla
	sta VERA_ADDR_L
	lda VERA_DATA0
	sta VERA_DATA0
	rts

; ===== READ NEXT BYTE FROM ZSM =====
; Returns byte in A, increments ZSM_PTR
READ_BYTE
	ldy #0
	lda (ZSM_PTR_LO),y
	inc ZSM_PTR_LO
	bne NoWrap
	inc ZSM_PTR_HI
NoWrap
	rts

; ===== MUTE/UNMUTE CHANNEL =====
; Input: X = channel (0-15)
; A = 0 to mute, non-zero to unmute
MUTE_CHANNEL
	beq MuteIt
	lda #1
	jmp DoMask
MuteIt
	lda #0
DoMask
	; Set/clear bit X in CH_MASK
	; (simplified: assumes X < 8 for now)
	rts

; ===== STOP PLAYBACK =====
STOP
	lda #$00
	sta FLAGS
	rts

; ===== RESTART =====
RESTART
	; Reset pointer to after header (offset +8)
	lda ZSM_PTR_LO
	sec
	sbc #8
	sta ZSM_PTR_LO
	lda ZSM_PTR_HI
	sbc #0
	sta ZSM_PTR_HI
	lda #1
	sta FLAGS
	rts
