| Chunky to planar for a whole frame, converting only what changed (video.c).
|
| void c2p_frame(const uint8_t *in, uint8_t *shadow, uint8_t *out, int32_t rows,
|	int32_t outStride, int32_t secondLine, int32_t full)
|
| in is the game's frame, 320 bytes a row, and shadow what out shows of it. Each row is compared
| with its shadow a long at a time; a 16-pixel group that differs is copied to the shadow and
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

| Exchanges the bits of x selected by mask with those s places above them: C2P_SWAP_BITS.
	.macro	SWAP_BITS x, mask, s
	move.l	\x,d4
	lsr.l	\s,d4
	eor.l	\x,d4
	and.l	\mask,d4
	eor.l	d4,\x
	lsl.l	\s,d4
	eor.l	d4,\x
	.endm

| Converts the group at (a0)+, copying it to (a1)+, into 8 words at (a2)+ and at secondLine
| (a6) bytes on. c2p16's r0, r1, r2, r3 are d3, d2, d1, d0. Uses d0-d5.
	.macro	GROUP
	movem.l	(a0)+,d0-d3
	movem.l	d0-d3,(a1)
	lea	16(a1),a1
	MERGE	d3, d1, #0x55555555, #1
	MERGE	d2, d0, #0x55555555, #1
	MERGE	d3, d2, #0x33333333, #2
	MERGE	d1, d0, #0x33333333, #2
	moveq	#14,d5
	SWAP_BITS d3, #0x0000cccc, d5
	SWAP_BITS d2, #0x0000cccc, d5
	SWAP_BITS d1, #0x0000cccc, d5
	SWAP_BITS d0, #0x0000cccc, d5
	SWAP_BITS d3, #0x00aa00aa, #7
	SWAP_BITS d2, #0x00aa00aa, #7
	SWAP_BITS d1, #0x00aa00aa, #7
	SWAP_BITS d0, #0x00aa00aa, #7
	moveq	#12,d5
	SWAP_BITS d3, #0x0000f0f0, d5
	SWAP_BITS d2, #0x0000f0f0, d5
	SWAP_BITS d1, #0x0000f0f0, d5
	SWAP_BITS d0, #0x0000f0f0, d5
	move.w	d3,(a2)
	move.w	d1,2(a2)
	move.w	d2,4(a2)
	move.w	d0,6(a2)
	swap	d3
	swap	d1
	swap	d2
	swap	d0
	move.w	d3,8(a2)
	move.w	d1,10(a2)
	move.w	d2,12(a2)
	move.w	d0,14(a2)
	move.l	a6,d4
	beq.s	1f
	move.w	d3,8(a2,a6.l)
	move.w	d1,10(a2,a6.l)
	move.w	d2,12(a2,a6.l)
	move.w	d0,14(a2,a6.l)
	swap	d3
	swap	d1
	swap	d2
	swap	d0
	move.w	d3,(a2,a6.l)
	move.w	d1,2(a2,a6.l)
	move.w	d2,4(a2,a6.l)
	move.w	d0,6(a2,a6.l)
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

	moveq	#79,d7			| longs in the row
scan:	cmpm.l	(a0)+,(a1)+
	dbne	d7,scan
	beq	next			| the rest of the row is unchanged
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
	lsr.w	#2,d7
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
