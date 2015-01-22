/*
 * traps.c - exception handling for the kernel decompressor
 *
 * Copyright (c) 2014 Andy Lutomirski
 * GPLv2
 */

#include "misc.h"

#ifdef USE_BOOT_IDT

#include <asm/processor.h>
#include <asm/desc.h>
#include <asm/traps.h>
#include <asm/uaccess.h>

static void error_puthex(unsigned long val)
{
	int i;

	char buf[2 * sizeof(val) + 1];

	for (i = 0; i < 2 * sizeof(val); i++) {
		int digit = val >> (8 * sizeof(val) - 4);
		buf[i] = digit < 10 ? digit + '0' : digit - 10 + 'a';
		val <<= 4;
	}

	buf[2 * sizeof(val)] = '\0';
	error_putstr(buf);
}

static gate_desc idt[X86_TRAP_PF + 1];	/* Keep the idt as small as possible. */

extern struct exception_table_entry __start___ex_table[];
extern struct exception_table_entry __stop___ex_table[];
extern asmlinkage __visible void do_boot_general_protection(
	unsigned long *ip_ptr);

asmlinkage __visible void do_boot_general_protection(unsigned long *ip_ptr)
{
	struct exception_table_entry *e;

	for (e = __start___ex_table; e != __stop___ex_table; e++) {
		if (e->insn + (unsigned long)&e->insn == *ip_ptr) {
			*ip_ptr = e->fixup + (unsigned long)&e->fixup;
			return;
		}
	}

	error("GPF\n");
}

extern asmlinkage void boot_general_protection(void);  /* #GP asm wrapper */

static asmlinkage void do_boot_page_fault(void)
{
	/* Not recoverable, so no asm wrapper needed. */
	unsigned long addr;
	unsigned long i;
	pte_t *pte = (pte_t*)ident_pgt_ptr;
        pgdval_t pgd, *pgd_p;
        pudval_t pud, *pud_p;
        pmdval_t pmd, *pmd_p;
	pmdval_t early_pmd_flags = __PAGE_KERNEL_LARGE & ~(_PAGE_GLOBAL | _PAGE_NX);

	asm ("mov %%cr2,%0" : "=rm" (addr));
	error_putstr("\n\nPage fault accessing 0x");
	error_puthex(addr);
	//error_putstr("\n\n -- System halted");

	pgd_p = &pte[pgd_index(addr)].pte;
	pgd = *pgd_p;

	if (pgd)
		pud_p = (pudval_t *)(pgd & PTE_PFN_MASK);
	else {
		if (next_ident_pgt >= 4) {
			error_putstr("\n\n -- System halted");
			while(1)
				asm("hlt");
                }

                pud_p = (pudval_t *) (pte+(next_ident_pgt+6)*PTRS_PER_PTE);
		next_ident_pgt++;
                for (i = 0; i < PTRS_PER_PUD; i++)
                        pud_p[i] = 0;
                *pgd_p = (pgdval_t)pud_p + 7;
	}
	pud_p += pud_index(addr);
        pud = *pud_p;

        if (pud)
                pmd_p = (pmdval_t *)(pud & PTE_PFN_MASK);
        else {
                if (next_ident_pgt >= 4) {
			error_putstr("\n\n -- System halted");
			while(1)
				asm("hlt");
                }

                pmd_p = (pmdval_t *) (pte+(next_ident_pgt+6)*PTRS_PER_PTE);
		next_ident_pgt++;
                for (i = 0; i < PTRS_PER_PMD; i++)
                        pmd_p[i] = 0;
                *pud_p = (pudval_t)pmd_p + 7;
        }
        pmd = (addr & PMD_MASK) + early_pmd_flags;
        pmd_p[pmd_index(addr)] = pmd;
}

void setup_idt(void)
{
	u16 cs;
	gate_desc d;
	struct desc_ptr gp_idt_descr = {
		.size = sizeof(idt) - 1,
		.address = (unsigned long)idt,
	};

	/*
	 * We don't know our GDT layout, so just reuse whatever code
	 * segment we're currently using.
	 */
	asm ("mov %%cs,%0" : "=rm" (cs));

	/* This is necessary for rdmsr_safe, and it's useful for debugging. */
	pack_gate(&d, GATE_INTERRUPT, (unsigned long)boot_general_protection,
		  0, 0, cs);
	BUILD_BUG_ON(X86_TRAP_GP >= ARRAY_SIZE(idt));
	idt[X86_TRAP_GP] = d;

	/* This is purely for debugging, but it can be very helpful. */
	pack_gate(&d, GATE_INTERRUPT, (unsigned long)do_boot_page_fault,
		  0, 0, cs);
	BUILD_BUG_ON(X86_TRAP_PF >= ARRAY_SIZE(idt));
	idt[X86_TRAP_PF] = d;

	asm volatile ("lidt %0" : : "m" (gp_idt_descr));
}

#endif /* USE_BOOT_IDT */
