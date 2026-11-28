; The engine test's movers on Tom's GPU (engine/jaguar/main.c, eb_movers): each frame, each mover's
; picture from its frame's box, those in view put in order (behind ones first, as the core's
; order_of()), and the shape list made for it and the statics in front of it, clipped to it; then
; shapes_gpu.s draws the list. Mirrors draw_movers in engine/core/engine.c. Lives at $F03860, after
; convert_gpu.s. Assembled by rmac.
;
; Parameter block (GPU RAM, $F03F40; longs): +0 the movers (16 bytes each: its static, or 0 when
;   not in view; its fronts' count; their statics' table)  +1 how many  +2 the shapes by type
;   +3 the view's x  +4 its y (pixels)  +5 the frame's pixels  +6 its pitch  +7 the list (main
;   RAM)  +8 its room (entries)  +9 room for those in view (main RAM, 64 bytes each)
;   +10 the shortest row the blitter is given (for shapes_gpu.s)
; A static: +0 type-frame word, +2 x, +4 y (words), +6 z, +16 footprint west, +17 north, +18 up.
; A shape: its size, its frames' offsets; a frame: right, left, above, below (words), its runs.
;
; One in view (64 bytes): +0 its static  +4 its mover  +8 its frame  +12 turned  +16/+20 its hot
; spot in the frame  +24..+36 its picture in the frame, clipped (x0, y0, x1, y1)
;
; r0-r3 a picture (x0, y0, x1, y1)  r4/r5 statics  r6-r9 temps  r10 a frame  r11/r12 a hot spot
; r13 turned  r14 the parameters  r15-r19 loop state  r20 the movers  r21 how many  r22 the shapes
; r23/r24 the view  r25 those in view  r26 how many  r27 a count  r28 the list's end  r29 its
; entries  r30 return  r31 return from Foot (no GPU interrupts here)

G_CTRL		.equ	$F02114
MOVER_PARAMS	.equ	$F03F40
SHAPE_PARAMS	.equ	$F03F80
ORDER		.equ	$F03190		; those in view in drawing order (longs: their offsets)
VIEW_W		.equ	320
VIEW_H		.equ	200

		.globl	gpu_movers
		.globl	gpu_movers_end

		.text
		.long
gpu_movers:
		.gpu
		.org	$F03860

		movei	#MOVER_PARAMS,r14
		load	(r14),r20
		load	(r14+1),r21
		load	(r14+2),r22
		load	(r14+3),r23
		load	(r14+4),r24
		load	(r14+9),r25
		load	(r14+7),r28
		moveq	#0,r26
		moveq	#0,r29
		moveq	#0,r27

; ---- each mover: in view? then into the order ----
InView:
		cmp	r21,r27			; i - n
		movei	#Drawing,r6
		jump	PL,(r6)
		nop
		move	r27,r15
		shlq	#4,r15
		add	r20,r15			; its mover
		load	(r15),r4		; its static
		addq	#1,r27
		cmpq	#0,r4
		movei	#InView,r6
		jump	EQ,(r6)
		nop
		movei	#AfterFrame1,r30
		movei	#Frame,r6
		jump	T,(r6)
		nop
AfterFrame1:
		cmpq	#0,r10
		movei	#InView,r6
		jump	EQ,(r6)
		nop
		; its picture in the frame: in view?
		sub	r23,r0
		sub	r24,r1
		sub	r23,r2
		sub	r24,r3
		movei	#InView,r6
		cmpq	#0,r2
		jump	MI,(r6)			; right of it left of the view
		nop
		cmpq	#0,r3
		jump	MI,(r6)
		nop
		movei	#VIEW_W,r7
		cmp	r7,r0			; x0 - VIEW_W
		jump	PL,(r6)
		nop
		movei	#VIEW_H,r7
		cmp	r7,r1
		jump	PL,(r6)
		nop
		; its record
		move	r26,r16
		shlq	#6,r16			; its offset among those in view
		move	r25,r7
		add	r16,r7
		store	r4,(r7)
		addq	#4,r7
		store	r15,(r7)
		addq	#4,r7
		store	r10,(r7)
		addq	#4,r7
		store	r13,(r7)
		addq	#4,r7
		sub	r23,r11
		store	r11,(r7)
		addq	#4,r7
		sub	r24,r12
		store	r12,(r7)
		addq	#4,r7
		; clipped to the frame
		cmpq	#0,r0
		jr	PL,Clip0
		nop
		moveq	#0,r0
Clip0:		cmpq	#0,r1
		jr	PL,Clip1
		nop
		moveq	#0,r1
Clip1:		movei	#VIEW_W-1,r8
		cmp	r8,r2			; x1 - (VIEW_W - 1)
		jr	MI,Clip2
		nop
		move	r8,r2
Clip2:		movei	#VIEW_H-1,r8
		cmp	r8,r3
		jr	MI,Clip3
		nop
		move	r8,r3
Clip3:		store	r0,(r7)
		addq	#4,r7
		store	r1,(r7)
		addq	#4,r7
		store	r2,(r7)
		addq	#4,r7
		store	r3,(r7)
		; into the order: before each it is behind (insertion, as the core)
		move	r4,r17			; this one's static
		move	r26,r18			; k
		addq	#1,r26
SortLoop:
		cmpq	#0,r18
		movei	#SortDone,r6
		jump	EQ,(r6)
		nop
		move	r18,r19
		subq	#1,r19
		shlq	#2,r19
		movei	#ORDER,r6
		add	r6,r19			; order[k - 1]'s place
		load	(r19),r9		; its offset
		add	r25,r9
		load	(r9),r5			; its static
		move	r17,r4
		movei	#AfterOrder,r30
		movei	#Order,r6
		jump	T,(r6)
		nop
AfterOrder:
		cmpq	#0,r10
		movei	#SortDone,r6
		jump	PL,(r6)			; not behind it: here
		nop
		load	(r19),r9		; behind it: it moves up one
		move	r19,r6
		addq	#4,r6
		store	r9,(r6)
		subq	#1,r18
		movei	#SortLoop,r6
		jump	T,(r6)
		nop
SortDone:
		move	r18,r19
		shlq	#2,r19
		movei	#ORDER,r6
		add	r6,r19
		store	r16,(r19)
		movei	#InView,r6
		jump	T,(r6)
		nop

; ---- the list: each in view, then those in front of it, clipped to it ----
Drawing:
		moveq	#0,r27
DrawLoop:
		cmp	r26,r27
		movei	#Done,r6
		jump	PL,(r6)
		nop
		move	r27,r19
		shlq	#2,r19
		movei	#ORDER,r6
		add	r6,r19
		load	(r19),r17
		add	r25,r17			; its record
		addq	#1,r27
		move	r17,r7
		addq	#8,r7
		load	(r7),r10		; its frame
		addq	#4,r7
		load	(r7),r13
		addq	#4,r7
		load	(r7),r11
		addq	#4,r7
		load	(r7),r12
		moveq	#0,r0			; the whole frame
		moveq	#0,r1
		movei	#VIEW_W-1,r2
		movei	#VIEW_H-1,r3
		movei	#AfterEmit1,r30
		movei	#Emit,r6
		jump	T,(r6)
		nop
AfterEmit1:
		move	r17,r7
		addq	#4,r7
		load	(r7),r15		; its mover
		addq	#4,r15
		load	(r15),r18		; its fronts
		addq	#4,r15
		load	(r15),r15		; their statics' table
FrontLoop:
		cmpq	#0,r18
		movei	#DrawLoop,r6
		jump	EQ,(r6)
		nop
		subq	#1,r18
		load	(r15),r4
		addq	#4,r15
		movei	#AfterFrame2,r30
		movei	#Frame,r6
		jump	T,(r6)
		nop
AfterFrame2:
		cmpq	#0,r10
		movei	#FrontLoop,r6
		jump	EQ,(r6)
		nop
		sub	r23,r11
		sub	r24,r12
		move	r17,r7
		movei	#24,r8
		add	r8,r7
		load	(r7),r0			; its mover's picture, clipped
		addq	#4,r7
		load	(r7),r1
		addq	#4,r7
		load	(r7),r2
		addq	#4,r7
		load	(r7),r3
		movei	#FrontLoop,r30
		movei	#Emit,r6
		jump	T,(r6)
		nop

Done:
		movei	#SHAPE_PARAMS,r6
		load	(r14+7),r7
		store	r7,(r6)
		addq	#4,r6
		store	r29,(r6)
		addq	#4,r6
		load	(r14+10),r7
		store	r7,(r6)
		movei	#$F03000,r6		; shapes_gpu.s draws it
		jump	T,(r6)
		nop

; ---- a static's frame (r4): r10 the frame (0: none), r11/r12 the hot spot, r13 turned, r0-r3
; its picture (world pixels); back to r30. Uses r5-r9. ----
Frame:
		loadw	(r4),r5			; type-frame
		move	r5,r13
		shrq	#15,r13			; turned
		move	r5,r6
		shlq	#22,r6
		shrq	#22,r6			; type
		shrq	#10,r5
		moveq	#31,r7
		and	r7,r5			; frame
		shlq	#2,r6
		add	r22,r6
		load	(r6),r6			; the shape
		moveq	#0,r10
		cmpq	#0,r6
		jump	EQ,(r30)
		nop
		move	r6,r7
		addq	#4,r7
		load	(r7),r8			; its first frame's offset
		addq	#1,r5
		shlq	#2,r5			; (frame + 1) * 4
		cmp	r8,r5
		jump	PL,(r30)		; no such frame
		nop
		add	r6,r5
		cmpq	#0,r10		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r5),r10
		add	r6,r10			; the frame
		loadw	(r10),r0		; right
		move	r10,r7
		addq	#2,r7
		loadw	(r7),r1			; left
		addq	#2,r7
		loadw	(r7),r2			; above
		addq	#2,r7
		loadw	(r7),r3			; below
		shlq	#16,r0
		sharq	#16,r0
		shlq	#16,r1
		sharq	#16,r1
		shlq	#16,r2
		sharq	#16,r2
		shlq	#16,r3
		sharq	#16,r3
		cmpq	#0,r13
		jr	EQ,Unturned
		nop
		move	r1,r9			; turned: left and above, right and below change
		move	r2,r1
		move	r9,r2
		move	r0,r9
		move	r3,r0
		move	r9,r3
Unturned:
		move	r4,r7
		addq	#2,r7
		loadw	(r7),r11		; x
		addq	#2,r7
		loadw	(r7),r12		; y
		addq	#2,r7
		loadb	(r7),r9			; z
		shlq	#16,r11
		sharq	#16,r11
		shlq	#16,r12
		sharq	#16,r12
		shlq	#3,r11
		addq	#7,r11
		shlq	#3,r12
		addq	#7,r12
		shlq	#2,r9
		sub	r9,r11			; the hot spot: x * 8 + 7 - z * 4
		sub	r9,r12
		move	r11,r9
		sub	r1,r9			; x0 = px - left
		move	r12,r1
		sub	r2,r1			; y0 = py - above
		add	r11,r0			; x1 = px + right
		move	r12,r2
		add	r3,r2			; y1 = py + below
		move	r2,r3
		move	r0,r2
		move	r9,r0
		jump	T,(r30)
		nop

; ---- order_of(r4, r5): r10 -1 (r4 behind r5), 1 (in front), or 0; back to r30. As the core's:
; by their footprints' ranges, what lies south or east, or above, is in front. Uses r0-r3,
; r5-r13, r15, r31. ----
Order:
		move	r4,r6
		movei	#AfterFootA,r31
		movei	#Foot,r9
		jump	T,(r9)
		nop
AfterFootA:
		move	r0,r11			; a: x0 r11, x1 r12, y0 r13, y1 r10, z0 r15, z1 (later) r5
		move	r1,r12
		move	r2,r13
		move	r3,r10
		move	r7,r15
		move	r5,r6
		move	r8,r5
		movei	#AfterFootB,r31
		movei	#Foot,r9
		jump	T,(r9)
		nop
AfterFootB:					; b: x0 r0, x1 r1, y0 r2, y1 r3, z0 r7, z1 r8
		moveq	#0,r9			; xc
		cmp	r0,r12			; a.x1 - b.x0
		jr	PL,XNotBehind
		nop
		jr	T,XDone
		subq	#1,r9
XNotBehind:	cmp	r11,r1			; b.x1 - a.x0
		jr	PL,XDone
		nop
		cmpq	#0,r9		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r9
XDone:		moveq	#0,r6			; yc
		cmp	r2,r10			; a.y1 - b.y0
		jr	PL,YNotBehind
		nop
		jr	T,YDone
		subq	#1,r6
YNotBehind:	cmp	r13,r3			; b.y1 - a.y0
		jr	PL,YDone
		nop
		cmpq	#0,r6		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r6
YDone:		moveq	#0,r0			; zc
		cmp	r7,r5			; a.z1 - b.z0: <= 0, behind
		jr	MI,ZBehind
		nop
		jr	NE,ZNotBehind
		nop
ZBehind:	jr	T,ZDone
		subq	#1,r0
ZNotBehind:	cmp	r8,r15			; a.z0 - b.z1: >= 0, in front
		jr	MI,ZDone
		nop
		cmpq	#0,r0		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r0
ZDone:
		; xc r9, yc r6, zc r0
		move	r9,r1
		or	r6,r1
		jr	NE,NotBoth
		nop
		jump	T,(r30)			; neither across nor along: by level
		move	r0,r10
NotBoth:	cmpq	#0,r0
		jr	EQ,NoLevel
		nop
		cmpq	#0,r9			; level, if across and along agree with it
		jr	EQ,XAgrees
		nop
		cmp	r0,r9
		jr	NE,NoLevel
		nop
XAgrees:	cmpq	#0,r6
		jr	EQ,YAgrees
		nop
		cmp	r0,r6
		jr	NE,NoLevel
		nop
YAgrees:	jump	T,(r30)
		move	r0,r10
NoLevel:	cmpq	#0,r9
		jr	NE,HasX
		nop
		jump	T,(r30)			; along alone
		move	r6,r10
HasX:		cmpq	#0,r6
		jr	NE,HasBoth
		nop
		jump	T,(r30)			; across alone
		move	r9,r10
HasBoth:	moveq	#0,r10
		cmp	r6,r9
		jr	NE,Disagree
		nop
		cmpq	#0,r10		; (read, not written, first: Atari's GPU/DSP bug 2)
		move	r9,r10
Disagree:	jump	T,(r30)
		nop

; the footprint of static r6: r0 x0, r1 x1, r2 y0, r3 y1 (cells), r7 z0, r8 z1; back to r31.
; Uses r9.
Foot:
		move	r6,r9
		addq	#2,r9
		loadw	(r9),r1			; x
		addq	#2,r9
		loadw	(r9),r3			; y
		addq	#2,r9
		loadb	(r9),r7			; z
		shlq	#16,r1
		sharq	#16,r1
		shlq	#16,r3
		sharq	#16,r3
		addq	#10,r9			; +16
		loadb	(r9),r0			; west
		addq	#1,r9
		loadb	(r9),r2			; north
		addq	#1,r9
		loadb	(r9),r8			; up
		neg	r0
		add	r1,r0			; x0 = x - fx
		neg	r2
		add	r3,r2			; y0 = y - fy
		add	r7,r8			; z1 = z + height
		jump	T,(r31)
		nop

; ---- an entry (shapes_gpu.s's ten longs) at the list's end, unless it is full: the frame r10,
; its hot spot r11/r12, turned r13, clipped to r0-r3; back to r30. Uses r5-r9. ----
Emit:
		load	(r14+8),r5
		cmp	r5,r29			; entries - room
		jump	PL,(r30)
		nop
		addq	#1,r29
		load	(r14+5),r6		; the frame's pixels
		load	(r14+6),r7		; its pitch
		move	r10,r5
		addq	#8,r5			; the runs, after the box
		store	r5,(r28)
		addq	#4,r28
		store	r6,(r28)
		addq	#4,r28
		cmpq	#0,r8		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r8
		cmpq	#0,r13
		movei	#EmitTurned,r9
		jump	NE,(r9)
		nop
		store	r11,(r28)		; along: x
		addq	#4,r28
		store	r12,(r28)		; across: y
		addq	#4,r28
		store	r0,(r28)
		addq	#4,r28
		store	r1,(r28)
		addq	#4,r28
		store	r2,(r28)
		addq	#4,r28
		store	r3,(r28)
		addq	#4,r28
		store	r8,(r28)		; along a pixel
		addq	#4,r28
		store	r7,(r28)		; across a row
		addq	#4,r28
		jump	T,(r30)
		nop
EmitTurned:
		store	r12,(r28)		; along: y
		addq	#4,r28
		store	r11,(r28)
		addq	#4,r28
		store	r1,(r28)
		addq	#4,r28
		store	r0,(r28)
		addq	#4,r28
		store	r3,(r28)
		addq	#4,r28
		store	r2,(r28)
		addq	#4,r28
		store	r7,(r28)		; along a row
		addq	#4,r28
		store	r8,(r28)		; across a pixel
		addq	#4,r28
		jump	T,(r30)
		nop

		.68000
		.long
gpu_movers_end:
