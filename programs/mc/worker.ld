/* The workers' code, at MC_ARENA_PHYS + MC_CODE_AT (mc.h); mc_worker first. No data of its own. */
ENTRY(mc_worker)

SECTIONS
{
    . = 0x86C01000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) *(.srodata*) }
    .data : { *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame*) }
}
