; Stella-runnable title ROM — upstream logo kernels plus the STM32
; RamKernel / 6-line cart nibble copy (firmware/stm32/src/core.c).
;
;   $F000     ColdStart (TIA setup, copy $F200 → $80, jmp $80)
;   $F140     CartPack (32 bytes; leave zeros if audio is uninitialized)
;   $F160     OsHead (6 lines: AUDV0 + copy pack → $D0, joystick, WSYNC, RTS)
;   $F200     RamKernel image (copied to RIOT $80)
;   $F280     VisibleBars (preroll + HMOVE pad + 7 logo pairs + wait, jmp OsHead)
;
; Firmware does not assemble this file. Build for Stella: make
;
; NTSC 261/262: 192 visible + 6 OsHead + 23/24 OS + 3 VS + 37 VB.
; PAL still uses the NTSC RamKernel counts.

	processor 6502
	include vcs.h

RAM_BASE	equ $80
PACK		equ $D0		; 32 packed nibbles ($D0–$EF)
FIELD		equ $FB		; even/odd (OS 23/24). Not $FE/$FF (jsr stack)
AUDIDX		equ $FC
DUMMY		equ $FD		; zp scratch (not $80 — that is RamKernel)
CARTPACK	equ $F140

GAUDIO		equ #0
KERNEL_PAIRS	equ 7
NUM_LINES	equ 8		; upstream count (7 pairs; keeps VISIBLE_LINES)
PREROLL		equ 50
#if 1	; NTSC
VISIBLE_TOTAL	equ 192
VISIBLE_LINES	equ (VISIBLE_TOTAL - NUM_LINES*2 - PREROLL + 1)
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
VISIBLE_TOTAL	equ 242
VISIBLE_LINES	equ (VISIBLE_TOTAL - NUM_LINES*2 - PREROLL + 1)
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
	lda #GBKCOLOR		; 2
	sta COLUBK		; 3
	lda #GCOL7		; 2
	sta COLUP0		; 3 @42
	lda #{6}		; 2
	sta GRP0		; 3 @47
	lda #GCOL8		; 2
	sta COLUP1		; 3 @52
	stx GRP0		; 3 @55
	sty COLUP0		; 3 @58<=@60
	lda #$00		; 2
	sta COLUBK		; 3
	sta HMCLR		; 3

	; left_line

	lda #$00		; 2 dummy
	lda #{3}		; 2
	sta HMOVE		; late hmove @71
	sta GRP1		; 3 VDELed
	lda #GCOL1		; 2
	sta COLUP1		; 3
	lda #GAUDIO		; 2
	sta AUDV0		; 3 @10
	ldx #{9}		; 2
	ldy #GCOL4		; 2
	lda #{1}		; 2
	sta GRP0		; 3
	lda #GCOL0		; 2
	sta COLUP0		; 3
	lda #{7}		; 2
	sta GRP1		; 3 VDELed
	lda #GBKCOLOR		; 2
	sta COLUPF		; 3
	lda #GCOL2		; 2
	sta COLUP0		; 3 @39
	lda #{5}		; 2
	sta GRP0		; 3 @44
	lda #GCOL3		; 2
	sta COLUP1		; 3 @49
	stx GRP0		; 3 @52
	sty COLUP0		; 3 @55<=@57
	lda #$00		; 2
	sta COLUPF		; 3
	lda #$80		; 2
	sta HMP0		; 3
	sta HMP1		; 3 @63

	jmp .lineX
.lineX

	ENDM

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

	org $F000
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
.wait
	dex
	bne .wait

	sta HMCLR
	nop
	nop

	ldx #(RamKernelEnd - RamKernel - 1)
.copy
	lda $F200,x
	sta RAM_BASE,x
	dex
	bpl .copy

	jmp RAM_BASE

;------------------------------------------------------------------------------
	org $F140
CartPack
	ds 32, 0

; First 6 overscan lines: play 6 tail samples, copy 32 pack bytes into RIOT.
	org $F160
OsHead
	lda #0
	sta GRP0
	sta GRP1
	sta GRP0
	ldx #0
	ldy #5
.os6
	lda #0			; sample (zeros unless CartPack/this immediate is filled)
	sta AUDV0
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	dey
	sta WSYNC
	bne .os6
	lda #0
	sta AUDV0
	lda CARTPACK,x
	sta PACK,x
	inx
	lda CARTPACK,x
	sta PACK,x
	inx
	ldx SWCHA
	lda $FE00,x
	ldx SWCHB
	lda $FE00,x
	ldx INPT4
	lda $FE00,x
	ldx INPT5
	lda $FE00,x
	sta WSYNC
	rts

;------------------------------------------------------------------------------
; Copied to RIOT $80. Remaining OS 23/24 + VS 3 + VB 37 from PACK. No preroll.
	org $F200
	rorg $80
RamKernel
	jsr VisibleBars
	inc FIELD
	lda FIELD
	lsr
	ldx #23
	bcc .rk_even
	inx
.rk_even
	jsr Play
	lda #2
	sta VSYNC
	ldx #3
	jsr Play
	stx VSYNC
	lda #2
	sta VBLANK
	ldx #37
	jsr Play
	stx VBLANK
	stx AUDIDX
	ldx #7
.rk_pad
	dex
	bne .rk_pad
	nop
	bit 0
	jmp RAM_BASE

Play
	lda AUDIDX
	lsr
	tay
	lda PACK,y
	bcc .lo
	lsr
	lsr
	lsr
	lsr
.lo	and #$0F
	sta AUDV0
	inc AUDIDX
	sta WSYNC
	dex
	bne Play
	rts
RamKernelEnd
	rend

;------------------------------------------------------------------------------
; Upstream unrolled logo (github.com/lodefmode/moviecart kernel/core.asm).
; Lives after the $F140/$F160/$F200 map so the pairs do not overlap it.
	org $F280
VisibleBars
	; Same preroll as upstream (50 even / 51 odd) so the digits sit
	; mid-screen. Last WSYNC + pad + first HMOVE are one scanline.
	lda FIELD
	lsr
	ldx #PREROLL
	bcc .even_pre
	inx
.even_pre
	jsr WaitLines

	; Cycle-match github.com/lodefmode/moviecart kernel/core.asm after
	; preroll so right-line HMOVE is @3. DUMMY is $FD, not $80.
	lda #0
	sta DUMMY
	sta DUMMY
	sta DUMMY
	sta DUMMY
	ldx #7
.busy_wait
	dex
	bne .busy_wait
	sta DUMMY
	sta DUMMY
	nop
	jmp line0

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
	jsr WaitLines
	jmp OsHead

WaitLines
	sta WSYNC
	dex
	bne WaitLines
	rts

	org $FE00
	ds 256, 0		; lda $FE00,x (firmware gstore; unused here)

	org $FFFA
	.word ColdStart
	.word ColdStart
	.word ColdStart
