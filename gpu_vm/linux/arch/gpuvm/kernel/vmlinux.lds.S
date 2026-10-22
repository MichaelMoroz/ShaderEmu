/* SPDX-License-Identifier: GPL-2.0 */
/* The image from GVM_RAM: head.S's _start is its first byte */
#define RO_EXCEPTION_TABLE_ALIGN	16

#include <asm-generic/vmlinux.lds.h>
#include <asm/cache.h>
#include <asm/page.h>
#include <asm/thread_info.h>

ENTRY(_start)

jiffies = jiffies_64;

SECTIONS
{
	. = PAGE_OFFSET;
	_text = .;
	HEAD_TEXT_SECTION
	.text : {
		_stext = .;
		TEXT_TEXT
		SCHED_TEXT
		CPUIDLE_TEXT
		LOCK_TEXT
		KPROBES_TEXT
		ENTRY_TEXT
		IRQENTRY_TEXT
		SOFTIRQENTRY_TEXT
		*(.fixup)
		_etext = .;
	}

	. = ALIGN(PAGE_SIZE);
	__init_begin = .;
	INIT_TEXT_SECTION(PAGE_SIZE)
	INIT_DATA_SECTION(16)
	PERCPU_SECTION(L1_CACHE_BYTES)
	. = ALIGN(PAGE_SIZE);
	__init_end = .;

	_sdata = .;
	RO_DATA(PAGE_SIZE)
	RW_DATA(L1_CACHE_BYTES, PAGE_SIZE, THREAD_SIZE)
	_edata = .;

	BSS_SECTION(PAGE_SIZE, PAGE_SIZE, 16)
	. = ALIGN(PAGE_SIZE);
	_end = .;

	STABS_DEBUG
	DWARF_DEBUG
	ELF_DETAILS

	DISCARDS
}
