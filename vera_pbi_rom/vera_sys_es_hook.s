    .setcpu "6502"

    .export _vera_editrv, _vera_screnv
    .export _vera_orig_editor_put, _vera_orig_screen_put
    .export _vera_hook_col_before, _vera_hook_row_before
    .export _VeraEditorPutHook, _VeraScreenPutHook

    .import _CallVeraApiService

CRITIC                  = $42
ROWCRS                  = $54
COLCRS                  = $55

VERA_CTL_BASE           = $8000
VERA_CTL_SIG0           = VERA_CTL_BASE + 0
VERA_CTL_SIG1           = VERA_CTL_BASE + 1
VERA_CTL_SIG2           = VERA_CTL_BASE + 2
VERA_CTL_SIG3           = VERA_CTL_BASE + 3
VERA_CTL_FLAGS          = VERA_CTL_BASE + 4
VERA_CTL_REQUEST        = VERA_CTL_BASE + 5
VERA_CTL_PARAM0         = VERA_CTL_BASE + 6

VERA_CTL_FLAG_API_READY = $80

VERA_REQ_CLEAR          = $01
VERA_REQ_PUTC           = $03
VERA_REQ_HOOK_PUTC      = $06

    .segment "LOWBSS"

_vera_editrv:
    .res 16

_vera_screnv:
    .res 16

_vera_orig_editor_put:
    .res 2

_vera_orig_screen_put:
    .res 2

_vera_hook_col_before:
    .res 1

_vera_hook_row_before:
    .res 1

vera_hook_char:
    .res 1

vera_hook_a:
    .res 1

vera_hook_x:
    .res 1

vera_hook_y:
    .res 1

vera_hook_critic:
    .res 1

    .segment "CODE"

.proc VeraMirrorHookChar
    lda VERA_CTL_SIG0
    cmp #'V'
    bne xit
    lda VERA_CTL_SIG1
    cmp #'C'
    bne xit
    lda VERA_CTL_SIG2
    cmp #'T'
    bne xit
    lda VERA_CTL_SIG3
    cmp #'L'
    bne xit
    lda VERA_CTL_FLAGS
    and #VERA_CTL_FLAG_API_READY
    beq xit

    lda CRITIC
    sta vera_hook_critic
    lda #$01
    sta CRITIC

    lda vera_hook_char
    cmp #$7D
    bne not_clear
    lda #VERA_REQ_CLEAR
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService
    jmp restore_critic

not_clear:
    lda vera_hook_char
    sta VERA_CTL_PARAM0
    lda #VERA_REQ_HOOK_PUTC
    sta VERA_CTL_REQUEST
    jsr _CallVeraApiService

restore_critic:
    lda vera_hook_critic
    sta CRITIC
xit:
    rts
.endproc

.proc _VeraEditorPutHook
    sta vera_hook_char
    lda COLCRS
    sta _vera_hook_col_before
    lda ROWCRS
    sta _vera_hook_row_before
    lda #>(editor_return-1)
    pha
    lda #<(editor_return-1)
    pha
    lda vera_hook_char
    jmp (_vera_orig_editor_put)
editor_return:
    sta vera_hook_a
    stx vera_hook_x
    sty vera_hook_y
    tya
    bmi xit
    jsr VeraMirrorHookChar
xit:
    ldy vera_hook_y
    ldx vera_hook_x
    lda vera_hook_a
    rts
.endproc

.proc _VeraScreenPutHook
    sta vera_hook_char
    lda COLCRS
    sta _vera_hook_col_before
    lda ROWCRS
    sta _vera_hook_row_before
    lda #>(screen_return-1)
    pha
    lda #<(screen_return-1)
    pha
    lda vera_hook_char
    jmp (_vera_orig_screen_put)
screen_return:
    sta vera_hook_a
    stx vera_hook_x
    sty vera_hook_y
    tya
    bmi xit
    jsr VeraMirrorHookChar
xit:
    ldy vera_hook_y
    ldx vera_hook_x
    lda vera_hook_a
    rts
.endproc
