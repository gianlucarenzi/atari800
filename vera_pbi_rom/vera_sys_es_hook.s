    .setcpu "6502"

    .export _vera_editrv, _vera_screnv
    .export _vera_orig_editor_put, _vera_orig_screen_put
    .export _VeraPutByte

    .import _CallVeraApiService

VERA_CTL_BASE    = $8000
VERA_CTL_REQUEST = VERA_CTL_BASE + 5
VERA_CTL_PARAM0  = VERA_CTL_BASE + 6

VERA_REQ_PUTC    = $03

    .segment "LOWBSS"

_vera_editrv:
    .res 16

_vera_screnv:
    .res 16

_vera_orig_editor_put:
    .res 2

_vera_orig_screen_put:
    .res 2

; Handler table reserved for future K: replacement
_vera_kbdrv:
    .res 16

    .segment "CODE"

; CIO PUT BYTE handler shared by E: and S: — routes directly to VERA output.
; On entry: A = ATASCII character, X = IOCB index * 16
; On exit:  Y = 1, C = 1 (CIO success)
.proc _VeraPutByte
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    ldy #1
    sec
    rts
.endproc
