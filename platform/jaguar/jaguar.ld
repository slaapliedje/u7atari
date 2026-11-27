/* The Jaguar cartridge: code and constants run in place from ROM at $802000, after the 8 KB
 * boot header at $800000 (tools/jag-rom.py adds it). .data is stored after them and copied to
 * RAM by startup.S; RAM below $4000 is left to the boot ROM. The heap (_sbrk, syscalls.c) runs
 * from the end of .bss to the stack, which takes the top __stack_size bytes of the 2 MB (256 K). */
ENTRY(_start)

MEMORY
{
	rom (rx)  : ORIGIN = 0x802000, LENGTH = 0x5FE000
	ram (rwx) : ORIGIN = 0x004000, LENGTH = 0x1FC000
}

__stack = 0x200000;
/* as the Atari's _stksize: the game has big locals (a program may set its own: --defsym) */
__stack_size = DEFINED(__stack_size) ? __stack_size : 0x40000;

SECTIONS
{
	.text : {
		KEEP(*(.text.start))
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
		__init_array_start = .;
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
		KEEP(*(.init_array))
		KEEP(*(.ctors))
		__init_array_end = .;
		*(.eh_frame*) *(.gcc_except_table*)
		. = ALIGN(4);
	} > rom

	.data : {
		__data_start = .;
		*(.data .data.*)
		. = ALIGN(4);
		__data_end = .;
	} > ram AT > rom
	__data_load = LOADADDR(.data);

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.bss .bss.*) *(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	} > ram
	__heap_start = .;
	__heap_end = __stack - __stack_size;

	/DISCARD/ : { *(.fini_array*) *(.dtors) *(.comment) }
}
