/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_PGTABLE_H
#define _ASM_GPUVM_PGTABLE_H

#include <asm-generic/pgtable-nopmd.h>
#include <asm/page.h>

/* No MMU: these mean nothing */
#define PAGE_NONE		__pgprot(0)
#define PAGE_SHARED		__pgprot(0)
#define PAGE_COPY		__pgprot(0)
#define PAGE_READONLY		__pgprot(0)
#define PAGE_KERNEL		__pgprot(0)
#define PAGE_KERNEL_EXEC	__pgprot(0)

#define pgprot_noncached(prot)		(prot)
#define pgprot_writecombine(prot)	(prot)

#define swapper_pg_dir		((pgd_t *)0)

#define __swp_type(x)		(0)
#define __swp_offset(x)		(0)
#define __swp_entry(typ, off)	((swp_entry_t) { ((typ) | ((off) << 7)) })
#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

#define kern_addr_valid(addr)	(1)

extern void paging_init(void);

#define ZERO_PAGE(vaddr)	(virt_to_page(empty_zero_page))
extern unsigned long empty_zero_page[];

#define TASK_SIZE		0xffffffffUL
#define VMALLOC_START		0
#define VMALLOC_END		0xffffffffUL

#endif /* _ASM_GPUVM_PGTABLE_H */
