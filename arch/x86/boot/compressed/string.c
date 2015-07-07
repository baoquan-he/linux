#include "../string.c"

#ifdef CONFIG_X86_32
void *__memcpy(void *dest, const void *src, size_t n)
{
	int d0, d1, d2;
	asm volatile(
		"rep ; movsl\n\t"
		"movl %4,%%ecx\n\t"
		"rep ; movsb\n\t"
		: "=&c" (d0), "=&D" (d1), "=&S" (d2)
		: "0" (n >> 2), "g" (n & 3), "1" (dest), "2" (src)
		: "memory");

	return dest;
}
#else
void *__memcpy(void *dest, const void *src, size_t n)
{
	long d0, d1, d2;
	asm volatile(
		"rep ; movsq\n\t"
		"movq %4,%%rcx\n\t"
		"rep ; movsb\n\t"
		: "=&c" (d0), "=&D" (d1), "=&S" (d2)
		: "0" (n >> 3), "g" (n & 7), "1" (dest), "2" (src)
		: "memory");

	return dest;
}
#endif

extern void error(char *x);
void *memcpy(void *dest, const void *src, size_t n)
{
	unsigned long start_dest, end_dest;
	unsigned long start_src, end_src;
	unsigned long max_start, min_end;

	if (dest < src)
		return __memcpy(dest, src, n);

	start_dest = (unsigned long)dest;
	end_dest = (unsigned long)dest + n;
	start_src = (unsigned long)src;
	end_src = (unsigned long)src + n;
	max_start = (start_dest > start_src) ?  start_dest : start_src;
	min_end = (end_dest < end_src) ? end_dest : end_src;

	if (max_start >= min_end)
		return __memcpy(dest, src, n);

	error("memcpy does not support overlapping with dest > src!\n");

	return dest;
}

void *memset(void *s, int c, size_t n)
{
	int i;
	char *ss = s;

	for (i = 0; i < n; i++)
		ss[i] = c;
	return s;
}
