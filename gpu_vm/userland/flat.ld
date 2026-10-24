/* A gpu_vm program for elf2flt (gpu_vm/userland/README.md): one image from address 0, laid out as the bFLT file
   will have it after its 64-byte header (file offset = address + 0x40). Text and everything read-only first, then
   from __gvm_data_start (64-aligned, so that the file's data starts on binfmt_flat's 32 bytes) the writable data and
   the bss. Nothing at address 0: binfmt_flat leaves a relocated word of 0 alone. */
ENTRY(_start)

SECTIONS
{
	. = 0x10;
	PROVIDE(__executable_start = .);
	.text : {
		*(.text.unlikely .text.unlikely.*)
		*(.text.startup .text.startup.*)
		*(.text .text.*)
	}
	/* (GNU ld's names for the text's end, which programs read: linux/libmedia/m_core.c) */
	PROVIDE(etext = .);
	PROVIDE(_etext = .);
	PROVIDE(__etext = .);
	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
		*(.gcc_except_table .gcc_except_table.*)
	}
	/* C++'s unwind tables (libunwind built bare-metal finds them by these symbols; -Wl,--eh-frame-hdr adds the
	   index): a program of C has none (clang makes no unwind tables for C here), so nothing is there */
	.eh_frame : {
		__eh_frame_start = .;
		KEEP(*(.eh_frame))
		__eh_frame_end = .;
	}
	.eh_frame_hdr : { KEEP(*(.eh_frame_hdr)) }
	__eh_frame_hdr_start = SIZEOF(.eh_frame_hdr) > 0 ? ADDR(.eh_frame_hdr) : 0;
	__eh_frame_hdr_end = SIZEOF(.eh_frame_hdr) > 0 ? . : 0;
	.tdata : { *(.tdata .tdata.*) }
	.tbss : { *(.tbss .tbss.*) }

	/* binfmt_flat (5.17, shared-library support even with BINFMT_FLAT_NO_DATA_START_OFFSET) stores the data's start in
	   the word just below it: a word of nothing there, or it lands on the last of .rodata */
	. += 4;
	. = ALIGN(64);
	__gvm_data_start = .;
	PROVIDE(__DATA_BEGIN__ = .);	/* (GNU ld's RISC-V name for the data's start: linux/apps/ui_bulk.h) */
	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
	}
	.init_array : {
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		PROVIDE_HIDDEN(__init_array_end = .);
	}
	.fini_array : {
		PROVIDE_HIDDEN(__fini_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		KEEP(*(.fini_array .dtors))
		PROVIDE_HIDDEN(__fini_array_end = .);
	}
	. = ALIGN(16);
	__gvm_data_end = .;
	PROVIDE(edata = .);
	PROVIDE(_edata = .);
	.bss : {
		__bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
	}
	. = ALIGN(16);
	_end = .;
	end = .;

	/DISCARD/ : { *(.note.GNU-stack) *(.comment) }
}
