; Shape drawing in copy mode, unflipped, on Tom's GPU: DrawSpans<SHAPE_COPY, false> in the fork's
; game/main/xmm/shapedraw.cpp, the game's most used drawing, step for step. spans.c copies this
; to the GPU's RAM at $F03000, posts a frame in the parameter block and waits for the GPU to stop.
; Assembled by rmac (CMakeLists.txt).
;
; Parameter block (GPU RAM, $F03F80; longs):
;   +0 source (the frame's first span)  +1 x  +2 y  +3 clip x0  +4 clip y0  +5 clip x1  +6 clip y1
;   +7 row table (little-endian linear addresses, one a row)  +8 linear base
; A span: its length * 2 (+1 when run-encoded; 0 ends the frame), its offsets along and across
; from the hot spot, all little-endian words at any address, then raw pixels or runs (count * 2,
; +1 for a fill of the byte after it). Positions are 16-bit, as in the C: each sum is wrapped.
;
; r0 source		r1 x		r2 y		r3/r4 clip x0/y0	r5/r6 clip x1/y1
; r7 row table		r8 linear base	r9 length	r10 along		r11 across
; r12/r13 temps		r15 screen (the span's first visible pixel)
; r16 first		r17 last	r18 run-encoded	r19 pos		r20 count
; r21 from		r22 to		r23 run byte	r28 destination		r30 pixel
; r24-r27, r29 the addresses jumped to

G_CTRL		.equ	$F02114
SPAN_PARAMS	.equ	$F03F80

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
		movei	#Span,r24
		movei	#Skip,r25
		movei	#Done,r26
		movei	#Runs,r27
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
		jump	EQ,(r26)		; 0: the frame is done
		moveq	#1,r12
		and	r12,r18			; run-encoded

		loadb	(r0),r10
		addq	#1,r0
		loadb	(r0),r12
		addq	#1,r0
		shlq	#8,r12
		or	r12,r10
		add	r1,r10
		shlq	#16,r10
		sharq	#16,r10			; along = (int16_t) (x + offset)

		loadb	(r0),r11
		addq	#1,r0
		loadb	(r0),r12
		addq	#1,r0
		shlq	#8,r12
		or	r12,r11
		add	r2,r11
		shlq	#16,r11
		sharq	#16,r11			; across = (int16_t) (y + offset)

; ---- outside the clip box: skipped ----
		cmp	r4,r11			; across - y0
		jump	MI,(r25)
		nop
		cmp	r11,r6			; y1 - across
		jump	MI,(r25)
		nop
		cmp	r10,r5			; x1 - along
		jump	MI,(r25)
		nop
		move	r10,r12
		add	r9,r12
		subq	#1,r12
		shlq	#16,r12
		sharq	#16,r12			; (int16_t) (along + length - 1)
		cmp	r3,r12			; that - x0
		jump	MI,(r25)
		nop

; ---- the visible pixels, first to last, counted from the span's start ----
		move	r3,r16
		sub	r10,r16			; x0 - along
		jr	PL,First
		nop
		moveq	#0,r16
First:
		move	r5,r17
		sub	r10,r17			; x1 - along
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

; ---- screen = linear base + the row's address + along + first ----
		move	r11,r12
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
		add	r10,r15
		add	r16,r15

		cmpq	#0,r18
		jump	NE,(r27)
		nop

; ---- raw pixels: last - first + 1 from source + first ----
		move	r0,r12
		add	r16,r12
		move	r17,r13
		sub	r16,r13
		addq	#1,r13
Copy:
		loadb	(r12),r30
		addq	#1,r12
		subq	#1,r13
		storeb	r30,(r15)
		jr	NE,Copy
		addq	#1,r15
		jump	T,(r24)
		add	r9,r0			; past the span

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
		move	r21,r28			; destination: screen + from - first
		sub	r16,r28
		add	r15,r28
		move	r22,r13			; to - from + 1 pixels
		sub	r21,r13
		addq	#1,r13
		btst	#0,r23
		jr	EQ,RawRun
		nop

		loadb	(r0),r30		; a fill
		addq	#1,r0
		cmp	r21,r22			; to - from: none when negative
		jump	MI,(r29)
		add	r20,r19			; pos += count (the delay slot runs either way)
Fill:
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,Fill
		addq	#1,r28
		jump	T,(r29)
		nop

RawRun:
		move	r21,r12			; source + from - pos
		sub	r19,r12
		add	r0,r12
		cmp	r21,r22			; to - from: none when negative
		jr	MI,RawDone
		add	r20,r19			; pos += count
RawCopy:
		loadb	(r12),r30
		addq	#1,r12
		subq	#1,r13
		storeb	r30,(r28)
		jr	NE,RawCopy
		addq	#1,r28
RawDone:
		jump	T,(r29)
		add	r20,r0			; past the run's pixels

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

Done:
		loadb	(r0),r9			; after the last store has gone out
		or	r9,r9
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

		.68000
		.long
gpu_spans_end:
