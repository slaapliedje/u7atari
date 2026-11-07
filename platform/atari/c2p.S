| Chunky to planar for a whole frame, converting only what changed (video.c).
|
| void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows,
|	int32_t outStride, int32_t secondLine, int32_t full)
|
| in is the game's frame, 320 bytes a row, and shadow what out shows of it. Each row is compared
| with its shadow a long at a time, four to a group; a 16-pixel group that differs is copied to the shadow and
| converted to 8 interleaved plane words at the same offset in out's row (which is 320 bytes too),
| and again secondLine bytes on when that is not 0 (the TT shows each line twice). Rows of out
| are outStride bytes apart. With full set, every group is converted.
|
| The conversion is c2p16 in c2p.h, which tests/c2p_test.c checks; tests/c2p_frame_test.c checks
| this against it.

| x and y exchange the bits of t = ((x >> s) ^ y) & mask: C2P_MERGE.
	.macro	MERGE x, y, mask, s
	move.l	\x,d4
	lsr.l	\s,d4
	eor.l	\y,d4
	and.l	\mask,d4
	eor.l	d4,\y
	lsl.l	\s,d4
	eor.l	d4,\x
	.endm

| Exchanges the high word of x with the low word of y.
	.macro	HIGH_LOW x, y
	swap	\x
	eor.w	\x,\y
	eor.w	\y,\x
	eor.w	\x,\y
	swap	\x
	.endm

| Converts the group at (a0)+, copying it to (a1)+, into 8 words at (a2)+ and at secondLine
| (a6) bytes on. Uses d0-d4.
| The longs d0-d3 hold pixels 0-3, 4-7, 8-11 and 12-15. Each step exchanges one bit of a pixel's
| place (which long, or which bit pair, nibble, byte or word in it) with one bit of its place in
| the long, pixel and plane alike, until each long holds two planes' words: d1 planes 0 and 1,
| d3 2 and 3, d0 4 and 5, d2 6 and 7. tests/c2p_frame_test.c checks it against c2p16 in c2p.h.
	.macro	GROUP
	movem.l	(a0)+,d0-d3
	movem.l	d0-d3,(a1)
	lea	16(a1),a1
	MERGE	d2, d0, #0x00ff00ff, #8
	MERGE	d3, d1, #0x00ff00ff, #8
	MERGE	d2, d0, #0x55555555, #1
	MERGE	d3, d1, #0x55555555, #1
	HIGH_LOW d0, d2
	HIGH_LOW d1, d3
	MERGE	d0, d2, #0x33333333, #2
	MERGE	d1, d3, #0x33333333, #2
	MERGE	d1, d0, #0x0f0f0f0f, #4
	MERGE	d3, d2, #0x0f0f0f0f, #4
	move.l	d1,(a2)
	move.l	d3,4(a2)
	move.l	d0,8(a2)
	move.l	d2,12(a2)
	move.l	a6,d4
	beq.s	1f
	move.l	d1,(a2,a6.l)
	move.l	d3,4(a2,a6.l)
	move.l	d0,8(a2,a6.l)
	move.l	d2,12(a2,a6.l)
1:	lea	16(a2),a2
	.endm

	.text
	.globl	_c2p_frame
_c2p_frame:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a3		| in, shadow and out rows
	move.l	52(sp),a4
	move.l	56(sp),a5
	move.l	68(sp),a6		| secondLine

row:	move.l	a3,a0
	move.l	a4,a1
	move.l	a5,a2
	tst.l	72(sp)
	bne	full

	moveq	#19,d7			| groups in the row
scan:	cmpm.l	(a0)+,(a1)+
	bne.s	differs
	cmpm.l	(a0)+,(a1)+
	bne.s	differs
	cmpm.l	(a0)+,(a1)+
	bne.s	differs
	cmpm.l	(a0)+,(a1)+
	dbne	d7,scan
	beq	next			| the rest of the row is unchanged
differs:
	move.l	a0,d6			| the group holding the long before a0
	sub.l	a3,d6
	subq.l	#4,d6
	and.w	#0xfff0,d6
	lea	(a3,d6.l),a0
	lea	(a4,d6.l),a1
	lea	(a5,d6.l),a2
	GROUP
	add.w	#16,d6
	move.w	#320,d7
	sub.w	d6,d7
	beq	next
	lsr.w	#4,d7
	subq.w	#1,d7
	bra	scan

full:	moveq	#19,d7			| groups in the row
group:	GROUP
	dbra	d7,group

next:	lea	320(a3),a3
	lea	320(a4),a4
	adda.l	64(sp),a5
	subq.l	#1,60(sp)
	bne	row

	movem.l	(sp)+,d2-d7/a2-a6
	rts
