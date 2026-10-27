| The IKBD mouse vector: ikbdsys calls it from the ACIA interrupt with A0 pointing at a relative
| mouse packet. It hands the packet to C, keeping every register the C code may clobber.

	.text
	.globl	_input_mouse_vector
_input_mouse_vector:
	movem.l	d0-d1/a0-a1,-(sp)
	move.l	a0,-(sp)
	jsr	_input_mouse_packet
	addq.l	#4,sp
	movem.l	(sp)+,d0-d1/a0-a1
	rts

| Stands in for scr_dump (0x502), which the VBL calls once Alt-Help has set _dumpflg (0x4EE) to 0:
| the screen dump would print the screen or wait on a printer. Puts _dumpflg back to -1, as the
| dump does when it ends.
	.globl	_input_no_dump
_input_no_dump:
	move.w	#-1,0x4ee.w
	rts
