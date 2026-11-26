; The engine test's terrain on Tom's GPU (engine/jaguar/main.c, eb_tiles): chunks of 16 by 16
; cells, each cell an 8 by 8 flat tile copied a row (a phrase) at a time into the static layer.
; Lives at $F03400, beside platform/jaguar/spans_gpu.s at $F03000. Assembled by rmac.
;
; Parameter block (GPU RAM, $F03E00, below spans_gpu.s's at $F03F80; longs):
;   +0 flats: per type below 150, its pixels (0: none) and its frames, two longs
;   +1 the layer's pitch  +2 chunks  +3 a tile of black (64 bytes)
;   +4 on, per chunk: its cell words (little-endian: type in bits 0-9, frame 10-14), the layer at
;      its first cell, cells across and rows down that fall in the layer
; A cell of type 150 or more, or a type not loaded, or a frame past its last, is black.
; Every tile and the layer are phrase aligned.
;
; r0 flats  r1 pitch  r2 chunks left  r3 the next chunk's record  r4 cells of the row
; r5 layer at the row  r6 across  r7 rows left  r8 cell  r9 layer at the tile  r10 across left
; r11-r13 temps  r15 tile pixels  r17 layer row  r18 phrase  r24 TileLoop  r25 Black
; r27 black tile  r28 $3FF  r29 150  r30 31

G_CTRL		.equ	$F02114
TILE_PARAMS	.equ	$F03E00

		.globl	gpu_tiles
		.globl	gpu_tiles_end

		.text
		.long
gpu_tiles:
		.gpu
		.org	$F03400

		movei	#TILE_PARAMS,r14
		load	(r14),r0
		load	(r14+1),r1
		load	(r14+2),r2
		load	(r14+3),r27
		movei	#TILE_PARAMS+16,r3
		movei	#$3FF,r28
		movei	#150,r29
		moveq	#31,r30
		movei	#TileLoop,r24
		movei	#Black,r25

ChunkLoop:
		load	(r3),r4
		addq	#4,r3
		load	(r3),r5
		addq	#4,r3
		load	(r3),r6
		addq	#4,r3
		load	(r3),r7
		addq	#4,r3
RowLoop:
		move	r4,r8
		move	r5,r9
		move	r6,r10
TileLoop:
		loadb	(r8),r11
		addq	#1,r8
		loadb	(r8),r12
		addq	#1,r8
		shlq	#8,r12
		or	r11,r12			; the cell's word
		move	r12,r13
		and	r28,r13			; type
		shrq	#10,r12
		and	r30,r12			; frame
		cmp	r29,r13			; type - 150
		jump	PL,(r25)
		nop
		shlq	#3,r13
		add	r0,r13
		load	(r13),r15		; the flat's pixels
		addq	#4,r13
		load	(r13),r11		; its frames
		cmpq	#0,r15
		jump	EQ,(r25)
		nop
		cmp	r11,r12			; frame - frames
		jump	PL,(r25)
		nop
		shlq	#6,r12
		jr	T,Copy
		add	r12,r15			; the frame's pixels
Black:
		move	r27,r15
Copy:
		move	r9,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		addq	#8,r15
		storep	r18,(r17)
		add	r1,r17
		loadp	(r15),r18
		storep	r18,(r17)
		addq	#8,r9			; the next tile
		subq	#1,r10
		jump	NE,(r24)
		nop
		addq	#32,r4			; the next row of cells
		move	r1,r11
		shlq	#3,r11
		add	r11,r5			; and of tiles
		subq	#1,r7
		movei	#RowLoop,r12
		jump	NE,(r12)
		nop
		subq	#1,r2
		movei	#ChunkLoop,r12
		jump	NE,(r12)
		nop

		load	(r27),r0		; after the last store has gone out
		or	r0,r0
		moveq	#0,r0			; stop
		movei	#G_CTRL,r1
		store	r0,(r1)
		nop
		nop

		.68000
		.long
gpu_tiles_end:
