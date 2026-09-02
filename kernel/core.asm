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

;user memory 128 to 255
RAM_BASE	equ $80
PACK		equ $D0		; 32 packed nibbles ($D0–$EF)
FIELD		equ $FB		; even/odd (OS 23/24). Not $FE/$FF (jsr stack)
AUDIDX		equ $FC
DUMMY		equ $FD		; zp scratch (not $80 — that is RamKernel)
CARTPACK	equ $F140

GAUDIO		equ #0
KERNEL_PAIRS	equ 7
NUM_LINES	equ 8
PREROLL		equ 50
#if 1	; NTSC
VISIBLE_TOTAL	equ 192
VISIBLE_LINES	equ (VISIBLE_TOTAL - NUM_LINES*2 - PREROLL + 1)
GCOL0			equ $42	;red
GCOL5			equ $36	;orange
GCOL1			equ $EC	;yellow
GCOL6			equ $D8	;light green
GCOL2			equ $72	;blue
GCOL7			equ $64	;purple
GCOL3			equ $B8	;cyan
GCOL8			equ $6C	;light purple
GCOL4			equ $06	;dark grey
GCOL9			equ $0A	;light grey
GBKCOLOR		equ $02	;dark grey
#else	; PAl
VISIBLE_TOTAL	equ 242
VISIBLE_LINES	equ (VISIBLE_TOTAL - NUM_LINES*2 - PREROLL + 1)
GCOL0			equ $44	;red
GCOL5			equ $46	;orange
GCOL1			equ $2C	;yellow
GCOL6			equ $36	;green
GCOL2			equ $B2	;blue
GCOL7			equ $C4	;purple
GCOL3			equ $9A	;cyan
GCOL8			equ $AC	;light purple
GCOL4			equ $F8	;dark grey
GCOL9			equ $FC	;light grey
GBKCOLOR		equ $04	;dark grey
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
	sta HMOVE		;back 8, late hmove ;needs to be on cycle 71
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
	lda #GBKCOLOR		; 2 playfield color
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
ColdStart
	sei
	cld
	ldx #$FF
	txs

	;zero memory
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

	ldx #$30		; going into HMP0 later
	sta RESP0
	nop
	sta RESP1
	lda #$06		;3 copies medium
	sta NUSIZ0
	lda #$02		;2 copies medium
	sta NUSIZ1

	stx HMP0
	lda #$20
	sta HMP1

	; HMOVE needs to be after WSYNC here
	sta WSYNC
	sta HMOVE

	ldx #12
.wait_cnt
	dex
	bne .wait_cnt

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
	ldx #23			; overscan
	bcc .rk_even
	inx
.rk_even
	jsr Play
	; vsync 3
	lda #2
	sta VSYNC
	ldx #3
	jsr Play
	stx VSYNC
	; vblank 37
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
	; preroll
	lda FIELD
	lsr
	ldx #PREROLL
	bcc .even_pre
	inx
.even_pre
	jsr wait_lines

	;; wait...

	lda #0
	sta DUMMY
	sta DUMMY
	sta DUMMY
	sta DUMMY

	ldx #7
busy_wait
	dex
	bne busy_wait

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

	;clear
	lda #0
	sta GRP0
	sta GRP1
	sta GRP0

	ldx #VISIBLE_LINES
	jsr wait_lines
	jmp OsHead

wait_lines
	sta WSYNC
	dex
	bne wait_lines
	rts

	org $FE00
	ds 256, 0		; lda $FE00,x (firmware gstore; unused here)

	org $FFFA
reset_loop
	.word ColdStart	;NMI
	.word ColdStart	;RESET
	.word ColdStart	;IRQ/BRK
