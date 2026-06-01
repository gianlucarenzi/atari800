; ============================================================================
; SERTERM_HANDLER.S - Low-level SIO & Interrupt Wrappers
; ============================================================================
; This module provides the interface between C code and Atari Hardware.
; It follows the proven architecture of the 'netcat' utility for FujiNet.
; ============================================================================

.include "atari.inc"

; Variables imported from C code
.import _trip               ; Flag set by the interrupt when data is ready

; Functions exported to be called from C
.export _siov               ; Executes a standard SIO transaction
.export _ih                 ; PROCEED line interrupt handler

.segment "CODE"

; ----------------------------------------------------------------------------
; _siov: Wrapper for the OS SIOV vector ($E459)
; ----------------------------------------------------------------------------
; Prepares the execution of an SIO command configured via the DCB (Device
; Control Block).
; ----------------------------------------------------------------------------
_siov:
    jsr SIOV                ; Jump to Operating System SIO routine
    rts                     ; Return to caller

; ----------------------------------------------------------------------------
; _ih: Interrupt Handler for the SIO PROCEED line
; ----------------------------------------------------------------------------
; This is called when the FujiNet asserts the PROCEED line (SIO Pin 8).
; It indicates that new data has arrived from the network or the RunCPM
; emulator has a message for the host.
; ----------------------------------------------------------------------------
_ih:
    ; --- CRITICAL HARDWARE STEP ---
    ; The PROCEED line is physically connected to the CA1 input of the 
    ; PIA chip ($D300). By reading PORTA ($D300), we clear the hardware 
    ; interrupt flag in the PIA. If this read is omitted, the Atari will 
    ; stay stuck in an infinite interrupt loop, causing a system freeze.
    lda $D300               ; Read PORTA to clear PIA IRQ flag
    
    ; Signal the main C loop that FujiNet needs attention
    lda #$01
    sta _trip               ; Set trip = 1
    
    ; --- OS STACK COMPATIBILITY ---
    ; The Atari OS IRQ dispatcher performs a PHA (Push Accumulator) before 
    ; jumping to the VPRCED vector. We must pull it back to maintain 
    ; stack integrity before returning.
    pla                     ; Restore the accumulator saved by the OS
    rti                     ; Return from Interrupt
