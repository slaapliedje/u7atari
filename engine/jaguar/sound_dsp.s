; The engine test's sound on Jerry's DSP (engine/jaguar/main.c, eb_sound and eb_music): four
; channels of samples and the music's OPL2, mixed at each I2S interrupt (SCLK 25: 15980 Hz).
; Lives at $F1B000. Assembled by rmac.
;
; The interrupt (register bank 0): each channel's 16-bit sample times its left and right volumes
; (0-255), and the music's next sample (from OBUF, made ahead) times its volume (0-256); the sums
; over 256 saturated to 16 bits.
;
; The main loop (bank 1): the OPL2 of core/opl.c, step for step (its reference: the same
; numbers), a block of 16 samples at a time into OBUF while there is room: the ticks due from
; the song's stream (a ring in DRAM the 68000 fills), the envelopes, each operator's step, wave
; and gain, then the samples. The 68000 gives it the tables (e_opl_tables) and a restart
; (M_CMD 1: the synth silenced, the ring's place 0; it says 0 when done).
;
; Channels (DSP RAM, $F1CF00, four of four longs): the next sample's address, the end's (the
; channel plays while it is below the end), the left volume, the right. The 68000 starts one by
; writing its end 0 (stopped), the volumes, the address, and the end last.
;
; The console's left output is RTXD ($F1A14C) and its right LTXD ($F1A148): the board's DAC puts
; the word sent first on its right (as JagMCP has it from the netlist).

D_FLAGS		.equ	$F1A100
LTXD		.equ	$F1A148
RTXD		.equ	$F1A14C

; DSP RAM (the code from $F1B000 below HSINE)
HSINE		.equ	$F1BC00		; 512 longs: half a sine (13 bits)
GAIN32		.equ	$F1C400		; 32: 65535 * 2^(-k/32)
DECAY		.equ	$F1C480		; 64: each rate's step a block (attenuation << 16)
ATTACK		.equ	$F1C580		; 64: each rate's fraction kept a block (<< 16)
MIX		.equ	$F1C680		; 16: the block's samples summed
CHANS		.equ	$F1C860		; 9 of 8 longs
OPS		.equ	$F1C980		; 18 of 16 longs: a channel's modulator, then its carrier
OBUF		.equ	$F1CE00		; 64 longs: the music's samples, made ahead
CHANNELS	.equ	$F1CF00		; the samples' channels
MCTL		.equ	$F1CF40		; the music's: 34 longs

; MCTL, longs: the ring's address, its size - 1, the end of what is written (the 68000's), the
; place read, the command, the end reached, the volume, OBUF's places (the interrupt's, the main
; loop's), the ticks to wait, the ticks' fraction, the LFOs' phases, waveform select, blocks
; made, the interrupt's samples short; for tests, a capture of the music's samples
M_RING		.equ	0
M_MASK		.equ	1
M_WR		.equ	2
M_RD		.equ	3
M_CMD		.equ	4
M_ENDED		.equ	5
M_VOL		.equ	6
M_ORD		.equ	7
M_OWR		.equ	8
M_WAIT		.equ	9
M_FRAC		.equ	10
M_LFOAM		.equ	11
M_LFOVIB	.equ	12
M_WSE		.equ	13
M_BLOCKS	.equ	14
M_SHORT		.equ	15
M_CAP		.equ	16		; (for tests) where the music's next sample is copied, as a word; 0 none
M_CAPEND	.equ	17		; and the end of that room
M_ISRS		.equ	18		; the interrupts taken (a count, for tests)
M_PING		.equ	19		; (for tests) the 68000's, copied by the main loop to M_PONG
M_PONG		.equ	20
M_SEEN		.equ	21		; the interrupt count the watchdog last saw, the loop's turns since,
M_QUIET		.equ	22		; and how often it had to set the I2S going again
M_REARMS	.equ	23
M_LAST		.equ	24		; (for tests) the last left sample sent
M_TURNS		.equ	25		; (for tests) the main loop's turns, and the synth's restarts
M_RESTARTS	.equ	26
M_LASTM		.equ	27		; (for tests) the last music sample taken
M_STAGE		.equ	28		; (for tests) the main loop's last stage (1 Loop .. 6 Restart)
M_RET		.equ	29		; (for tests) where the main loop was at the last interrupt, +2
M_BAD		.equ	30		; (for tests) the times R31 was not the stack's top less 4 at
M_BADSP		.equ	31		; the interrupt's end; the first time: R31, bank 1's R31, the place
M_BADRET	.equ	32
M_BADSTAGE	.equ	33

; a channel, longs
C_FNUM		.equ	0
C_BLOCK		.equ	1
C_KEY		.equ	2
C_RC0		.equ	3
C_LIVE		.equ	4
C_FBMUL		.equ	5		; 2 ^ feedback, 0 none
C_MODMASK	.equ	6		; -1: the modulator moves the carrier; 0: it is added

; an operator, longs (the carrier's 16 on)
O_PHASE		.equ	0
O_STEP		.equ	1
O_G		.equ	2		; gain << 4
O_DG		.equ	3
O_ZMASK		.equ	4
O_NMASK		.equ	5
O_ENV		.equ	6		; attenuation << 16
O_STATE		.equ	7		; off, attack, decay, sustain, release
O_R20		.equ	8
O_R40		.equ	9
O_R60		.equ	10
O_R80		.equ	11
O_RE0		.equ	12
O_OUT0		.equ	13
O_OUT1		.equ	14

RATE		.equ	15980
LFO_AM_STEP	.equ	242		; 65536 * 3.7 Hz * 16 / RATE
LFO_VIB_STEP	.equ	400		; 65536 * 6.1 Hz * 16 / RATE

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

; ---- the interrupt (bank 0) ----
; r14 channel  r20 left  r21 right  r22 channels left  r23 Next  r24 Chan  r1-r8 temps
; r9 D_FLAGS  r10 the flags as the main loop had them (read before anything here changes them,
; and put back: else a compare and its branch there, the interrupt between, go wrong)

Isr:		movei	#D_FLAGS,r9
		load	(r9),r10
		movei	#MCTL,r14		; (counted)
		load	(r14+M_ISRS),r1
		addq	#1,r1
		store	r1,(r14+M_ISRS)
		movei	#CHANNELS,r14
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
		move	r1,r3		; the sample: its long read (main memory only as longs,
		shrq	#2,r3			; as etjag's DSP, heard on a console), the half wanted
		shlq	#2,r3
		load	(r3),r3
		btst	#1,r1
		jr	NE,.low
		nop
		jr	T,.got
		sharq	#16,r3			; (the high half)
.low:		shlq	#16,r3
		sharq	#16,r3
.got:		addq	#2,r1
		store	r1,(r14)
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
		; the music's sample, if one is made
		cmpq	#0,r14		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#MCTL,r14
		load	(r14+M_ORD),r1
		cmpq	#0,r2		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r14+M_OWR),r2
		movei	#.out,r8
		cmp	r1,r2
		jr	NE,.music
		nop
		load	(r14+M_SHORT),r3
		addq	#1,r3
		store	r3,(r14+M_SHORT)
		jump	T,(r8)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
.music:		movei	#OBUF,r3
		move	r1,r4
		shlq	#2,r4
		add	r4,r3
		load	(r3),r5
		movei	#MCTL+(M_LASTM*4),r7
		store	r5,(r7)
		load	(r14+M_VOL),r6
		addq	#1,r1
		moveq	#31,r7
		shlq	#1,r7
		addq	#1,r7			; 63
		and	r7,r1
		store	r1,(r14+M_ORD)
		imult	r6,r5
		add	r5,r20
		add	r5,r21
.out:		sharq	#8,r20
		sharq	#8,r21
		sat16s	r20
		sat16s	r21
		movei	#MCTL+(M_LAST*4),r14
		store	r20,(r14)
		movei	#LTXD,r14
		store	r21,(r14)		; right
		store	r20,(r14+1)		; left (RTXD)
		; back: the flags written whole, not as read: the main loop's bank (1), the I2S interrupt
		; on and its latch cleared, IMASK off; only the condition codes (Z C N) as they were
		moveq	#7,r3
		and	r3,r10
		movei	#$4420,r3
		or	r3,r10
		; the place to go back to: at the stack's top always (on a console the interrupt now and
		; then pushes it with bank 1's R31, not bank 0's: so read from the top, not through R31,
		; and both banks' R31 put back to the top; counted, for tests, with both R31s and the
		; place, the first time)
		movei	#$F1CFFC,r4
		movei	#MCTL,r14
		load	(r4),r3
		movei	#.spok,r6
		cmp	r4,r31
		jump	EQ,(r6)
		nop
		load	(r14+M_BAD),r5
		addq	#1,r5
		store	r5,(r14+M_BAD)
		subq	#1,r5
		jump	NE,(r6)			; (the first time only: the rest)
		nop
		store	r31,(r14+M_BADSP)
		cmpq	#0,r5		; (read, not written, first: Atari's GPU/DSP bug 2)
		movefa	r31,r5
		movei	#MCTL+(M_BADRET*4),r7
		store	r5,(r7)
		addq	#4,r7
		store	r3,(r7)			; (M_BADSTAGE: the place)
.spok:		cmpq	#0,r31		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#$F1D000,r31
		moveta	r31,r31
		addq	#2,r3
		cmpq	#0,r3		; (read first: Atari's GPU/DSP bug 1, an indexed store's data)
		store	r3,(r14+M_RET)
		; the flags put back BEFORE the jump, not in its delay slot: there, the instruction
		; interrupted can run in the wrong register bank (Atari's "Lies and Damned Lies", TWI's
		; finding); the flags' store takes effect an instruction late, so the jump still reads
		; this bank's register, and it is atomic with its delay slot (the place in both banks'
		; R30 all the same, the main loop's never used, should the switch come sooner)
		move	r3,r30
		moveta	r30,r30
		store	r10,(r9)
		jump	T,(r30)
		nop

; ---- the main loop (bank 1) ----

Main:		movei	#$F1D000,r31		; the stack (bank 0's): the top of DSP RAM
		moveta	r31,r31			; (and bank 1's: see the interrupt's end)
		movei	#$F1A150,r1		; SCLK 25 and SMODE internal, word strobe, rising: set
		moveq	#25,r0			; here, whole longs (as etjag's, heard on a console; the
		store	r0,(r1)			; 68000's two halves were not)
		movei	#$F1A154,r1
		moveq	#$0D,r0
		store	r0,(r1)
		movei	#D_FLAGS,r1
		movei	#$4020,r0		; bank 1 for this, the I2S interrupt on
		store	r0,(r1)
		nop
		nop
		nop
		nop
Loop:		movei	#MCTL,r14
		moveq	#1,r0			; (for tests: where the main loop got to)
		store	r0,(r14+M_STAGE)
		load	(r14+M_PING),r0		; (a test's: the 68000's ping back)
		cmpq	#0,r0		; (read first: Atari's GPU/DSP bug 1, an indexed store's data)
		store	r0,(r14+M_PONG)
		load	(r14+M_TURNS),r0
		addq	#1,r0
		store	r0,(r14+M_TURNS)
		; the watchdog: no interrupt over 4096 turns of this loop (some 20 ms), the I2S clock, its
		; mode and the interrupt set going again (counted)
		movei	#.watched,r2
		load	(r14+M_ISRS),r0
		load	(r14+M_SEEN),r1
		cmp	r0,r1
		jr	EQ,.quiet
		nop
		store	r0,(r14+M_SEEN)
		moveq	#0,r0
		store	r0,(r14+M_QUIET)
		jump	T,(r2)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
.quiet:		load	(r14+M_QUIET),r1
		addq	#1,r1
		store	r1,(r14+M_QUIET)
		btst	#12,r1
		jump	EQ,(r2)
		nop
		moveq	#0,r1
		store	r1,(r14+M_QUIET)
		load	(r14+M_REARMS),r1
		addq	#1,r1
		store	r1,(r14+M_REARMS)
		movei	#$F1A150,r1
		moveq	#25,r0
		store	r0,(r1)
		movei	#$F1A154,r1
		moveq	#$0D,r0
		store	r0,(r1)
		movei	#D_FLAGS,r1
		movei	#$4020,r0		; (IMASK off, the I2S interrupt on, bank 1)
		store	r0,(r1)
		nop
		nop
.watched:	load	(r14+M_CMD),r0
		cmpq	#0,r0
		movei	#Restart,r1
		jump	NE,(r1)
		nop
		; room in OBUF for a block?
		load	(r14+M_ORD),r1
		load	(r14+M_OWR),r2
		sub	r1,r2
		moveq	#31,r3
		shlq	#1,r3
		addq	#1,r3
		and	r3,r2			; made, not yet played
		movei	#48,r3
		movei	#Loop,r4
		sub	r3,r2
		jump	PL,(r4)
		nop
		movei	#Block,r1
		jump	T,(r1)
		nop

; the synth silenced, the ring's place 0
Restart:
		moveq	#6,r0			; (for tests: where the main loop got to)
		store	r0,(r14+M_STAGE)
		load	(r14+M_RESTARTS),r0
		addq	#1,r0
		store	r0,(r14+M_RESTARTS)
		movei	#CHANS,r1
		cmpq	#0,r2		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#(OBUF-CHANS)/4,r2
		moveq	#0,r0
.zero:		store	r0,(r1)
		subq	#1,r2
		jr	NE,.zero
		addq	#4,r1
		cmpq	#0,r1		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#OPS+(O_ENV*4),r1
		cmpq	#0,r2		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#18,r2
		movei	#511<<16,r0
		movei	#64,r3
.env:		store	r0,(r1)
		subq	#1,r2
		jr	NE,.env
		add	r3,r1
		moveq	#0,r0
		store	r0,(r14+M_RD)
		store	r0,(r14+M_ENDED)
		store	r0,(r14+M_WAIT)
		store	r0,(r14+M_FRAC)
		store	r0,(r14+M_LFOAM)
		store	r0,(r14+M_LFOVIB)
		store	r0,(r14+M_WSE)
		cmpq	#0,r1		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#Loop,r1
		store	r0,(r14+M_CMD)
		jump	T,(r1)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")

; ---- a block: the ticks due, the envelopes, the samples ----
; the ticks: r1 frac  r3 RATE  r10 ring  r11 mask  r12 rd  r13 wr  r5 register  r6 value

Block:
		moveq	#2,r1			; (for tests: where the main loop got to)
		store	r1,(r14+M_STAGE)
		load	(r14+M_FRAC),r1
		cmpq	#0,r2		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#960,r2			; 16 samples * 60
		add	r2,r1
		movei	#RATE,r3
		load	(r14+M_RING),r10
		load	(r14+M_MASK),r11
		load	(r14+M_RD),r12
		load	(r14+M_WR),r13
TickLoop:	move	r1,r4
		sub	r3,r4
		movei	#TicksDone,r0
		jump	MI,(r0)
		nop
		move	r4,r1			; one due
		load	(r14+M_ENDED),r4
		cmpq	#0,r4
		movei	#TickLoop,r0
		jump	NE,(r0)
		nop
		load	(r14+M_WAIT),r4
		cmpq	#0,r4
		jr	EQ,Token
		nop
		subq	#1,r4
		store	r4,(r14+M_WAIT)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
Token:		move	r13,r4			; two bytes there at least?
		sub	r12,r4
		and	r11,r4
		cmpq	#2,r4
		movei	#TickLoop,r0
		jump	CS,(r0)			; (below 2)
		nop
		movei	#$ff,r8
		move	r10,r4
		add	r12,r4
		move	r4,r5		; the byte: its long read, shifted down
		shrq	#2,r5
		shlq	#2,r5
		load	(r5),r5
		moveq	#3,r7
		and	r4,r7
		neg	r7
		addq	#3,r7
		shlq	#3,r7
		sh	r7,r5
		and	r8,r5
		addq	#1,r12
		and	r11,r12
		move	r10,r4
		add	r12,r4
		move	r4,r6		; the byte: its long read, shifted down
		shrq	#2,r6
		shlq	#2,r6
		load	(r6),r6
		moveq	#3,r7
		and	r4,r7
		neg	r7
		addq	#3,r7
		shlq	#3,r7
		sh	r7,r6
		and	r8,r6
		movei	#$ff,r4
		cmp	r4,r5
		jump	EQ,(r0)			; the tick's end
		nop
		subq	#2,r4			; $fd: the song's
		cmp	r4,r5
		jr	NE,.notend
		nop
		moveq	#1,r4
		store	r4,(r14+M_ENDED)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
.notend:	addq	#1,r12
		and	r11,r12
		addq	#1,r4			; $fe n: n ticks with none
		cmp	r4,r5
		jr	NE,Write
		nop
		subq	#1,r6
		store	r6,(r14+M_WAIT)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
TicksDone:
		moveq	#3,r0			; (for tests: where the main loop got to)
		store	r0,(r14+M_STAGE)
		store	r1,(r14+M_FRAC)
		movei	#Envelopes,r0
		store	r12,(r14+M_RD)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")

; a register written: r5 its number, r6 the value (r0 TickLoop's... Token's place again)
Write:		movei	#Token,r0
		cmpq	#1,r5
		jr	NE,.w20
		nop
		moveq	#16,r7
		shlq	#1,r7			; $20
		and	r6,r7
		store	r7,(r14+M_WSE)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
.w20:		move	r5,r7
		shrq	#5,r7			; the register's class
		jump	EQ,(r0)			; (0: none else)
		move	r5,r8
		cmpq	#5,r7
		movei	#.wa,r9
		jump	EQ,(r9)
		cmpq	#6,r7
		movei	#.wc,r9
		jump	EQ,(r9)
		cmpq	#7,r7
		movei	#.we,r9
		jump	EQ,(r9)
		nop
		; 20h-9fh: an operator's 20h, 40h, 60h, 80h
		moveq	#31,r9
		and	r9,r8			; the slot
		shlq	#2,r8
		movei	#SlotOps,r9
		add	r9,r8
		load	(r8),r8			; its operator's offset, or -1
		cmpq	#0,r8
		jump	MI,(r0)
		shlq	#2,r7
		addq	#28,r7			; O_R20 * 4 for 1, on by 4
		add	r7,r8
		movei	#OPS,r9
		add	r9,r8
		jump	T,(r0)
		store	r6,(r8)
.we:		movei	#$e0,r9			; e0h-f5h: the waveform
		sub	r9,r8
		shlq	#2,r8
		movei	#SlotOps,r9
		add	r9,r8
		load	(r8),r8
		cmpq	#0,r8
		jump	MI,(r0)
		nop
		movei	#OPS+(O_RE0*4),r9
		add	r9,r8
		jump	T,(r0)
		store	r6,(r8)
.wc:		movei	#$c0,r9			; c0h-c8h: feedback, connection
		sub	r9,r8
		cmpq	#9,r8
		jump	CC,(r0)			; (9 and on: none)
		shlq	#5,r8
		movei	#CHANS+(C_RC0*4),r9
		add	r9,r8
		jump	T,(r0)
		store	r6,(r8)
.wa:		movei	#$a0,r9			; a0h-a8h: the F-number's low
		sub	r9,r8
		cmpq	#9,r8
		jr	CS,.wfl
		nop
		subq	#16,r8			; b0h-b8h
		cmpq	#9,r8
		jump	CC,(r0)
		nop
		movei	#.wb,r9
		jump	T,(r9)
		nop
.wfl:		shlq	#5,r8
		movei	#CHANS,r9
		add	r9,r8
		load	(r8),r7
		movei	#$300,r9
		and	r9,r7
		or	r6,r7
		jump	T,(r0)
		store	r7,(r8)
		; b0h-b8h: the F-number's high, the block, the key
.wb:		move	r8,r15
		shlq	#7,r15			; the channel's operators
		movei	#OPS,r9
		add	r9,r15
		shlq	#5,r8
		movei	#CHANS,r9
		add	r9,r8
		load	(r8),r7
		movei	#$ff,r9
		and	r9,r7
		moveq	#3,r9
		and	r6,r9
		shlq	#8,r9
		or	r9,r7
		store	r7,(r8)			; C_FNUM
		move	r6,r7
		shrq	#2,r7
		moveq	#7,r9
		and	r9,r7
		addq	#4,r8
		store	r7,(r8)			; C_BLOCK
		addq	#4,r8
		move	r6,r7
		shrq	#5,r7
		moveq	#1,r9
		and	r9,r7			; on
		load	(r8),r9			; C_KEY
		store	r7,(r8)
		cmp	r9,r7
		jump	EQ,(r0)
		cmpq	#0,r7
		jr	EQ,.off
		nop
		moveq	#1,r7			; on: the attack, the phase restarted
		moveq	#0,r9
		store	r7,(r15+O_STATE)
		store	r9,(r15+O_PHASE)
		store	r7,(r15+16+O_STATE)
		store	r9,(r15+16+O_PHASE)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")
.off:		moveq	#4,r7			; off: the sounding ones released
		load	(r15+O_STATE),r9
		cmpq	#0,r9
		jr	EQ,.off2
		nop
		store	r7,(r15+O_STATE)
.off2:		load	(r15+16+O_STATE),r9
		cmpq	#0,r9
		jump	EQ,(r0)
		nop
		store	r7,(r15+16+O_STATE)
		jump	T,(r0)
		nop			; (not an indexed store after a jump: Atari's "Lies and Damned Lies")

; ---- the envelopes a block on, each operator's step, wave and gain ----
; r14 channel  r15 operator  r16 keycode  r17 F-number  r18 block  r19 operators left
; r20 channels left  r21 the KSL's base  r22 tremolo's attenuation  r23 vibrato's triangle
; r24 waveform select  r25-r29 temps  r0-r13 temps

Envelopes:	load	(r14+M_LFOAM),r0	; tremolo: a triangle 0-5 (1 dB)
		shrq	#8,r0
		movei	#255,r1
		and	r1,r0
		btst	#7,r0
		jr	EQ,.am
		nop
		sub	r0,r1
		move	r1,r0
.am:		move	r0,r22
		moveq	#5,r1
		mult	r1,r22
		shrq	#7,r22
		load	(r14+M_LFOVIB),r0	; vibrato: a triangle -64 to 64
		shrq	#8,r0
		movei	#255,r1
		and	r1,r0
		movei	#64,r1
		movei	#.vib,r2
		cmp	r1,r0
		jump	CS,(r2)			; below 64: itself
		move	r0,r23
		movei	#192,r1
		cmp	r1,r0
		jr	CC,.vibhi
		nop
		cmpq	#0,r23		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#128,r23		; 128 - t
		jr	T,.vib
		sub	r0,r23
.vibhi:		movei	#256,r1			; t - 256
		sub	r1,r23
.vib:		load	(r14+M_WSE),r24
		movei	#CHANS,r14
		movei	#OPS,r15
		moveq	#9,r20
EnvChan:	load	(r14+C_FNUM),r17
		load	(r14+C_BLOCK),r18
		move	r17,r16
		shrq	#9,r16
		moveq	#1,r0
		and	r0,r16
		move	r18,r0
		shlq	#1,r0
		or	r0,r16			; keycode
		move	r17,r0			; the KSL's base: ksl_base[fnum >> 6] - 16 * (7 - block)
		shrq	#6,r0
		shlq	#2,r0
		movei	#KslBase,r1
		add	r1,r0
		load	(r0),r21
		moveq	#7,r0
		sub	r18,r0
		shlq	#4,r0
		sub	r0,r21
		moveq	#2,r19
EnvOp:		load	(r15+O_R20),r2
		load	(r15+O_STATE),r0
		load	(r15+O_ENV),r1
		move	r16,r3			; the rate's offset
		btst	#4,r2
		jr	NE,.ksr
		nop
		shrq	#2,r3
.ksr:		cmpq	#1,r0
		movei	#.attack,r9
		jump	EQ,(r9)
		cmpq	#2,r0
		movei	#.decay,r9
		jump	EQ,(r9)
		cmpq	#3,r0
		movei	#.sustain,r9
		jump	EQ,(r9)
		cmpq	#4,r0
		movei	#.release,r9
		jump	EQ,(r9)
		nop
		movei	#EnvGain,r9
		jump	T,(r9)
		nop
.attack:	load	(r15+O_R60),r4
		shrq	#4,r4			; R
		movei	#EnvGain,r9
		jump	EQ,(r9)
		shlq	#2,r4
		add	r3,r4			; 4R + rof
		movei	#60,r5
		cmp	r5,r4
		jr	CS,.keep
		nop
		jr	T,.attacked
		moveq	#0,r1
.keep:		shlq	#2,r4
		movei	#ATTACK,r5
		add	r5,r4
		load	(r4),r4			; keep
		move	r1,r5
		shrq	#16,r5
		mult	r4,r5			; (env >> 16) * keep
		shlq	#16,r1
		shrq	#16,r1
		mult	r4,r1
		shrq	#16,r1
		add	r5,r1
.attacked:	move	r1,r4
		shrq	#16,r4
		jump	NE,(r9)			; (EnvGain)
		nop
		moveq	#0,r1
		moveq	#2,r0			; decay
		jump	T,(r9)
		nop
.decay:		load	(r15+O_R80),r5		; the sustain level: (sl == 15 ? 31 : sl) << 20
		shrq	#4,r5
		cmpq	#15,r5
		jr	NE,.sl
		nop
		moveq	#31,r5
.sl:		shlq	#20,r5
		cmpq	#0,r4		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r15+O_R60),r4
		movei	#.decayed,r7
		moveq	#15,r6
		and	r6,r4			; R
		jump	EQ,(r7)
		shlq	#2,r4
		add	r3,r4
		movei	#63,r6
		cmp	r6,r4
		jr	CS,.dr
		nop
		move	r6,r4
.dr:		shlq	#2,r4
		movei	#DECAY,r6
		add	r6,r4
		load	(r4),r4
		add	r4,r1
.decayed:	movei	#EnvGain,r9
		cmp	r5,r1
		jump	CS,(r9)			; below the level
		nop
		move	r5,r1
		cmpq	#0,r0		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#3,r0			; sustain
		jump	T,(r9)
		nop
.sustain:	btst	#5,r2
		movei	#EnvGain,r9
		jump	NE,(r9)			; held
		nop
.release:	load	(r15+O_R80),r4
		movei	#.released,r7
		moveq	#15,r6
		and	r6,r4
		jump	EQ,(r7)
		shlq	#2,r4
		add	r3,r4
		movei	#63,r6
		cmp	r6,r4
		jr	CS,.rr
		nop
		move	r6,r4
.rr:		shlq	#2,r4
		movei	#DECAY,r6
		add	r6,r4
		load	(r4),r4
		add	r4,r1
.released:	movei	#511<<16,r5
		movei	#EnvGain,r9
		cmp	r5,r1
		jump	CS,(r9)
		nop
		move	r5,r1
		cmpq	#4,r0
		jump	NE,(r9)
		nop
		moveq	#0,r0			; off
		; the gain it glides to: r0 state  r1 env  r2 20h
EnvGain:	store	r0,(r15+O_STATE)
		store	r1,(r15+O_ENV)
		cmpq	#0,r4		; (read, not written, first: Atari's GPU/DSP bug 2, its delay slot's write)
		moveq	#0,r4			; to
		cmpq	#0,r0
		movei	#.glide,r9
		jump	EQ,(r9)
		nop
		shrq	#16,r1			; att = env >> 16 + total level * 4
		load	(r15+O_R40),r5
		moveq	#31,r6
		shlq	#1,r6
		addq	#1,r6			; 63
		and	r5,r6
		shlq	#2,r6
		add	r6,r1
		shrq	#6,r5			; key scaling
		jr	EQ,.trem
		move	r21,r6
		cmpq	#0,r6
		jr	MI,.trem
		nop
		jr	EQ,.trem
		cmpq	#1,r5
		jr	EQ,.ksl
		cmpq	#2,r5
		jr	NE,.ksl6
		nop
		jr	T,.ksl
		shrq	#1,r6
.ksl6:		shlq	#1,r6
.ksl:		add	r6,r1
.trem:		btst	#7,r2
		jr	EQ,.att
		nop
		add	r22,r1
.att:		movei	#512,r6
		cmp	r6,r1
		jr	CC,.glide		; 512 and more: none
		move	r1,r6
		moveq	#31,r7
		and	r7,r6
		shlq	#2,r6
		movei	#GAIN32,r7
		add	r7,r6
		cmpq	#0,r4		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r6),r4
		shrq	#5,r1
		sh	r1,r4
		shlq	#4,r4
.glide:		load	(r15+O_G),r5		; dg = (to - g) / 16, toward 0; g = to - dg * 16
		cmpq	#0,r6		; (read, not written, first: Atari's GPU/DSP bug 2, its delay slot's write)
		move	r4,r6
		sub	r5,r6
		jr	PL,.up
		nop
		neg	r6
		shrq	#4,r6
		jr	T,.dg
		neg	r6
.up:		shrq	#4,r6
.dg:		store	r6,(r15+O_DG)
		shlq	#4,r6
		sub	r6,r4
		store	r4,(r15+O_G)
		; its step: fnum * mult_x2 * 6372 (with vibrato) << block
		moveq	#15,r4
		and	r2,r4
		shlq	#2,r4
		movei	#MultX2,r5
		add	r5,r4
		load	(r4),r4
		mult	r17,r4
		movei	#6372,r5
		mult	r5,r4
		btst	#6,r2
		jr	EQ,.step
		move	r4,r5
		shrq	#13,r5
		imult	r23,r5
		sharq	#1,r5
		add	r5,r4
.step:		cmpq	#0,r5		; (read, not written, first: Atari's GPU/DSP bug 2, its delay slot's write)
		move	r18,r5
		neg	r5
		sh	r5,r4
		store	r4,(r15+O_STEP)
		; its wave's masks
		moveq	#0,r4
		cmpq	#0,r24
		jr	EQ,.wave
		nop
		cmpq	#0,r4		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r15+O_RE0),r4
		moveq	#3,r5
		and	r5,r4
.wave:		shlq	#3,r4
		movei	#WaveMasks,r5
		add	r5,r4
		load	(r4),r5
		addq	#4,r4
		load	(r4),r6
		cmpq	#0,r5		; (read first: Atari's GPU/DSP bug 1, an indexed store's data)
		cmpq	#0,r6		; (read first: Atari's GPU/DSP bug 1, an indexed store's data)
		store	r5,(r15+O_ZMASK)
		store	r6,(r15+O_NMASK)
		; the next operator
		movei	#64,r4
		add	r4,r15
		subq	#1,r19
		movei	#EnvOp,r9
		jump	NE,(r9)
		nop
		; the channel: live, feedback, connection
		movei	#-128,r4
		add	r4,r15
		load	(r15+O_STATE),r4
		load	(r15+16+O_STATE),r5
		or	r5,r4
		jr	EQ,.dead
		moveq	#0,r5
		cmpq	#0,r5		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r5
.dead:		store	r5,(r14+C_LIVE)
		cmpq	#0,r4		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r14+C_RC0),r4
		moveq	#1,r5
		and	r4,r5
		subq	#1,r5			; 0 (added) or -1 (moving)
		store	r5,(r14+C_MODMASK)
		shrq	#1,r4
		moveq	#7,r5
		and	r5,r4
		jr	EQ,.fb
		moveq	#0,r5
		cmpq	#0,r5		; (read, not written, first: Atari's GPU/DSP bug 2)
		moveq	#1,r5
		neg	r4
		sh	r4,r5			; 1 << feedback
.fb:		store	r5,(r14+C_FBMUL)
		movei	#128,r4
		add	r4,r15
		addq	#32,r14
		subq	#1,r20
		movei	#EnvChan,r9
		jump	NE,(r9)
		nop
		; the LFOs on
		cmpq	#0,r14		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#MCTL,r14
		load	(r14+M_LFOAM),r0
		movei	#LFO_AM_STEP,r1
		add	r1,r0
		store	r0,(r14+M_LFOAM)
		load	(r14+M_LFOVIB),r0
		movei	#LFO_VIB_STEP,r1
		add	r1,r0
		store	r0,(r14+M_LFOVIB)
		moveq	#4,r1			; (for tests: where the main loop got to)
		store	r1,(r14+M_STAGE)

; ---- the block's samples into OBUF: a channel at a time, its 16 ----
; the channel's: r1-r4 the modulator's phase, step, gain, gain step; r5-r8 the carrier's; r9 r10
; the modulator's last outputs; r11 2^feedback; r12 r13 the modulator's masks (silence, turn
; over); r16 r17 the carrier's; r18 -1 (it moves the carrier) or 0 (added); r19 MIX's place;
; r20 HSINE; r21 511; r22 samples left; r14 channel; r15 operators; r0 Inner; r23-r29 temps (r30 r31 the interrupt's, in both banks)

		movei	#MIX,r1			; MIX emptied
		moveq	#16,r2
		moveq	#0,r0
.clear:		store	r0,(r1)
		subq	#1,r2
		jr	NE,.clear
		addq	#4,r1
		cmpq	#0,r20		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#HSINE,r20
		movei	#511,r21
		movei	#CHANS,r14
		cmpq	#0,r15		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#OPS,r15
		movei	#Inner,r0
OneChan:	load	(r14+C_LIVE),r23
		cmpq	#0,r23
		movei	#ChanDone,r24
		jump	EQ,(r24)
		nop
		cmpq	#0,r1		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r15+O_PHASE),r1
		cmpq	#0,r2		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r15+O_STEP),r2
		load	(r15+O_G),r3
		load	(r15+O_DG),r4
		load	(r15+16+O_PHASE),r5
		load	(r15+16+O_STEP),r6
		load	(r15+16+O_G),r7
		load	(r15+16+O_DG),r8
		load	(r15+O_OUT0),r9
		load	(r15+O_OUT1),r10
		load	(r14+C_FBMUL),r11
		load	(r15+O_ZMASK),r12
		load	(r15+O_NMASK),r13
		load	(r15+16+O_ZMASK),r16
		load	(r15+16+O_NMASK),r17
		load	(r14+C_MODMASK),r18
		cmpq	#0,r19		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#MIX,r19
		moveq	#16,r22
Inner:		add	r2,r1			; the modulator: phase, gain on
		add	r4,r3
		move	r9,r23			; its feedback: (out0 + out1) * 2^fb >> 9
		add	r10,r23
		imult	r11,r23
		move	r1,r24
		shrq	#22,r24
		sharq	#9,r23
		add	r23,r24			; its place in the wave
		add	r6,r5			; the carrier: phase, gain on
		add	r8,r7
		move	r24,r25			; the wave: half a sine at i & 511, turned over (N), or
		and	r21,r25			; silenced (Z)
		shlq	#2,r25
		add	r20,r25
		load	(r25),r25
		move	r24,r26
		and	r13,r26
		move	r24,r27
		and	r12,r27
		shlq	#22,r26
		subq	#1,r27
		sharq	#31,r26			; N: -1 turned over
		sharq	#31,r27			; Z: 0 silenced
		move	r3,r28
		sharq	#5,r28
		xor	r26,r25
		sub	r26,r25
		and	r27,r25
		imult	r28,r25
		move	r9,r10
		sharq	#15,r25			; its output
		move	r25,r9
		move	r25,r26			; moving the carrier, or added
		and	r18,r26
		move	r5,r24
		shrq	#22,r24
		move	r25,r29
		add	r26,r24			; the carrier's place in its wave
		sub	r26,r29
		move	r24,r25
		and	r21,r25
		shlq	#2,r25
		add	r20,r25
		load	(r25),r25
		move	r24,r26
		and	r17,r26
		move	r24,r27
		and	r16,r27
		shlq	#22,r26
		subq	#1,r27
		sharq	#31,r26
		sharq	#31,r27
		move	r7,r28
		sharq	#5,r28
		xor	r26,r25
		sub	r26,r25
		and	r27,r25
		imult	r28,r25
		load	(r19),r26
		sharq	#15,r25
		add	r29,r25
		add	r26,r25
		store	r25,(r19)
		subq	#1,r22
		jump	NE,(r0)
		addq	#4,r19
		store	r1,(r15+O_PHASE)	; the channel's back
		store	r3,(r15+O_G)
		store	r5,(r15+16+O_PHASE)
		store	r7,(r15+16+O_G)
		store	r9,(r15+O_OUT0)
		store	r10,(r15+O_OUT1)
ChanDone:	addq	#32,r14
		movei	#128,r23
		add	r23,r15
		movei	#CHANS+(9*32),r23	; (the channels' end: r31, as r30, the interrupt's on a console)
		cmp	r23,r14
		movei	#OneChan,r24
		jump	NE,(r24)
		nop
		; the samples, saturated, into OBUF
		cmpq	#0,r14		; (read, not written, first: Atari's GPU/DSP bug 2)
		movei	#MCTL,r14
		cmpq	#0,r19		; (read, not written, first: Atari's GPU/DSP bug 2)
		load	(r14+M_OWR),r19
		movei	#MIX,r1
		movei	#OBUF,r2
		moveq	#16,r3
		moveq	#31,r4
		shlq	#1,r4
		addq	#1,r4			; 63
		movei	#.out,r9
.out:		load	(r1),r5
		addq	#4,r1
		sat16s	r5
		move	r19,r6
		shlq	#2,r6
		add	r2,r6
		store	r5,(r6)
		load	(r14+M_CAP),r7		; (a test's capture)
		cmpq	#0,r7
		jr	EQ,.nocap
		nop
		load	(r14+M_CAPEND),r8
		cmp	r8,r7
		jr	CC,.nocap
		nop
		storew	r5,(r7)
		addq	#2,r7
		store	r7,(r14+M_CAP)
.nocap:		addq	#1,r19
		subq	#1,r3
		jump	NE,(r9)
		and	r4,r19
		; the block made
		store	r19,(r14+M_OWR)
		load	(r14+M_BLOCKS),r0
		addq	#1,r0
		store	r0,(r14+M_BLOCKS)
		moveq	#5,r0			; (for tests: where the main loop got to)
		store	r0,(r14+M_STAGE)
		movei	#Loop,r0
		jump	T,(r0)
		nop

; ---- constants ----
		.long
SlotOps:	; each slot's operator (its offset in OPS), or -1
		dc.l	0*64, 2*64, 4*64, 1*64, 3*64, 5*64, -1, -1
		dc.l	6*64, 8*64, 10*64, 7*64, 9*64, 11*64, -1, -1
		dc.l	12*64, 14*64, 16*64, 13*64, 15*64, 17*64, -1, -1
		dc.l	-1, -1, -1, -1, -1, -1, -1, -1
MultX2:		dc.l	1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
KslBase:	dc.l	0, 48, 64, 74, 80, 86, 90, 94, 96, 100, 102, 104, 106, 108, 110, 112
WaveMasks:	; by waveform: the phase bits that silence it, that turn it over
		dc.l	0, 512, 512, 512, 0, 0, 256, 0
dsp_code_end:

		.68000
dsp_sound_end:

		.if	dsp_code_end > HSINE
		.error	"the DSP's code runs into its tables"
		.endif
