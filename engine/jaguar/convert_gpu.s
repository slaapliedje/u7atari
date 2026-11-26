; The engine test's shape conversion on Tom's GPU (engine/jaguar/main.c, eb_convert): an object
; shape of the original's (little-endian spans) made into the engine's runs (engine/core/engine.h),
; as e_convert_shape in engine/core/engine.c does. Run twice a shape: once to measure (every
; store goes to one dummy long), once to write. Lives at $F03500, after tiles_gpu.s. Assembled by
; rmac.
;
; Parameter block (GPU RAM, $F03F00; longs): +0 the shape  +1 where to write (or the dummy)
;   +2 a mask for the output's offsets (-1 writing, 0 measuring)  +3 the size made (0: it cannot
;   be put in runs, or might not fit), written by the GPU  +4 where no span may start (writing;
;   room less E_CONVERT_SLACK)
; The shape is trusted to be well formed (as the original trusts it); only places that do not
; fit a run's header are refused.
;
; r0 the shape  r2 output  r3 bytes made  r4 frames left  r5 the shape's next frame offset
; r6 source  r7 the span's length  r8 its first column  r9 its row << 24  r10 pos in the span
; r11 count  r12 a byte  r13 the open raw run's header offset  r14 params
; r15 the open raw run's length (-1: none)  r16 its first column  r17-r19 temps  r20 AppFull
; r21/r22 Append's step and source
; r23 AppLoop  r24 255 (a run's longest, and E_BLACK)  r25 SpanLoop  r26 Fail  r27 our next frame offset's place  r28 the mask
; r29 return from Open and Close  r30 return from Append

G_CTRL		.equ	$F02114
CONV_PARAMS	.equ	$F03F00

		.globl	gpu_convert
		.globl	gpu_convert_end

; a byte of the source
.macro RDB dst
		loadb	(r6),\dst
		addq	#1,r6
.endm
; a little-endian word of the source
.macro RD16 dst,t
		loadb	(r6),\dst
		addq	#1,r6
		loadb	(r6),\t
		addq	#1,r6
		shlq	#8,\t
		or	\t,\dst
.endm
; a little-endian long of the source
.macro RD32 dst,t
		RD16	\dst,\t
		loadb	(r6),\t
		addq	#1,r6
		shlq	#16,\t
		or	\t,\dst
		loadb	(r6),\t
		addq	#1,r6
		shlq	#24,\t
		or	\t,\dst
.endm
; the output's address at offset off
.macro ADDR dst,off
		move	\off,\dst
		and	r28,\dst
		add	r2,\dst
.endm
.macro PUT32 val
		ADDR	r19,r3
		store	\val,(r19)
		addq	#4,r3
.endm
.macro PUT16 val
		ADDR	r19,r3
		storew	\val,(r19)
		addq	#2,r3
.endm
.macro PUTB val
		ADDR	r19,r3
		storeb	\val,(r19)
		addq	#1,r3
.endm
; a column or row from the hot spot: a signed byte, or the shape is refused
.macro CHK8 val,t
		move	\val,\t
		sharq	#7,\t
		addq	#1,\t
		shrq	#1,\t			; 0 for -128..127
		jump	NE,(r26)
		nop
.endm

		.text
		.long
gpu_convert:
		.gpu
		.org	$F03500

		movei	#CONV_PARAMS,r14
		load	(r14),r0
		load	(r14+1),r2
		load	(r14+2),r28
		moveq	#0,r3
		movei	#255,r24
		movei	#SpanLoop,r25
		movei	#Fail,r26
		movei	#AppLoop,r23
		movei	#AppFull,r20

; ---- the size and the frame offsets, filled in as we go ----
		move	r0,r6
		addq	#4,r6
		RD32	r4,r17			; the first frame's offset
		subq	#4,r4
		shrq	#2,r4			; frames
		move	r4,r17
		addq	#1,r17
		moveq	#0,r18
Zero:
		PUT32	r18
		subq	#1,r17
		jr	NE,Zero
		nop
		move	r0,r5
		addq	#4,r5
		moveq	#4,r27

; ---- a frame: its box, then its spans ----
FrameLoop:
		cmpq	#0,r4
		movei	#Finish,r17
		jump	EQ,(r17)
		nop
		subq	#1,r4
		ADDR	r19,r27
		store	r3,(r19)		; the frame's offset
		addq	#4,r27
		move	r5,r6
		RD32	r17,r18
		addq	#4,r5
		move	r0,r6
		add	r17,r6
		RD16	r17,r18			; right, left, above, below
		PUT16	r17
		RD16	r17,r18
		PUT16	r17
		RD16	r17,r18
		PUT16	r17
		RD16	r17,r18
		PUT16	r17

SpanLoop:
		cmpq	#0,r28			; writing: room left for a span?
		jr	EQ,Roomy
		nop
		load	(r14+4),r17
		cmp	r17,r3			; made - limit
		jump	PL,(r26)
		nop
Roomy:
		RD16	r7,r17			; the span's header
		cmpq	#0,r7
		movei	#FrameEnd,r17
		jump	EQ,(r17)
		nop
		RD16	r8,r17
		shlq	#16,r8
		sharq	#16,r8			; first column
		RD16	r9,r17
		shlq	#16,r9
		sharq	#16,r9			; row
		CHK8	r9,r17
		shlq	#24,r9
		move	r7,r17
		shrq	#1,r7			; length
		moveq	#0,r10			; pos
		moveq	#0,r15
		subq	#1,r15			; no raw run open
		btst	#0,r17
		movei	#Encoded,r18
		jump	NE,(r18)
		nop

; ---- raw: one raw run (split at 255) ----
		move	r7,r11
		move	r6,r22
		moveq	#1,r21
		movei	#RawDone,r30
		movei	#Append,r17
		jump	T,(r17)
		nop
RawDone:
		move	r22,r6
		move	r25,r29
		movei	#Close,r17
		jump	T,(r17)
		nop

; ---- run-encoded: raw pieces and short fills into raw runs, long fills apart ----
Encoded:
		cmp	r7,r10			; pos - length
		jr	MI,EncRun
		nop
		move	r25,r29			; done: the raw run closed, the next span
		movei	#Close,r17
		jump	T,(r17)
		nop
EncRun:
		RDB	r12
		move	r12,r11
		shrq	#1,r11			; count
		btst	#0,r12
		movei	#EncRaw,r17
		jump	EQ,(r17)
		nop
		cmpq	#8,r11			; E_FILL_SHORTEST
		movei	#EncShort,r17
		jump	MI,(r17)
		nop
		movei	#EncFill,r29
		movei	#Close,r17
		jump	T,(r17)
		nop
EncFill:
		move	r8,r17
		add	r10,r17			; first column + pos
		CHK8	r17,r18
		shlq	#24,r17
		shrq	#8,r17
		or	r9,r17
		move	r11,r18
		shlq	#8,r18
		or	r18,r17
		bset	#0,r17			; a fill
		PUT32	r17
		RDB	r12
		cmpq	#0,r12			; colour 0 kept as E_BLACK
		jr	NE,EncNZ
		nop
		move	r24,r12
EncNZ:
		move	r12,r17
		shlq	#8,r17
		or	r12,r17
		move	r17,r18
		shlq	#16,r18
		or	r18,r17			; the byte four times
		PUT32	r17
		add	r11,r10
		movei	#Encoded,r17
		jump	T,(r17)
		nop
EncShort:
		move	r6,r22			; count copies of the byte
		moveq	#0,r21
		movei	#EncShortDone,r30
		movei	#Append,r17
		jump	T,(r17)
		nop
EncShortDone:
		addq	#1,r6
		movei	#Encoded,r17
		jump	T,(r17)
		nop
EncRaw:
		move	r6,r22
		moveq	#1,r21
		movei	#EncRawDone,r30
		movei	#Append,r17
		jump	T,(r17)
		nop
EncRawDone:
		move	r22,r6
		movei	#Encoded,r17
		jump	T,(r17)
		nop

FrameEnd:
		moveq	#0,r17
		PUT32	r17
		movei	#FrameLoop,r17
		jump	T,(r17)
		nop

; ---- r11 bytes from r22 on (r21 apart: 1 raw, 0 a fill) into the open raw run; back to r30 ----
Append:
		cmpq	#0,r11
		jump	EQ,(r30)
		nop
		cmpq	#0,r15
		jr	PL,AppLoop
		nop
		move	r23,r29
		movei	#Open,r17
		jump	T,(r17)
		nop
AppLoop:
		cmp	r24,r15			; full?
		jump	EQ,(r20)
		nop
AppByte:
		loadb	(r22),r12
		add	r21,r22
		cmpq	#0,r12			; colour 0 kept as E_BLACK (r24: 255)
		jr	NE,AppNZ
		nop
		move	r24,r12
AppNZ:
		PUTB	r12
		addq	#1,r15
		addq	#1,r10
		subq	#1,r11
		jump	NE,(r23)
		nop
		jump	T,(r30)
		nop
AppFull:					; closed, and another opened after it
		movei	#AppReopen,r29
		movei	#Close,r17
		jump	T,(r17)
		nop
AppReopen:
		movei	#AppByte,r29
		movei	#Open,r17
		jump	T,(r17)
		nop

; ---- a raw run opened at first column + pos; back to r29 ----
Open:
		move	r8,r16
		add	r10,r16
		CHK8	r16,r17
		move	r3,r13
		addq	#4,r3			; its header, written when it closes
		jump	T,(r29)
		moveq	#0,r15

; ---- the open raw run, if any, closed: its header, its pixels padded; back to r29 ----
Close:
		cmpq	#0,r15
		jump	MI,(r29)
		nop
		move	r16,r17
		shlq	#24,r17
		shrq	#8,r17
		or	r9,r17
		move	r15,r18
		shlq	#8,r18
		or	r18,r17
		ADDR	r19,r13
		store	r17,(r19)
Pad:
		moveq	#3,r17
		and	r3,r17
		jr	EQ,Padded
		moveq	#0,r18
		PUTB	r18
		jr	T,Pad
		nop
Padded:
		moveq	#0,r15
		subq	#1,r15
		jump	T,(r29)
		nop

Finish:
		moveq	#0,r17
		ADDR	r19,r17
		store	r3,(r19)		; the size, first
		jr	T,Stop
		nop
Fail:
		moveq	#0,r3
Stop:
		store	r3,(r14+3)
		load	(r0),r17		; after the last store has gone out
		or	r17,r17
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

		.68000
		.long
gpu_convert_end:
