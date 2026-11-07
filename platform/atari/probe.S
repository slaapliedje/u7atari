| bool probe_long(uint32_t address, uint32_t *value): reads a long in supervisor mode, with bus
| errors caught, to find out whether a card is there.

	.text
	.globl	_probe_long
_probe_long:
	movea.l	4(sp),a0
	movea.l	8(sp),a1
	move.l	0x8.w,d1		| the bus error vector
	move.l	sp,saved_sp
	move.l	#bus_error,0x8.w
	move.l	(a0),(a1)
	nop
	moveq	#1,d0
	bra	done
bus_error:
	move.l	saved_sp,sp
	moveq	#0,d0
done:
	move.l	d1,0x8.w
	rts

	.bss
	.even
saved_sp:
	.skip	4
