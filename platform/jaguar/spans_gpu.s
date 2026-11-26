; Shape drawing on Tom's GPU: every DrawSpans<Mode, Flipped> of the fork's
; game/main/xmm/shapedraw.cpp, step for step. spans.c copies this to the GPU's RAM at $F03000,
; posts a frame in the parameter block and waits for the GPU to stop. Assembled by rmac
; (CMakeLists.txt).
;
; Parameter block (GPU RAM, $F03F80; longs):
;   +0 source (the frame's first span)  +1/+2 along and across base (x, y; flipped: y, x)
;   +3/+4 clip along and across minimum  +5/+6 their maximum  +7 row table (little-endian linear
;   addresses, one a row)  +8 linear base  +9 table  +10 mode (0 copy, 1 translate, 2 blend)
;   +11 flipped  +12 step (1; flipped, a row: 320)
; A span: its length * 2 (+1 when run-encoded; 0 ends the frame), its offsets along and across
; from the hot spot, all little-endian words at any address, then raw pixels or runs (count * 2,
; +1 for a fill of the byte after it). Positions are 16-bit, as in the C: each sum is wrapped.
; Unflipped, a span runs along a row; flipped, down a column: the screen address is the row of
; its first visible pixel, and each pixel a row further on.
;
; r0 source		r1/r2 along/across base		r3/r4 clip along/across minimum
; r5/r6 their maximum	r7 row table	r8 linear base	r9 length	r10 along	r11 across
; r12/r13 temps		r14 table	r15 screen (the span's first visible pixel)
; r16 first		r17 last	r18 run-encoded	r19 pos		r20 count	r21 from	r22 to
; r23 run byte		r24 Span	r25 Skip	r26 the mode's raw pixels	r27 its fill
; r28 destination	r29 RunLoop	r30 pixel	r31 step
; Alternate bank r0: flipped; r1: r14 kept while a blit has r14. A pixel routine has r13 pixels to
; put at r28 on (r31 apart), from r12 on (raw) or of value r30 (fill), and goes to After. Copy mode
; hands a row (not a column) of BLIT_SMALLEST pixels or more to the blitter, a pixel at a time
; (source and destination need not share a phrase offset), and waits for it: what comes next may
; draw over the same pixels.

G_CTRL		.equ	$F02114
SPAN_PARAMS	.equ	$F03F80
; the blitter, for copy mode's long rows (CopyRaw, CopyFill): its registers as longs from BLITTER
BLITTER		.equ	$F02200		; +0 A1_BASE +1 A1_FLAGS +3 A1_PIXEL +9 A2_BASE +10 A2_FLAGS
					; +12 A2_PIXEL +14 B_CMD +15 B_COUNT +26/+27 B_PATD
BLIT_FLAGS	.equ	$16018		; 8-bit pixels, a pixel at a time, width 4096 (one row only)
BLIT_COPY	.equ	$01800001	; source on, replace
BLIT_FILL	.equ	$00010000	; the pattern register's pixels
BLIT_SMALLEST	.equ	8		; shorter rows: the byte loop is quicker than the set-up

		.globl	gpu_spans
		.globl	gpu_spans_end

		.text
		.long
gpu_spans:
		.gpu
		.org	$F03000

		movei	#SPAN_PARAMS,r14
		load	(r14),r0
		load	(r14+1),r1
		load	(r14+2),r2
		load	(r14+3),r3
		load	(r14+4),r4
		load	(r14+5),r5
		load	(r14+6),r6
		load	(r14+7),r7
		load	(r14+8),r8
		load	(r14+10),r12		; the mode's two pixel routines
		movei	#Routines,r13
		shlq	#3,r12
		add	r12,r13
		load	(r13),r26
		addq	#4,r13
		load	(r13),r27
		load	(r14+11),r12
		moveta	r12,r0			; flipped
		load	(r14+12),r31
		load	(r14+9),r14		; the table, last: r14 was the block
		movei	#Span,r24
		movei	#Skip,r25
		movei	#RunLoop,r29

; ---- a span's header and offsets ----
Span:
		loadb	(r0),r9
		addq	#1,r0
		loadb	(r0),r12
		addq	#1,r0
		shlq	#8,r12
		or	r12,r9			; the header
		move	r9,r18
		shrq	#1,r9			; length
		movei	#Done,r12
		jump	EQ,(r12)		; 0: the frame is done
		moveq	#1,r13
		and	r13,r18			; run-encoded

		loadb	(r0),r10
		addq	#1,r0
		loadb	(r0),r12
		addq	#1,r0
		shlq	#8,r12
		or	r12,r10
		add	r1,r10
		shlq	#16,r10
		sharq	#16,r10			; along = (int16_t) (base + offset)

		loadb	(r0),r11
		addq	#1,r0
		loadb	(r0),r12
		addq	#1,r0
		shlq	#8,r12
		or	r12,r11
		add	r2,r11
		shlq	#16,r11
		sharq	#16,r11			; across = (int16_t) (base + offset)

; ---- outside the clip box: skipped ----
		cmp	r4,r11			; across - minimum
		jump	MI,(r25)
		nop
		cmp	r11,r6			; maximum - across
		jump	MI,(r25)
		nop
		cmp	r10,r5			; along maximum - along
		jump	MI,(r25)
		nop
		move	r10,r12
		add	r9,r12
		subq	#1,r12
		shlq	#16,r12
		sharq	#16,r12			; (int16_t) (along + length - 1)
		cmp	r3,r12			; that - along minimum
		jump	MI,(r25)
		nop

; ---- the visible pixels, first to last, counted from the span's start ----
		move	r3,r16
		sub	r10,r16			; minimum - along
		jr	PL,First
		nop
		moveq	#0,r16
First:
		move	r5,r17
		sub	r10,r17			; maximum - along
		move	r9,r12
		subq	#1,r12			; length - 1
		cmp	r12,r17
		jr	MI,Last
		nop
		move	r12,r17
Last:
		cmp	r16,r17			; last - first
		jump	MI,(r25)
		nop

; ---- screen: linear base + a row's address + the offset into it ----
; unflipped: row across, along + first in; flipped: row along + first, across in
		movefa	r0,r13
		cmpq	#0,r13
		jr	NE,FlipRow
		move	r11,r12			; across
		jr	T,Row
		nop
FlipRow:
		move	r10,r12
		add	r16,r12			; along + first
Row:
		shlq	#2,r12
		add	r7,r12
		loadb	(r12),r15
		addq	#1,r12
		loadb	(r12),r13
		addq	#1,r12
		shlq	#8,r13
		or	r13,r15
		loadb	(r12),r13
		addq	#1,r12
		shlq	#16,r13
		or	r13,r15
		loadb	(r12),r13
		shlq	#24,r13
		or	r13,r15
		add	r8,r15
		movefa	r0,r13
		cmpq	#0,r13
		jr	NE,FlipIn
		nop
		add	r10,r15
		jr	T,In
		add	r16,r15			; along + first
FlipIn:
		add	r11,r15			; across
In:
		cmpq	#0,r18
		movei	#Runs,r12
		jump	NE,(r12)
		nop

; ---- raw pixels: one run of the span's whole length ----
		moveq	#0,r19			; pos
		move	r9,r20			; count
		move	r16,r21			; from
		move	r17,r22			; to
		move	r0,r12
		add	r16,r12			; source + first
		add	r9,r0			; past the span

; ---- pixels from to to of a raw run, r12 at from's ----
EmitRaw:
		cmp	r21,r22			; to - from: none when negative
		jr	MI,NoPixels
		nop
		move	r22,r13
		sub	r21,r13
		addq	#1,r13			; to - from + 1
		move	r21,r28
		sub	r16,r28
		imult	r31,r28
		add	r15,r28			; screen + (from - first) * step
		jump	T,(r26)
		nop
NoPixels:
		movei	#After,r13
		jump	T,(r13)
		nop

; ---- runs: each clipped to first..last ----
Runs:
		moveq	#0,r19			; pos
RunLoop:
		cmp	r9,r19			; pos - length
		jump	PL,(r24)		; the span is done
		nop
		loadb	(r0),r23
		addq	#1,r0
		move	r23,r20
		shrq	#1,r20			; count
		move	r19,r21			; from = max(pos, first)
		cmp	r16,r21
		jr	PL,From
		nop
		move	r16,r21
From:
		move	r19,r22			; to = min(pos + count - 1, last)
		add	r20,r22
		subq	#1,r22
		cmp	r17,r22
		jr	MI,To
		nop
		move	r17,r22
To:
		movei	#RunRaw,r12
		btst	#0,r23
		jump	EQ,(r12)
		nop
		loadb	(r0),r30		; a fill of one value
		addq	#1,r0
		cmp	r21,r22
		jr	MI,NoFill
		nop
		move	r22,r13
		sub	r21,r13
		addq	#1,r13
		move	r21,r28
		sub	r16,r28
		imult	r31,r28
		add	r15,r28
		jump	T,(r27)
		nop
NoFill:
		jump	T,(r29)
		add	r20,r19			; pos += count
RunRaw:
		move	r21,r12			; source + from - pos
		sub	r19,r12
		add	r0,r12
		movei	#EmitRaw,r13
		jump	T,(r13)
		add	r20,r0			; past the run's pixels

; ---- after a run's pixels: the next run, or the next span ----
After:
		cmpq	#0,r18
		jump	EQ,(r24)		; a raw span: done
		nop
		jump	T,(r29)
		add	r20,r19			; pos += count

; ---- a span not drawn: past its pixels ----
Skip:
		cmpq	#0,r18
		jr	NE,SkipRuns
		nop
		jump	T,(r24)
		add	r9,r0
SkipRuns:
		loadb	(r0),r23
		addq	#1,r0
		move	r23,r20
		shrq	#1,r20
		btst	#0,r23
		jr	EQ,SkipRaw
		sub	r20,r9			; length -= count
		jr	T,SkipTest
		addq	#1,r0
SkipRaw:
		add	r20,r0
SkipTest:
		cmpq	#0,r9
		jr	NNNZ,SkipRuns		; while length > 0
		nop
		jump	T,(r24)
		nop

; ---- the pixel routines ----
; copy: the shape's pixels
CopyRaw:
		movei	#CopyRawBlit,r21
		cmpq	#1,r31			; a row?
		jr	NE,CopyRawBytes
		cmpq	#BLIT_SMALLEST,r13	; (the delay slot) long enough?
		jump	PL,(r21)
		nop
CopyRawBytes:
		loadb	(r12),r30
		addq	#1,r12
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,CopyRawBytes
		add	r31,r28
		movei	#After,r12
		jump	T,(r12)
		nop
CopyFill:
		movei	#CopyFillBlit,r21
		cmpq	#1,r31
		jr	NE,CopyFillBytes
		cmpq	#BLIT_SMALLEST,r13
		jump	PL,(r21)
		nop
CopyFillBytes:
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,CopyFillBytes
		add	r31,r28
		movei	#After,r12
		jump	T,(r12)
		nop
CopyRawBlit:
		moveta	r14,r1
		movei	#BLITTER,r14
		moveq	#7,r23
		move	r28,r22
		move	r22,r21
		and	r23,r21			; the destination's pixel in its phrase
		sub	r21,r22			; and its phrase
		store	r22,(r14)		; A1_BASE
		store	r21,(r14+3)		; A1_PIXEL: y 0, that x
		move	r12,r22
		move	r22,r21
		and	r23,r21
		sub	r21,r22
		store	r22,(r14+9)		; A2_BASE
		store	r21,(r14+12)		; A2_PIXEL
		movei	#BLIT_FLAGS,r22
		store	r22,(r14+1)		; A1_FLAGS
		store	r22,(r14+10)		; A2_FLAGS
		movei	#$10000,r22
		or	r13,r22
		store	r22,(r14+15)		; B_COUNT: one row of r13
		movei	#BLIT_COPY,r22
		movei	#BlitGo,r21
		jump	T,(r21)
		nop
CopyFillBlit:
		moveta	r14,r1
		movei	#BLITTER,r14
		moveq	#7,r23
		move	r28,r22
		move	r22,r21
		and	r23,r21
		sub	r21,r22
		store	r22,(r14)		; A1_BASE
		store	r21,(r14+3)		; A1_PIXEL
		movei	#BLIT_FLAGS,r22
		store	r22,(r14+1)		; A1_FLAGS
		move	r30,r22			; the byte in each of a phrase's eight
		shlq	#8,r22
		or	r30,r22
		move	r22,r21
		shlq	#16,r21
		or	r21,r22
		store	r22,(r14+26)		; B_PATD
		store	r22,(r14+27)
		movei	#$10000,r22
		or	r13,r22
		store	r22,(r14+15)		; B_COUNT
		movei	#BLIT_FILL,r22
BlitGo:
		store	r22,(r14+14)		; B_CMD: go
BlitWait:
		load	(r14+14),r22
		btst	#0,r22			; idle?
		jr	EQ,BlitWait
		nop
		movefa	r1,r14
		movei	#After,r12
		jump	T,(r12)
		nop

; translate: the screen through the table, whatever the shape's pixels
Translate:
		loadb	(r28),r30
		add	r14,r30
		loadb	(r30),r30
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,Translate
		add	r31,r28
		movei	#After,r12
		jump	T,(r12)
		nop

; blend: a pixel the table maps to 255 stays solid; any other picks the 256-byte table, counted
; from the table's start, that recolors the screen
BlendRaw:
		movei	#255,r22
BlendRawLoop:
		loadb	(r12),r30		; the pixel
		addq	#1,r12
		move	r14,r23
		add	r30,r23
		loadb	(r23),r23		; mix
		cmp	r22,r23
		jr	EQ,BlendRawPut
		shlq	#8,r23
		add	r14,r23
		loadb	(r28),r21
		add	r21,r23
		loadb	(r23),r30		; table[mix * 256 + screen]
BlendRawPut:
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,BlendRawLoop
		add	r31,r28
		movei	#After,r12
		jump	T,(r12)
		nop
BlendFill:
		move	r14,r23
		add	r30,r23
		loadb	(r23),r23		; mix
		movei	#255,r22
		movei	#CopyFill,r21
		cmp	r22,r23
		jump	EQ,(r21)		; solid
		shlq	#8,r23
		add	r14,r23			; the recoloring table
BlendFillLoop:
		loadb	(r28),r30
		add	r23,r30
		loadb	(r30),r30
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,BlendFillLoop
		add	r31,r28
		movei	#After,r12
		jump	T,(r12)
		nop

Done:
		loadb	(r0),r9			; after the last store has gone out
		or	r9,r9
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

; each mode's raw and fill routines
		.long
Routines:
		dc.l	CopyRaw, CopyFill
		dc.l	Translate, Translate
		dc.l	BlendRaw, BlendFill

		.68000
		.long
gpu_spans_end:
