#define __pa(x)  ((unsigned long)(x))
#define __va(x)  ((void *)((unsigned long)(x)))

#include "misc.h"

#include <asm/init.h>
#include <asm/pgtable.h>

#include "../../mm/ident_map.c"
#include "../string.h"

struct alloc_pgt_data {
	unsigned char *pgt_buf;
	unsigned long pgt_buf_size;
	unsigned long pgt_buf_offset;
};

static void *alloc_pgt_page(void *context)
{
	struct alloc_pgt_data *d = (struct alloc_pgt_data *)context;
	unsigned char *p = (unsigned char *)d->pgt_buf;

	if (d->pgt_buf_offset >= d->pgt_buf_size) {
		debug_putstr("out of pgt_buf in misc.c\n");
		debug_putaddr(d->pgt_buf_offset);
		debug_putaddr(d->pgt_buf_size);
		return NULL;
	}

	p += d->pgt_buf_offset;
	d->pgt_buf_offset += PAGE_SIZE;

	return p;
}

/*
 * Use a normal definition of memset() from string.c. There are already
 * included header files which expect a definition of memset() and by
 * the time we define memset macro, it is too late.
 */
#undef memset

unsigned long __force_order;
static struct alloc_pgt_data pgt_data;
static struct x86_mapping_info mapping_info;
static pgd_t *level4p;

void fill_pagetable(unsigned long start, unsigned long size)
{
	unsigned long end = start + size;

	if (!level4p) {
		pgt_data.pgt_buf_offset = 0;
		mapping_info.alloc_pgt_page = alloc_pgt_page;
		mapping_info.context = &pgt_data;
		mapping_info.pmd_flag = __PAGE_KERNEL_LARGE_EXEC;

		/*
		 * come from startup_32 ?
		 * then cr3 is _pgtable, we can reuse it.
		 */
		level4p = (pgd_t *)read_cr3();
		if ((unsigned long)level4p == (unsigned long)_pgtable) {
			pgt_data.pgt_buf = (unsigned char *)_pgtable +
						 BOOT_INIT_PGT_SIZE;
			pgt_data.pgt_buf_size = BOOT_PGT_SIZE -
						 BOOT_INIT_PGT_SIZE;
			memset((unsigned char *)pgt_data.pgt_buf, 0,
				pgt_data.pgt_buf_size);
			debug_putstr("boot via startup_32\n");
		} else {
			pgt_data.pgt_buf = (unsigned char *)_pgtable;
			pgt_data.pgt_buf_size = BOOT_PGT_SIZE;
			memset((unsigned char *)pgt_data.pgt_buf, 0,
				pgt_data.pgt_buf_size);
			debug_putstr("boot via startup_64\n");
			level4p = (pgd_t *)alloc_pgt_page(&pgt_data);
		}
	}

	/* align boundary to 2M */
	start = round_down(start, PMD_SIZE);
	end = round_up(end, PMD_SIZE);
	if (start >= end)
		return;

	kernel_ident_mapping_init(&mapping_info, level4p, start, end);
}

void switch_pagetable(void)
{
	write_cr3((unsigned long)level4p);
}
