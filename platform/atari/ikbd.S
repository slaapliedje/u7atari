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

| The MIDI system vector (sound.c): TOS's ACIA interrupt calls it, then its own one for what came
| in, for as long as either ACIA asks for service. While the transmit interrupt is on, sends the
| next queued byte once the transmitter is empty, and turns the interrupt off when the queue is.
	.equ	MIDI_QUEUE_SIZE, 1024		| sound.c has it too
	.globl	_midi_system_vector
_midi_system_vector:
	tst.b	_midi_sending
	beq.s	2f
	btst	#1,0xfffffc04.w			| transmitter empty
	beq.s	2f
	move.w	_midi_queue_out,d1
	cmp.w	_midi_queue_in,d1
	beq.s	1f
	lea	_midi_queue,a0
	move.b	(a0,d1.w),0xfffffc06.w
	addq.w	#1,d1
	and.w	#MIDI_QUEUE_SIZE-1,d1
	move.w	d1,_midi_queue_out
	bra.s	2f
1:	move.b	#0x95,0xfffffc04.w		| transmit interrupt off
	clr.b	_midi_sending
2:	move.l	_midi_old_system_vector,-(sp)
	rts
