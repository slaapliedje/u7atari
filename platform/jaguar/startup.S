/* Jaguar start-up: the boot ROM jumps here, to $802000 in the cartridge, where the program
 * runs in place (jaguar.ld). Follows jagwrestle's startup.s (rmac) in GNU syntax, less the
 * video set-up, which video.c does.
 *
 * Sets the GPU and DSP to big-endian, masks interrupts, sets the stack at the top of RAM,
 * copies .data from the cartridge to RAM, clears .bss, runs the C++ constructors, then main.
 * jag_vbl is the vertical-blank handler (LEVEL0, $100): the Object Processor overwrites a
 * bitmap object's first phrase as it draws it, so the handler puts it back from jag_vbl_phrase
 * each frame, counts jag_ticks and calls jag_vbl_hook if one is set. */

	.equ	G_END, 0xF0210C
	.equ	D_END, 0xF1A10C
	.equ	G_CTRL, 0xF02114
	.equ	D_CTRL, 0xF1A114
	.equ	VI, 0xF0004E
	.equ	INT1, 0xF000E0
	.equ	INT2, 0xF000E2

	.section .text.start, "ax"
	.globl	_start
_start:
	move.w	#0x2700, %sr
	move.l	#0x70007, G_END
	move.l	#0x70007, D_END
	clr.l	G_CTRL
	clr.l	D_CTRL
	move.w	#0xFFFF, VI
	move.l	#__stack, %sp

	/* .data: from the cartridge to RAM */
	lea	__data_load, %a0
	lea	__data_start, %a1
	lea	__data_end, %a2
1:	cmp.l	%a2, %a1
	bcc.s	2f
	move.w	(%a0)+, (%a1)+
	bra.s	1b
2:
	/* .bss: cleared */
	lea	__bss_start, %a1
	lea	__bss_end, %a2
3:	cmp.l	%a2, %a1
	bcc.s	4f
	clr.w	(%a1)+
	bra.s	3b
4:
	/* exceptions: bus and address errors, illegal instructions and the like to jag_crash */
	move.l	#exc_bus, 0x08
	move.l	#exc_address, 0x0C
	lea	exc_short, %a0
	lea	0x10, %a1
	moveq	#8-1, %d0
8:	move.l	%a0, (%a1)+
	addq.l	#8, %a0               /* exc_step */
	dbra	%d0, 8b

	/* C++ constructors */
	lea	__init_array_start, %a3
5:	cmp.l	#__init_array_end, %a3
	bcc.s	6f
	move.l	(%a3)+, %a0
	jsr	(%a0)
	bra.s	5b
6:
	jsr	jag_main
7:	bra.s	7b

	.text
	.globl	jag_vbl
jag_vbl:
	movem.l	%d0-%d1/%a0-%a1, -(%sp)
	move.l	jag_vbl_object, %a0
	move.l	%a0, %d0
	beq.s	1f
	move.l	jag_vbl_phrase, (%a0)
	move.l	jag_vbl_phrase+4, 4(%a0)
1:	addq.l	#1, jag_ticks
	move.l	jag_vbl_hook, %a0
	move.l	%a0, %d0
	beq.s	2f
	jsr	(%a0)
2:	move.w	#0x101, INT1
	move.w	#0, INT2
	movem.l	(%sp)+, %d0-%d1/%a0-%a1
	rte

/* Exceptions. Vectors 4 to 11 each have a stub of exc_step bytes pushing its number; bus and
 * address errors (2, 3) have a longer frame. jag_crash (main.c) logs and stops:
 * jag_crash(long_frame, frame), frame pointing at the vector number, the 68000's exception frame
 * after it and d0-d7/a0-a6 before it. */
	.equ	exc_step, 8
exc_short:
	.irp	n, 4, 5, 6, 7, 8, 9, 10, 11
	move.w	#\n, -(%sp)
	bra.w	exc_common_short
	.endr
exc_bus:
	move.w	#2, -(%sp)
	bra.s	exc_common_long
exc_address:
	move.w	#3, -(%sp)
exc_common_long:
	moveq	#1, %d1
	bra.s	exc_common
exc_common_short:
	moveq	#0, %d1
exc_common:
	move.w	#0x2700, %sr
	movem.l	%d0-%d7/%a0-%a6, -(%sp)
	lea	60(%sp), %a0
	move.l	%a0, -(%sp)
	move.l	%d1, -(%sp)
	jsr	jag_crash
9:	bra.s	9b

	.bss
	.balign	4
	.globl	jag_ticks, jag_vbl_object, jag_vbl_phrase, jag_vbl_hook
jag_ticks:	.space	4
jag_vbl_object:	.space	4
jag_vbl_phrase:	.space	8
jag_vbl_hook:	.space	4
