#include "misc.h"

#include <asm/msr.h>
#include <asm/archrandom.h>
#include <asm/e820.h>

#include <generated/compile.h>
#include <linux/module.h>
#include <linux/uts.h>
#include <linux/utsname.h>
#include <generated/utsrelease.h>

/* Simplified build-specific string for starting entropy. */
static const char build_str[] = UTS_RELEASE " (" LINUX_COMPILE_BY "@"
		LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION;

#define I8254_PORT_CONTROL	0x43
#define I8254_PORT_COUNTER0	0x40
#define I8254_CMD_READBACK	0xC0
#define I8254_SELECT_COUNTER0	0x02
#define I8254_STATUS_NOTREADY	0x40
static inline u16 i8254(void)
{
	u16 status, timer;

	do {
		outb(I8254_PORT_CONTROL,
		     I8254_CMD_READBACK | I8254_SELECT_COUNTER0);
		status = inb(I8254_PORT_COUNTER0);
		timer  = inb(I8254_PORT_COUNTER0);
		timer |= inb(I8254_PORT_COUNTER0) << 8;
	} while (status & I8254_STATUS_NOTREADY);

	return timer;
}

static unsigned long rotate_xor(unsigned long hash, const void *area,
				size_t size)
{
	size_t i;
	unsigned long *ptr = (unsigned long *)area;

	for (i = 0; i < size / sizeof(hash); i++) {
		/* Rotate by odd number of bits and XOR. */
		hash = (hash << ((sizeof(hash) * 8) - 7)) | (hash >> 7);
		hash ^= ptr[i];
	}

	return hash;
}

/* Attempt to create a simple but unpredictable starting entropy. */
static unsigned long get_random_boot(void)
{
	unsigned long hash = 0;

	hash = rotate_xor(hash, build_str, sizeof(build_str));
	hash = rotate_xor(hash, real_mode, sizeof(*real_mode));

	return hash;
}

static unsigned long get_random_long(void)
{
#ifdef CONFIG_X86_64
	const unsigned long mix_const = 0x5d6008cbf3848dd3UL;
#else
	const unsigned long mix_const = 0x3f39e593UL;
#endif
	unsigned long raw, random = get_random_boot();
	bool use_i8254 = true;

	debug_putstr("KASLR using");

	if (has_cpuflag(X86_FEATURE_RDRAND)) {
		debug_putstr(" RDRAND");
		if (rdrand_long(&raw)) {
			random ^= raw;
			use_i8254 = false;
		}
	}

	if (has_cpuflag(X86_FEATURE_TSC)) {
		debug_putstr(" RDTSC");
		raw = rdtsc();

		random ^= raw;
		use_i8254 = false;
	}

	if (use_i8254) {
		debug_putstr(" i8254");
		random ^= i8254();
	}

	/* Circular multiply for better bit diffusion */
	asm("mul %3"
	    : "=a" (random), "=d" (raw)
	    : "a" (random), "rm" (mix_const));
	random += raw;

	debug_putstr("...\n");

	return random;
}

struct mem_vector {
	unsigned long start;
	unsigned long size;
};

#define MEM_AVOID_MAX 4
static struct mem_vector mem_avoid[MEM_AVOID_MAX];

static bool mem_overlaps(struct mem_vector *one, struct mem_vector *two)
{
	/* Item one is entirely before item two. */
	if (one->start + one->size <= two->start)
		return false;
	/* Item one is entirely after item two. */
	if (one->start >= two->start + two->size)
		return false;
	return true;
}

static void mem_avoid_init(unsigned long input, unsigned long input_size,
			   unsigned long output)
{
	unsigned long init_size = real_mode->hdr.init_size;
	u64 initrd_start, initrd_size;
	u64 cmd_line, cmd_line_size;
	char *ptr;

	/*
	 * Avoid the region that is unsafe to overlap during
	 * decompression.
	 * As we already move ZO (arch/x86/boot/compressed/vmlinux)
	 * to the end of buffer, [input+input_size, output+init_size)
	 * has [_text, _end) for ZO.
	 */
	mem_avoid[0].start = input;
	mem_avoid[0].size = (output + init_size) - input;
	fill_pagetable(input, (output + init_size) - input);

	/* Avoid initrd. */
	initrd_start  = (u64)real_mode->ext_ramdisk_image << 32;
	initrd_start |= real_mode->hdr.ramdisk_image;
	initrd_size  = (u64)real_mode->ext_ramdisk_size << 32;
	initrd_size |= real_mode->hdr.ramdisk_size;
	mem_avoid[1].start = initrd_start;
	mem_avoid[1].size = initrd_size;
	/* don't need to set mapping for initrd */

	/* Avoid kernel command line. */
	cmd_line  = (u64)real_mode->ext_cmd_line_ptr << 32;
	cmd_line |= real_mode->hdr.cmd_line_ptr;
	/* Calculate size of cmd_line. */
	ptr = (char *)(unsigned long)cmd_line;
	for (cmd_line_size = 0; ptr[cmd_line_size++]; )
		;
	mem_avoid[2].start = cmd_line;
	mem_avoid[2].size = cmd_line_size;
	fill_pagetable(cmd_line, cmd_line_size);

	/* Avoid params */
	mem_avoid[3].start = (unsigned long)real_mode;
	mem_avoid[3].size = sizeof(*real_mode);
	fill_pagetable((unsigned long)real_mode, sizeof(*real_mode));

	/* don't need to set mapping for setup_data */

#ifdef CONFIG_X86_VERBOSE_BOOTUP
	/* for video ram */
	fill_pagetable(0, PMD_SIZE);
#endif
}

/* Does this memory vector overlap a known avoided area? */
static bool mem_avoid_overlap(struct mem_vector *img)
{
	int i;
	struct setup_data *ptr;

	for (i = 0; i < MEM_AVOID_MAX; i++) {
		if (mem_overlaps(img, &mem_avoid[i]))
			return true;
	}

	/* Avoid all entries in the setup_data linked list. */
	ptr = (struct setup_data *)(unsigned long)real_mode->hdr.setup_data;
	while (ptr) {
		struct mem_vector avoid;

		avoid.start = (unsigned long)ptr;
		avoid.size = sizeof(*ptr) + ptr->len;

		if (mem_overlaps(img, &avoid))
			return true;

		ptr = (struct setup_data *)(unsigned long)ptr->next;
	}

	return false;
}

static unsigned long
mem_min_overlap(struct mem_vector *img, struct mem_vector *out)
{
	int i;
	struct setup_data *ptr;
	unsigned long min = img->start + img->size;

	for (i = 0; i < MEM_AVOID_MAX; i++) {
		if (mem_overlaps(img, &mem_avoid[i]) &&
			(mem_avoid[i].start < min)) {
			*out = mem_avoid[i];
			min = mem_avoid[i].start;
		}
	}

	/* Check all entries in the setup_data linked list. */
	ptr = (struct setup_data *)(unsigned long)real_mode->hdr.setup_data;
	while (ptr) {
		struct mem_vector avoid;

		avoid.start = (unsigned long)ptr;
		avoid.size = sizeof(*ptr) + ptr->len;

		if (mem_overlaps(img, &avoid) && (avoid.start < min)) {
			*out = avoid;
			min = avoid.start;
		}

		ptr = (struct setup_data *)(unsigned long)ptr->next;
	}

	return min;
}

struct slot_area {
	unsigned long addr;
	int num;
};

#define MAX_SLOT_AREA 100

static struct slot_area slot_areas[MAX_SLOT_AREA];

static unsigned long slot_max;

static unsigned long slot_area_index;

static void store_slot_info(struct mem_vector *region, unsigned long image_size)
{
	struct slot_area slot_area;

	slot_area.addr = region->start;
	if (image_size <= CONFIG_PHYSICAL_ALIGN)
		slot_area.num = region->size / CONFIG_PHYSICAL_ALIGN;
	else
		slot_area.num = (region->size - image_size) /
				CONFIG_PHYSICAL_ALIGN + 1;

	if (slot_area.num > 0) {
		slot_areas[slot_area_index++] = slot_area;
		slot_max += slot_area.num;
	}
}

static unsigned long slots_fetch_random(void)
{
	unsigned long random;
	int i;

	/* Handle case of no slots stored. */
	if (slot_max == 0)
		return 0;

	random = get_random_long() % slot_max;

	for (i = 0; i < slot_area_index; i++) {
		if (random >= slot_areas[i].num) {
			random -= slot_areas[i].num;
			continue;
		}
		return slot_areas[i].addr + random * CONFIG_PHYSICAL_ALIGN;
	}

	if (i == slot_area_index)
		debug_putstr("Something wrong happened in slots_fetch_random()...\n");
	return 0;
}

static void process_e820_entry(struct e820entry *entry,
			       unsigned long minimum,
			       unsigned long image_size)
{
	struct mem_vector region, out;
	struct slot_area slot_area;
	unsigned long min, start_orig;

	/* Skip non-RAM entries. */
	if (entry->type != E820_RAM)
		return;

	/* Ignore entries entirely below our minimum. */
	if (entry->addr + entry->size < minimum)
		return;

	region.start = entry->addr;
	region.size = entry->size;

repeat:
	start_orig = region.start;

	/* Potentially raise address to minimum location. */
	if (region.start < minimum)
		region.start = minimum;

	/* Return if slot area array is full */
	if (slot_area_index == MAX_SLOT_AREA)
		return;

	/* Potentially raise address to meet alignment requirements. */
	region.start = ALIGN(region.start, CONFIG_PHYSICAL_ALIGN);

	/* Did we raise the address above the bounds of this e820 region? */
	if (region.start > entry->addr + entry->size)
		return;

	/* Reduce size by any delta from the original address. */
	region.size -= region.start - start_orig;

	/* Return if region can't contain decompressed kernel */
	if (region.size < image_size)
		return;

	if (!mem_avoid_overlap(&region)) {
		store_slot_info(&region, image_size);
		return;
	}

	min = mem_min_overlap(&region, &out);

	if (min > region.start + image_size) {
		struct mem_vector tmp;

		tmp.start = region.start;
		tmp.size = min - region.start;
		store_slot_info(&tmp, image_size);
	}

	region.size -= out.start - region.start + out.size;
	region.start = out.start + out.size;
	goto repeat;
}

static unsigned long find_random_phy_addr(unsigned long minimum,
				      unsigned long size)
{
	int i;
	unsigned long addr;

	/* Make sure minimum is aligned. */
	minimum = ALIGN(minimum, CONFIG_PHYSICAL_ALIGN);

	/* Verify potential e820 positions, appending to slots list. */
	for (i = 0; i < real_mode->e820_entries; i++) {
		process_e820_entry(&real_mode->e820_map[i], minimum, size);
		if (slot_area_index == MAX_SLOT_AREA) {
			debug_putstr("Stop processing e820 since slot_areas is full...\n");
			break;
		}
	}

	return slots_fetch_random();
}

static unsigned long find_random_virt_offset(unsigned long minimum,
				  unsigned long image_size)
{
	unsigned long slot_num, random;

	/* Make sure minimum is aligned. */
	minimum = ALIGN(minimum, CONFIG_PHYSICAL_ALIGN);

	if (image_size <= CONFIG_PHYSICAL_ALIGN)
		slot_num = (CONFIG_RANDOMIZE_BASE_MAX_OFFSET - minimum) /
				CONFIG_PHYSICAL_ALIGN;
	else
		slot_num = (CONFIG_RANDOMIZE_BASE_MAX_OFFSET -
				minimum - image_size) /
				CONFIG_PHYSICAL_ALIGN + 1;

	random = get_random_long() % slot_num;

	return random * CONFIG_PHYSICAL_ALIGN + minimum;
}

void choose_kernel_location(unsigned char *input,
				unsigned long input_size,
				unsigned char **output,
				unsigned long output_size,
				unsigned char **virt_offset)
{
	unsigned long random, min_addr;

	*virt_offset = (unsigned char *)LOAD_PHYSICAL_ADDR;

#ifdef CONFIG_HIBERNATION
	if (!cmdline_find_option_bool("kaslr")) {
		debug_putstr("KASLR disabled by default...\n");
		return;
	}
#else
	if (cmdline_find_option_bool("nokaslr")) {
		debug_putstr("KASLR disabled by cmdline...\n");
		return;
	}
#endif

	real_mode->hdr.loadflags |= KASLR_FLAG;

	/* Record the various known unsafe memory ranges. */
	mem_avoid_init((unsigned long)input, input_size,
		       (unsigned long)*output);

	/* start from 512M */
	min_addr = (unsigned long)*output;
	if (min_addr > (512UL<<20))
		min_addr = 512UL<<20;

	/* Walk e820 and find a random address. */
	random = find_random_phy_addr(min_addr, output_size);
	if (!random)
		debug_putstr("KASLR could not find suitable E820 region...\n");
	else {
		if ((unsigned long)*output != random) {
			fill_pagetable(random, output_size);
			switch_pagetable();
			*output = (unsigned char *)random;
		}
	}

	/*
	 * Get a random address between LOAD_PHYSICAL_ADDR and
	 * CONFIG_RANDOMIZE_BASE_MAX_OFFSET
	 */
	random = find_random_virt_offset(LOAD_PHYSICAL_ADDR, output_size);
	*virt_offset = (unsigned char *)random;
}
