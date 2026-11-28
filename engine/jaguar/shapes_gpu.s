; The engine test's shapes on Tom's GPU (engine/jaguar/main.c, eb_shape): a list of frames, each
; in the engine's runs (engine/core/engine.h), drawn one after another. A run's header is one long:
; row (signed byte), first column (signed byte), length, fill bit. Lives at $F03000, below
; tiles_gpu.s at $F03400. Assembled by rmac.
;
; Parameter block (GPU RAM, $F03F80; longs): +0 the list (main RAM)  +1 entries in it
;   +2 the shortest row the blitter is given
; An entry, ten longs: its first run, the surface's pixels, the hot spot along and across, the
; clip's along and across minimum, then maximum, the along and across steps. Unflipped, along is
; x (step 1) and across y (step the pitch); flipped, the other way about.
; Rows of +2 pixels or more go to the blitter, which the GPU does not wait for until it draws the
; next run.
;
; r0 entry  r1 entries left  r2 runs  r3 pixels  r4/r5 along/across base  r6/r7 their minimum
; r8/r9 their maximum  r10/r11 their steps  r12 header  r13 across  r14 the blitter  r15 along
; r16 length  r17 first visible  r18 count  r19 destination  r20 source  r21-r23 temps
; r24 RunLoop  r25 EntryLoop  r26 BLIT_FLAGS  r27 $10000  r28 the shortest row blitted

G_CTRL		.equ	$F02114
SHAPE_PARAMS	.equ	$F03F80
BLITTER		.equ	$F02200		; +0 A1_BASE +1 A1_FLAGS +3 A1_PIXEL +9 A2_BASE +10 A2_FLAGS
					; +12 A2_PIXEL +14 B_CMD +15 B_COUNT +26/+27 B_PATD
BLIT_FLAGS	.equ	$16018		; 8-bit pixels, a pixel at a time, width 4096 (one row only)
BLIT_COPY	.equ	$01800001	; source on, replace
BLIT_FILL	.equ	$00010000	; the pattern register's pixels

		.globl	gpu_shapes
		.globl	gpu_shapes_end

		.text
		.long
gpu_shapes:
		.gpu
		.org	$F03000

		movei	#SHAPE_PARAMS,r21
		load	(r21),r0
		addq	#4,r21
		load	(r21),r1
		addq	#4,r21
		load	(r21),r28
		movei	#BLITTER,r14
		movei	#BLIT_FLAGS,r26
		movei	#$10000,r27
		movei	#RunLoop,r24
		movei	#EntryLoop,r25

EntryLoop:
		cmpq	#0,r1
		movei	#Done,r21
		jump	EQ,(r21)
		nop
		subq	#1,r1
		load	(r0),r2
		addq	#4,r0
		load	(r0),r3
		addq	#4,r0
		load	(r0),r4
		addq	#4,r0
		load	(r0),r5
		addq	#4,r0
		load	(r0),r6
		addq	#4,r0
		load	(r0),r7
		addq	#4,r0
		load	(r0),r8
		addq	#4,r0
		load	(r0),r9
		addq	#4,r0
		load	(r0),r10
		addq	#4,r0
		load	(r0),r11
		addq	#4,r0

; ---- a run: where it is, past its pixels ----
RunLoop:
		load	(r2),r12
		addq	#4,r2
		cmpq	#0,r12
		jump	EQ,(r25)		; 0: the frame is done
		move	r12,r13
		sharq	#24,r13			; the row
		move	r12,r15
		shlq	#8,r15
		sharq	#24,r15			; the first column
		move	r12,r16
		shlq	#16,r16
		shrq	#24,r16			; the length
		add	r5,r13			; across
		add	r4,r15			; along
		move	r2,r20			; its pixels
		btst	#0,r12
		jr	NE,Past
		moveq	#4,r21			; a fill's: a long
		cmpq	#0,r21		; (read, not written, first: Atari's GPU/DSP bug 2)
		move	r16,r21
		addq	#3,r21
		shrq	#2,r21
		shlq	#2,r21			; raw: the length, padded to a long
Past:
		add	r21,r2

; ---- clipped: across in the box, along from first to last ----
		cmp	r7,r13			; across - minimum
		jump	MI,(r24)
		nop
		cmp	r13,r9			; maximum - across
		jump	MI,(r24)
		nop
		move	r6,r17
		sub	r15,r17			; minimum - along
		jr	PL,First
		nop
		cmpq	#0,r17		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#0,r17
First:
		move	r8,r18
		sub	r15,r18			; maximum - along
		move	r16,r22
		subq	#1,r22			; length - 1
		cmp	r22,r18
		jr	MI,Last
		nop
		move	r22,r18
Last:
		sub	r17,r18			; last - first
		jump	MI,(r24)
		addq	#1,r18			; count

; ---- the first visible pixel: pixels + across * its step + (along + first) * its step ----
		move	r13,r19
		imult	r11,r19
		move	r15,r22
		add	r17,r22
		imult	r10,r22
		add	r22,r19
		add	r3,r19

BlitIdle:					; the last run's blit done
		load	(r14+14),r22
		btst	#0,r22
		jr	EQ,BlitIdle
		nop

		btst	#0,r12
		movei	#Fill,r21
		jump	NE,(r21)
		nop

; ---- raw pixels ----
		add	r17,r20			; from the first visible
		movei	#RawBlit,r21
		cmpq	#1,r10			; a row?
		jr	NE,RawBytes
		cmp	r28,r18			; (the delay slot) long enough?
		jump	PL,(r21)
		nop
RawBytes:
		loadb	(r20),r22
		addq	#1,r20
		subq	#1,r18
		storeb	r22,(r19)
		jr	NE,RawBytes
		add	r10,r19
		jump	T,(r24)
		nop

; ---- a fill ----
Fill:
		load	(r20),r23		; its pixel, four times
		movei	#FillBlit,r21
		cmpq	#1,r10
		jr	NE,FillBytes
		cmp	r28,r18
		jump	PL,(r21)
		nop
FillBytes:
		subq	#1,r18
		storeb	r23,(r19)
		jr	NE,FillBytes
		add	r10,r19
		jump	T,(r24)
		nop

; ---- a row by the blitter, a pixel at a time (source and destination need not share a phrase
; offset); not waited for ----
RawBlit:
		moveq	#7,r23
		move	r19,r22
		move	r19,r21
		and	r23,r21			; the destination's pixel in its phrase
		sub	r21,r22			; and its phrase
		store	r22,(r14)		; A1_BASE
		store	r21,(r14+3)		; A1_PIXEL: y 0, that x
		move	r20,r22
		move	r20,r21
		and	r23,r21
		sub	r21,r22
		store	r22,(r14+9)		; A2_BASE
		store	r21,(r14+12)		; A2_PIXEL
		store	r26,(r14+1)		; A1_FLAGS
		store	r26,(r14+10)		; A2_FLAGS
		move	r27,r22
		or	r18,r22
		store	r22,(r14+15)		; B_COUNT: one row of r18
		movei	#BLIT_COPY,r22
		store	r22,(r14+14)		; B_CMD: go
		jump	T,(r24)
		nop
FillBlit:
		moveq	#7,r21
		and	r19,r21
		move	r19,r22
		sub	r21,r22
		store	r22,(r14)		; A1_BASE
		store	r21,(r14+3)		; A1_PIXEL
		store	r26,(r14+1)		; A1_FLAGS
		store	r23,(r14+26)		; B_PATD: the pixel in all eight
		store	r23,(r14+27)
		move	r27,r22
		or	r18,r22
		store	r22,(r14+15)		; B_COUNT
		movei	#BLIT_FILL,r22
		store	r22,(r14+14)		; B_CMD: go
		jump	T,(r24)
		nop

Done:
		load	(r14+14),r22		; the last blit done
		btst	#0,r22
		jr	EQ,Done
		nop
		load	(r0),r22		; after the last store has gone out
		cmpq	#0,r22		; (read, not written: Atari's GPU/DSP bug 2, not or)
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

		.68000
		.long
gpu_shapes_end:
