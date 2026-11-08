| Chunky to planar for a whole frame, converting only what changed (video.c).
|
| void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows,
|	int32_t outStride, int32_t secondLine, int32_t full, uint8_t *copy)
|
| in is the game's frame, 320 bytes a row, and shadow what out shows of it. Each row is compared
| with its shadow a long at a time, four to a group; a 16-pixel group that differs is copied to the shadow and
| converted to 8 interleaved plane words at the same offset in out's row (which is 320 bytes too),
| and again secondLine bytes on when that is not 0 (the TT shows each line twice). Rows of out
| are outStride bytes apart. With full set, every group is converted: the shadow is first made
| to differ from in everywhere. With copy not NULL (and secondLine 0), in is also copied to copy
| as it is read: the game's copy of its frame, and the compare in one pass over in.
|
| The loop that compares and converts must stay inside the 68030's 256-byte instruction cache,
| or each group fetches its code again: masks are kept in registers, and the TT's doubled lines
| and the copy have loops of their own.
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

| Converts the group in d0-d3, copying it to (a1)+, into 8 words at (a2)+, and with second set
| at secondLine (a6) bytes on too. Uses d0-d4; d5 holds 0x00ff00ff and d6 0x55555555, and with
| neither second nor copy d7 0x33333333.
| The longs d0-d3 hold pixels 0-3, 4-7, 8-11 and 12-15. Each step exchanges one bit of a pixel's
| place (which long, or which bit pair, nibble, byte or word in it) with one bit of its place in
| the long, pixel and plane alike, until each long holds two planes' words: d1 planes 0 and 1,
| d3 2 and 3, d0 4 and 5, d2 6 and 7. tests/c2p_frame_test.c checks it against c2p16 in c2p.h.
	.macro	GROUP second, copy
	movem.l	d0-d3,(a1)
	lea	16(a1),a1
	MERGE	d2, d0, d5, #8
	MERGE	d3, d1, d5, #8
	MERGE	d2, d0, d6, #1
	MERGE	d3, d1, d6, #1
	HIGH_LOW d0, d2
	HIGH_LOW d1, d3
	.if \second + \copy
	MERGE	d0, d2, #0x33333333, #2
	MERGE	d1, d3, #0x33333333, #2
	.else
	MERGE	d0, d2, d7, #2
	MERGE	d1, d3, d7, #2
	.endif
	MERGE	d1, d0, #0x0f0f0f0f, #4
	MERGE	d3, d2, #0x0f0f0f0f, #4
	.if \second
	move.l	d1,(a2,a6.l)
	move.l	d3,4(a2,a6.l)
	move.l	d0,8(a2,a6.l)
	move.l	d2,12(a2,a6.l)
	.endif
	move.l	d1,(a2)+
	move.l	d3,(a2)+
	move.l	d0,(a2)+
	move.l	d2,(a2)+
	.endm

| Counts a group of the row done, and goes on to the next while there is one: d7 counts the
| groups with second, and without it a6 points at the end of out's row.
	.macro	NEXT_GROUP second, label
	.if \second
	dbra	d7,\label
	.else
	cmpa.l	a6,a2
	bne	\label
	.endif
	.endm

| The rows: each group read whole, with copy set copied d7 bytes on, and compared with its
| shadow a long at a time from the registers, then converted when one differs. Only the shadow
| has gone past the long that differed; 3: to 6: step back to the group.
	.macro	ROWS second, copy
1:	move.l	a3,a0			| in, shadow and out of the row
	move.l	a4,a1
	move.l	a5,a2
	.if \second
	moveq	#19,d7			| groups in the row
	.else
	lea	320(a5),a6
	.endif
2:	movem.l	(a0)+,d0-d3
	.if \copy
	movem.l	d0-d3,-16(a0,d7.l)
	.endif
	cmp.l	(a1)+,d0
	bne.s	6f
	cmp.l	(a1)+,d1
	bne.s	5f
	cmp.l	(a1)+,d2
	bne.s	4f
	cmp.l	(a1)+,d3
	bne.s	3f
	lea	16(a2),a2
	NEXT_GROUP \second, 2b
	bra	7f
3:	subq.l	#4,a1
4:	subq.l	#4,a1
5:	subq.l	#4,a1
6:	subq.l	#4,a1
	GROUP	\second, \copy
	NEXT_GROUP \second, 2b
7:	lea	320(a3),a3
	lea	320(a4),a4
	adda.l	64(sp),a5
	subq.l	#1,60(sp)
	bne	1b
	.endm

	.text
	.globl	_c2p_frame
_c2p_frame:
	movem.l	d2-d7/a2-a6,-(sp)
	move.l	48(sp),a3		| in, shadow and out rows
	move.l	52(sp),a4
	move.l	56(sp),a5
	move.l	68(sp),a6		| secondLine

	tst.l	72(sp)
	beq.s	compare
	move.l	a3,a0			| full: the shadow made the opposite of in
	move.l	a4,a1
	move.l	60(sp),d7
	mulu.w	#80,d7
	bra.s	2f
1:	move.l	(a0)+,d0
	not.l	d0
	move.l	d0,(a1)+
2:	subq.l	#1,d7
	bcc.s	1b

compare:
	move.l	#0x00ff00ff,d5
	move.l	#0x55555555,d6
	move.l	76(sp),d7
	bne	copying
	move.l	a6,d0
	bne	doubled
	move.l	#0x33333333,d7
	ROWS	0, 0
	bra	done
doubled:
	ROWS	1, 0
	bra	done
copying:
	sub.l	a3,d7			| copy - in
	ROWS	0, 1
done:	movem.l	(sp)+,d2-d7/a2-a6
	rts
