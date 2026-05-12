    .setcpu "6502"

    .export _vera_editrv, _vera_screnv
    .export _vera_orig_editor_put, _vera_orig_screen_put
    .export _VeraPutByte
    .export _vera_saved_dosini

    .import _CallVeraApiService

VERA_CTL_BASE    = $8000
VERA_CTL_REQUEST = VERA_CTL_BASE + 5
VERA_CTL_PARAM0  = VERA_CTL_BASE + 6

VERA_REQ_PUTC    = $03

CRITIC           = $42

    .segment "LOWBSS"

_vera_editrv:
    .res 16

_vera_screnv:
    .res 16

_vera_orig_editor_put:
    .res 2

_vera_orig_screen_put:
    .res 2

; Old DOSINI value chained by vera_dosini_hook.
; Survives warm start (LOWBSS is in $8000+ RAM, untouched by OS warm start).
_vera_saved_dosini:
    .res 2

    .segment "CODE"

; CIO PUT BYTE handler shared by E: and S: — routes directly to VERA output.
; On entry: A = ATASCII character, X = IOCB index * 16, CRITIC = 1 (set by CIO)
; On exit:  Y = 1, C = 1 (CIO success), CRITIC = 0
.proc _VeraPutByte
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService     ; CRITIC still 1 during VRAM write (blocks VBI cursor)
    lda #$00
    sta CRITIC                  ; re-enable deferred VBI (CIO never clears CRITIC itself)
    ldy #1
    sec
    rts
.endproc
