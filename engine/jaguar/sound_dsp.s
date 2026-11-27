; The engine test's sounds on Jerry's DSP (engine/jaguar/main.c, eb_sound): four channels mixed
; at each I2S interrupt (SCLK 25: 15980 Hz), each 16-bit sample times its channel's left and right
; volumes (0-255), the sums over 256 saturated to 16 bits. Lives at $F1B000. Assembled by rmac.
;
; Channels (DSP RAM, $F1CF00, four of four longs): the next sample's address, the end's (the
; channel plays while it is below the end), the left volume, the right. The 68000 starts one by
; writing its end 0 (stopped), the volumes, the address, and the end last.
;
; The console's left output is RTXD ($F1A14C) and its right LTXD ($F1A148): the board's DAC puts
; the word sent first on its right (as JagMCP has it from the netlist).
;
; r14 channel  r20 left  r21 right  r22 channels left  r23 Next  r24 Chan  r1-r6 temps

D_FLAGS		.equ	$F1A100
LTXD		.equ	$F1A148
RTXD		.equ	$F1A14C
CHANNELS	.equ	$F1CF00

		.globl	dsp_sound
		.globl	dsp_sound_end

		.text
		.long
dsp_sound:
		.dsp
		.org	$F1B000

		movei	#Main,r0		; $F1B000: the CPU interrupt's place (not used)
		jump	T,(r0)
		nop
		nop
		nop
		nop
		movei	#Isr,r30		; $F1B010: the I2S interrupt
		jump	T,(r30)
		nop

Main:		movei	#$F1D000,r31		; the stack: the top of DSP RAM
		movei	#D_FLAGS,r1
		movei	#$20,r0			; the I2S interrupt on
		store	r0,(r1)
Idle:		jr	T,Idle
		nop

Isr:		movei	#CHANNELS,r14
		moveq	#0,r20
		moveq	#0,r21
		moveq	#4,r22
		movei	#Next,r23
		movei	#Chan,r24
Chan:		load	(r14),r1		; the next sample
		load	(r14+1),r2		; the end
		sub	r1,r2
		jump	MI,(r23)
		nop
		jump	EQ,(r23)
		nop
		loadw	(r1),r3
		addq	#2,r1
		store	r1,(r14)
		shlq	#16,r3
		sharq	#16,r3
		load	(r14+2),r4
		load	(r14+3),r5
		move	r3,r6
		imult	r4,r3
		imult	r5,r6
		add	r3,r20
		add	r6,r21
Next:		subq	#1,r22
		jump	NE,(r24)
		addq	#16,r14
		sharq	#8,r20
		sharq	#8,r21
		sat16s	r20
		sat16s	r21
		movei	#LTXD,r14
		store	r21,(r14)		; right
		store	r20,(r14+1)		; left (RTXD)
		; back: the latch cleared, interrupts on again
		movei	#D_FLAGS,r1
		load	(r1),r2
		bclr	#3,r2
		bset	#10,r2
		load	(r31),r3
		addq	#2,r3
		addq	#4,r31
		jump	T,(r3)
		store	r2,(r1)

		.68000
dsp_sound_end:
