/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GPUVM_PAGE_H
#define _ASM_GPUVM_PAGE_H

#include <linux/const.h>
#include <asm/gvm.h>

#define PAGE_SHIFT	12
#define PAGE_SIZE	(_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE - 1))

/* No MMU: an address is physical. RAM, and so the kernel's image, starts at GVM_RAM. */
#define PAGE_OFFSET	GVM_RAM
#define PHYS_OFFSET	GVM_RAM
#define ARCH_PFN_OFFSET	(PAGE_OFFSET >> PAGE_SHIFT)

#ifndef __ASSEMBLY__

#include <asm/string.h>

/* memset and memcpy of a page are the commit's operations (asm/string.h) */
#define clear_page(page)	memset((page), 0, PAGE_SIZE)
#define copy_page(to, from)	memcpy((to), (from), PAGE_SIZE)

#define clear_user_page(page, vaddr, pg)	clear_page(page)
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;
typedef struct page *pgtable_t;

#define pte_val(x)	((x).pte)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x)	((pte_t) { (x) })
#define __pgd(x)	((pgd_t) { (x) })
#define __pgprot(x)	((pgprot_t) { (x) })

extern unsigned long memory_start;
extern unsigned long memory_end;

#define __va(x)		((void *)((unsigned long)(x)))
#define __pa(x)		((unsigned long)(x))

#define virt_to_pfn(kaddr)	(__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn)	__va((pfn) << PAGE_SHIFT)

#define virt_to_page(addr)	pfn_to_page(virt_to_pfn(addr))
#define page_to_virt(page)	pfn_to_virt(page_to_pfn(page))

#define pfn_valid(pfn)		((pfn) >= ARCH_PFN_OFFSET && ((pfn) - ARCH_PFN_OFFSET) < max_mapnr)

#define page_to_phys(page)	((dma_addr_t)page_to_pfn(page) << PAGE_SHIFT)

#define virt_addr_valid(kaddr)	((unsigned long)(kaddr) >= memory_start && \
				 (unsigned long)(kaddr) < memory_end)

#endif /* __ASSEMBLY__ */

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#endif /* _ASM_GPUVM_PAGE_H */
