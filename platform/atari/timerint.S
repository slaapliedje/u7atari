| The Timer B interrupt that drives the sound tick (timer.c).
|
| The MFP runs in software end-of-interrupt mode, and Timer B outranks the system timer and the
| keyboard and MIDI ACIAs, so the in-service bit is cleared at once and the tick runs at IPL 5:
| a tick that sends many MIDI bytes must not hold up _hz_200 or lose keyboard bytes. The tick
| runs on its own stack, since the interrupted code may be on a small one inside TOS. Only one
| interrupt at a time gets past timer_sound_due; one arriving during a tick just counts itself.

	.text
	.globl	_timer_sound_vector
_timer_sound_vector:
	movem.l	d0-d1/a0-a2,-(sp)
	move.b	#0xfe,0xfffffa0f.w		| ISRA: Timer B (bit 0) no longer in service
	jsr	_timer_sound_due
	tst.l	d0
	beq.s	1f
	move.l	sp,a2
	lea	sound_stack_top,sp
	move.w	#0x2500,sr
	jsr	_timer_sound_run
	ori.w	#0x0700,sr
	move.l	a2,sp
1:	movem.l	(sp)+,d0-d1/a0-a2
	rte

	.bss
	.even
	.skip	8192
sound_stack_top:
