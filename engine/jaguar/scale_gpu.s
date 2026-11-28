; The engine test's scaled pictures on Tom's GPU (engine/jaguar/main.c, eb_scaled): a bitmap drawn
; into a frame, nearest pixel, each of its pixels' colour 0 left clear; for the intro's zooms (the
; monitor, the trees, the stones), which the 68000 could not do in time. The 68000 clips it and
; works out where each row and column starts (16.16 places in the bitmap). Lives at $F03CA0,
; after movers_gpu.s. Assembled by rmac.
;
; Parameter block (GPU RAM, $F03FC0; longs): +0 the bitmap's pixels  +1 its pitch  +2 the first
;   row's first pixel in the frame  +3 the frame's pitch  +4 rows  +5 columns (both 1 or more)
;   +6 the step in the bitmap a pixel (16.16)  +7 the first row's place in it (16.16)  +8 the
;   first column's
;
; r1 the bitmap  r2 its pitch  r4 the row in the frame  r5 the frame's pitch  r6 rows left
; r7 columns  r8 the step  r9 the row's place  r10 the first column's  r11 a column's place
; r12 a pixel in the frame  r13 columns left  r15 a pixel in the bitmap  r16 its colour
; r17 the row in the bitmap  r20-r22 the loops' places  r14 the parameters

G_CTRL		.equ	$F02114
SCALE_PARAMS	.equ	$F03FC0

		.globl	gpu_scale
		.globl	gpu_scale_end

		.text
		.long
gpu_scale:
		.gpu
		.org	$F03CA0

		movei	#SCALE_PARAMS,r14
		load	(r14),r1
		load	(r14+1),r2
		load	(r14+2),r4
		load	(r14+3),r5
		load	(r14+4),r6
		load	(r14+5),r7
		load	(r14+6),r8
		load	(r14+7),r9
		load	(r14+8),r10
		movei	#Row,r20
		movei	#Pixel,r21
		movei	#Skip,r22

Row:		move	r9,r17
		shrq	#16,r17			; the row in the bitmap
		mult	r2,r17			; its first pixel's offset
		add	r1,r17
		cmpq	#0,r11		; (read, not written, first: Atari's GPU/DSP bug 2)
		move	r10,r11
		cmpq	#0,r12		; (read, not written, first: Atari's GPU/DSP bug 2)
		move	r4,r12
		cmpq	#0,r13		; (read, not written, first: Atari's GPU/DSP bug 2)
		move	r7,r13
Pixel:		move	r11,r15
		shrq	#16,r15
		add	r17,r15
		loadb	(r15),r16
		add	r8,r11
		cmpq	#0,r16			; colour 0: left clear
		jump	EQ,(r22)
		nop
		storeb	r16,(r12)
Skip:		subq	#1,r13
		jump	NE,(r21)
		addq	#1,r12
		add	r8,r9			; the next row
		add	r5,r4
		subq	#1,r6
		jump	NE,(r20)
		nop

		load	(r1),r16		; after the last store has gone out
		cmpq	#0,r16		; (read, not written: Atari's GPU/DSP bug 2, not or)
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

		.68000
		.long
gpu_scale_end:
