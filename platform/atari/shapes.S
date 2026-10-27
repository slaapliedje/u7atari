| Shape drawing in copy mode, unflipped: most of the game's drawing time.
|
| void shape_spans_copy(const uint8_t *source, int32_t x, int32_t y, const int16_t clip[4],
|	const uint8_t *rowTable, uint8_t *linearBase)
|
| Called by DrawShapeFrame (game/main/xmm/shapedraw.cpp in the fork) with source at a frame's
| first span; clip is the view's x0, y0, x1, y1 and rowTable its table of little-endian linear
| addresses, one a row. Spans are drawn run by run, with longs for runs of 8 pixels or more (the
| 030 takes them at any address); spans wholly inside the clip box, most of them, skip the
| clipping arithmetic. Matches DrawSpans<SHAPE_COPY, false> exactly (tests/shapes_test.cpp).
|
| d0 span length, then pixels left in it	a0 source
| d1 along (x), then fill value			a1 screen
| d2 across (y), temp				a2 row table
| d3 temp					a3 linear base
| d4/d5 clip x0/x1				a4/a5 clip y0/y1
| d6/d7 x/y					a6 temp

| Copies \n > 0 bytes from (a0)+ to (a1)+.
	.macro	COPY n, t
	cmp.w	#8,\n
	blt.s	2f
	move.w	\n,\t
	lsr.w	#2,\t
	subq.w	#1,\t
1:	move.l	(a0)+,(a1)+
	dbra	\t,1b
	and.w	#3,\n
2:	subq.w	#1,\n
	bmi.s	3f
4:	move.b	(a0)+,(a1)+
	dbra	\n,4b
3:
	.endm

| Stores \n >= 0 bytes of value \v (its low byte; the rest is overwritten) at (a1)+.
	.macro	FILL n, v, t
	cmp.w	#8,\n
	blt.s	2f
	move.b	\v,\t
	lsl.w	#8,\t
	move.b	\v,\t
	move.w	\t,\v
	swap	\v
	move.w	\t,\v
	move.w	\n,\t
	lsr.w	#2,\t
	subq.w	#1,\t
1:	move.l	\v,(a1)+
	dbra	\t,1b
	and.w	#3,\n
2:	subq.w	#1,\n
	bmi.s	3f
4:	move.b	\v,(a1)+
	dbra	\n,4b
3:
	.endm

| Reads the span's offsets and leaves a1 at its first pixel on the screen, or branches to
| \skip if its row is outside the clip box, or to \clip, with a1 at the row, if it crosses the
| box's left or right edge.
	.macro	POSITION skip, clip
	move.w	(a0)+,d1
	ror.w	#8,d1
	add.w	d6,d1
	move.w	(a0)+,d2
	ror.w	#8,d2
	add.w	d7,d2
	cmp.w	a4,d2
	blt	\skip
	cmp.w	a5,d2
	bgt	\skip
	move.l	(a2,d2.w*4),d3
	ror.w	#8,d3
	swap	d3
	ror.w	#8,d3
	lea	(a3,d3.l),a1
	cmp.w	d4,d1
	blt	\clip
	move.w	d1,d2
	add.w	d0,d2
	subq.w	#1,d2
	cmp.w	d5,d2
	bgt	\clip
	adda.w	d1,a1
	.endm

| For a span of d0 pixels from x d1 on the row at a1 that crosses the clip box's left or right
| edge: d2 = pixels to pass over, d3 = pixels to draw, a1 = where the first goes; or branches to
| \skip if none are visible.
	.macro	VISIBLE skip
	move.w	d4,d2
	sub.w	d1,d2
	bpl.s	1f
	moveq	#0,d2
1:	move.w	d5,d3
	sub.w	d1,d3
	cmp.w	d0,d3
	blt.s	2f
	move.w	d0,d3
	subq.w	#1,d3
2:	sub.w	d2,d3
	addq.w	#1,d3
	ble	\skip
	adda.w	d1,a1
	adda.w	d2,a1
	.endm

	.text
	.globl	_shape_spans_copy
_shape_spans_copy:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a0
	move.l	52(sp),d6
	move.l	56(sp),d7
	move.l	60(sp),a1
	move.l	64(sp),a2
	move.l	68(sp),a3
	move.w	(a1),d4
	movea.w	2(a1),a4
	move.w	4(a1),d5
	movea.w	6(a1),a5

span:	move.w	(a0)+,d0
	ror.w	#8,d0
	lsr.w	#1,d0			| length; C set if run-encoded
	beq	done
	bcs	encoded

	POSITION raw_skip, raw_clip
	COPY	d0, d2
	bra	span
raw_skip:
	adda.w	d0,a0
	bra	span
raw_clip:
	VISIBLE	raw_skip
	lea	(a0,d0.w),a6		| the span's end
	adda.w	d2,a0
	COPY	d3, d2
	move.l	a6,a0
	bra	span

encoded:
	POSITION rle_skip, rle_clip
run:	moveq	#0,d3
	move.b	(a0)+,d3
	lsr.w	#1,d3			| count; C set for a fill
	bcs	fill
	beq	next
	sub.w	d3,d0
	COPY	d3, d2
	tst.w	d0
	bgt	run
	bra	span
fill:	move.b	(a0)+,d1
	sub.w	d3,d0
	FILL	d3, d1, d2
next:	tst.w	d0
	bgt	run
	bra	span

rle_skip:
	moveq	#0,d3
	move.b	(a0)+,d3
	lsr.w	#1,d3
	bcc.s	1f
	addq.l	#1,a0
	bra.s	2f
1:	adda.w	d3,a0
2:	sub.w	d3,d0
	bgt.s	rle_skip
	bra	span
| A run-encoded span that crosses the clip box's left or right edge: d2 pixels to pass over,
| then d3 to draw, and d0 still counts down the span. d6 and d7 are borrowed.
rle_clip:
	VISIBLE	rle_skip
	movem.l	d6-d7,-(sp)
clip_run:
	moveq	#0,d1
	move.b	(a0)+,d1
	lsr.w	#1,d1			| count; C set for a fill
	bcs.s	clip_fill
	sub.w	d1,d0
	cmp.w	d2,d1
	bgt.s	1f
	sub.w	d1,d2			| all passed over
	adda.w	d1,a0
	bra.s	clip_next
1:	adda.w	d2,a0
	sub.w	d2,d1			| pixels in the run from the first drawn one
	moveq	#0,d2
	move.w	d1,d7
	cmp.w	d3,d1
	ble.s	2f
	move.w	d3,d1
2:	sub.w	d1,d3
	sub.w	d1,d7			| run pixels after the drawn ones
	COPY	d1, d6
	adda.w	d7,a0
	bra.s	clip_next
clip_fill:
	move.b	(a0)+,d7
	sub.w	d1,d0
	cmp.w	d2,d1
	bgt.s	1f
	sub.w	d1,d2
	bra.s	clip_next
1:	sub.w	d2,d1
	moveq	#0,d2
	cmp.w	d3,d1
	ble.s	2f
	move.w	d3,d1
2:	sub.w	d1,d3
	FILL	d1, d7, d6
clip_next:
	tst.w	d3
	beq.s	1f
	tst.w	d0
	bgt	clip_run
1:	movem.l	(sp)+,d6-d7
	tst.w	d0			| pass over what is left of the span
	bgt	rle_skip
	bra	span

done:	movem.l	(sp)+,d2-d7/a2-a6
	rts
