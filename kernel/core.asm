; MovieCart demo kernel — RamKernel in RIOT $80 (generate_title_rom shape).
; Cart ROM: ColdStart, RamKernel image, VisibleBars only.
; Visible scanlines: kernel macro + line0..end_lines (unchanged).
;
; Build: make (dasm → core.bin)

	processor 6502
	include vcs.h

RAM_BASE	equ $80
FIELD		equ $F1

GAUDIO	equ #0
NUM_LINES equ	8
PREROLL			equ 50

#if 1	; NTSC
VISIBLE_LINES	equ (192 - NUM_LINES*2 - PREROLL + 1)
GCOL0			equ $42
GCOL5			equ $36
GCOL1			equ $EC
GCOL6			equ $D8
GCOL2			equ $72
GCOL7			equ $64
GCOL3			equ $B8
GCOL8			equ $6C
GCOL4			equ $06
GCOL9			equ $0A
GBKCOLOR		equ $02

#else	; PAL

VISIBLE_LINES	equ (242 - NUM_LINES*2 - PREROLL + 1)
GCOL0			equ $44
GCOL5			equ $46
GCOL1			equ $2C
GCOL6			equ $36
GCOL2			equ $B2
GCOL7			equ $C4
GCOL3			equ $9A
GCOL8			equ $AC
GCOL4			equ $F8
GCOL9			equ $FC
GBKCOLOR		equ $04
#endif


	seg

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	MAC kernel

	; right_line


	lda #{4}		; 2
	sta GRP1		; 3 VDELed
	sta HMOVE		; 3 @03 +8 pixel
	lda #GAUDIO		; 2
	ldx #{10}		; 2
	sta AUDV0		; 3 @10
	ldy #GCOL9		; 2
	lda #GCOL6		; 2
	sta COLUP1		; 3
	lda #{2}		; 2
	sta GRP0		; 3
	lda #GCOL5		; 2
	sta COLUP0		; 3
	lda #{8}		; 2
	sta GRP1		; 3 VDELed
	lda #GBKCOLOR	; 2
	sta COLUBK		; 3 background color
	lda #GCOL7		; 2
	sta COLUP0		; 3 @42! end of GRP0a display
	lda #{6}		; 2
	sta GRP0		; 3 @47! end of GRP1a display
	lda #GCOL8		; 2
	sta COLUP1		; 3 @52
	stx GRP0		; 3 @55
	sty COLUP0		; 3 @58<=@60
	lda #$00		; 2 turn off background color
	sta COLUBK		; 3 background color
	sta HMCLR		; 3


	; left_line

	lda #$00		; 2 dummy
	lda #{3}		; 2
	sta HMOVE		;back 8, late hmove    ;needs to be on cycle 71
	sta GRP1		; 3 VDELed
	lda #GCOL1		; 2
	sta COLUP1		; 3
	lda #GAUDIO		; 2
	sta AUDV0		; 3 @10
	ldx #{9}		; 2
	ldy #GCOL4		; 2
	lda #{1} 		; 2
	sta GRP0		; 3
	lda #GCOL0		; 2
	sta COLUP0		; 3
	lda #{7}		; 2
	sta GRP1		; 3 VDELed
	lda #GBKCOLOR	; 2 playfield color
	sta COLUPF		; 3 playfield color
	lda #GCOL2		; 2
	sta COLUP0		; 3 @39! end of GRP0a display
	lda #{5}		; 2
	sta GRP0		; 3 @44! end of GRP1a display
	lda #GCOL3		; 2
	sta COLUP1		; 3 @49
	stx GRP0		; 3 @52
	sty COLUP0		; 3 @55<=@57
	lda #$00		; 2 turn off playfield
	sta COLUPF		; 3
	lda #$80		; 2
	sta HMP0		; 3
	sta HMP1		; 3 @63

	jmp .lineX
.lineX

	ENDM

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	org $F000

;------------------------------------------------------------------------------
ColdStart
	sei
	cld
	ldx #$FF
	txs

	lda #0
ClearMem
	sta 0,x
	dex
	bne ClearMem

	; RESP while still locked to ClearMem WSYNC — before RamKernel copy
	lda #1
	sta VDELP1

	lda #$CF
	sta PF0
	lda #$33
	sta PF1
	lda #$CC
	sta PF2

	ldx #$30
	sta RESP0
	nop
	sta RESP1
	lda #$06
	sta NUSIZ0
	lda #$02
	sta NUSIZ1

	stx HMP0

	lda #$20
	sta HMP1

	sta WSYNC
	sta HMOVE

	ldx #12
.cold_wait
	dex
	bne .cold_wait

	sta HMCLR
	nop
	nop

	ldx #(RamKernelEnd - RamKernel - 1)
.copy
	lda RamKernel,x
	sta RAM_BASE,x
	dex
	bpl .copy

	jmp RAM_BASE

;------------------------------------------------------------------------------
; Copied to RIOT $80. Original frame order: visible, then blanking tail.
; Sole cart call: jsr VisibleBars. Tail continues in RIOT after rts.
RamKernel
	jsr VisibleBars

	inc FIELD
	lda FIELD
	lsr
	bcc .rk_even

	ldx #30
	ldy #(PREROLL+1)
	jmp .rk_os

.rk_even
	ldx #29
	ldy #PREROLL

.rk_os
	sta WSYNC
	dex
	bne .rk_os

	lda #2
	sta VSYNC
	ldx #3
.rk_vsync
	sta WSYNC
	dex
	bne .rk_vsync
	lda #0
	sta VSYNC

	lda #2
	sta VBLANK
	ldx #37
.rk_vblank
	sta WSYNC
	dex
	bne .rk_vblank
	lda #0
	sta VBLANK

	tya
	tax
.rk_pr
	sta WSYNC
	dex
	bne .rk_pr

	ldx #7
.rk_busy
	dex
	bne .rk_busy
	lda #0
	sta $82
	sta $82
	sta $82
	sta $82
	nop

	jmp RAM_BASE
RamKernelEnd

;------------------------------------------------------------------------------
; Visible scanlines only — kernel macro + line0..end_lines unchanged.
; SyncToRight in cart (same cycle phase as original HMCLR/nop/nop/line0 entry).
VisibleBars
	sta WSYNC
	ldx #6
.vb_resp
	dex
	bne .vb_resp
	nop
	ldx #$30
	sta RESP0
	nop
	sta RESP1
	stx HMP0
	lda #$20
	sta HMP1
	sta WSYNC
	sta HMOVE
	ldx #12
.vb_sync
	dex
	bne .vb_sync
	sta HMCLR
	nop
	nop

line0

	kernel %01111100, %00110000, %01111100, %01111100, %00011100, %11111110, %01111100, %11111110, %01111100, %01111100
	kernel %10000110, %01010000, %10000010, %10000010, %00100100, %10000000, %10000010, %00000010, %10000010, %10000010
	kernel %10001010, %10010000, %00000100, %00000100, %01000100, %10000000, %10000000, %00000100, %10000010, %10000001
	kernel %10010010, %00010000, %00011000, %00111000, %10000100, %01111100, %11111100, %00001000, %01111100, %10000011
	kernel %10100010, %00010000, %01100000, %00000100, %11111110, %00000010, %10000010, %00010000, %10000010, %01111101
	kernel %11000010, %00010000, %11000000, %10000010, %00000100, %10000010, %10000010, %00100000, %10000010, %00000001
	kernel %01111100, %11111110, %11111110, %01111100, %00000100, %01111100, %01111100, %01000000, %01111100, %11111110

end_lines

	lda #0
	sta GRP0
	sta GRP1
	sta GRP0

	ldx #VISIBLE_LINES
.vb_wait
	sta WSYNC
	dex
	bne .vb_wait

	rts

	org $FFFA
	.word ColdStart
	.word ColdStart
	.word ColdStart
