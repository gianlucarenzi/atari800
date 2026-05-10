; Minimal Atari XL header
.segment "EXEHDR"
.byte $FF, $FF
.word $2000, $2040

.segment "STARTUP"
start:
    sei
    ; Wait for boot to finish (simple frame count poll)
    ; $14 is the frame counter (RTCLOK)
wait_boot:
    lda $14
    cmp #$50 ; Wait roughly 1 second
    bcc wait_boot

    ; Activate VeraX16 PBI device ($D1FF)
    lda #$80
    sta $D1FF

    ; Set DCSEL = 2 (bits 6:1)
    ; CTRL = (2 << 1) = $04
    lda #$04
    sta $D105

    ; Write to FX_CTRL ($D109)
    lda #$AA
    sta $D109

    ; Read back FX_CTRL
    lda $D109
    cmp #$AA
    beq success

failure:
    lda #$02 ; Red background
    sta $D01A
    jmp failure

success:
    lda #$06 ; Green background
    sta $D01A
    jmp success

.segment "VECTORS"
.addr start
