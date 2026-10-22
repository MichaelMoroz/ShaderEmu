/* A gpu_vm program for elf2flt (gpu_vm/userland/README.md): one image from address 0, laid out as the bFLT file
   will have it after its 64-byte header (file offset = address + 0x40). Text and everything read-only first, then
   from __gvm_data_start (64-aligned, so that the file's data starts on binfmt_flat's 32 bytes) the writable data and
   the bss. Nothing at address 0: binfmt_flat leaves a relocated word of 0 alone. */
ENTRY(_start)

SECTIONS
{
	. = 0x10;
	.text : {
		*(.text.unlikely .text.unlikely.*)
		*(.text.startup .text.startup.*)
		*(.text .text.*)
	}
	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
	}
	.tdata : { *(.tdata .tdata.*) }
	.tbss : { *(.tbss .tbss.*) }

	. = ALIGN(64);
	__gvm_data_start = .;
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
	.bss : {
		__bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
	}
	. = ALIGN(16);
	_end = .;
	end = .;

	/DISCARD/ : { *(.note.GNU-stack) *(.comment) *(.eh_frame) *(.eh_frame_hdr) }
}
