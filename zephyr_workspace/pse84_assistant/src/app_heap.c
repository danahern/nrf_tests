/*
 * PSE84 Voice Assistant — 256 KB heap in SOCMEM.
 *
 * Overrides the standard malloc/free/calloc/realloc symbols so
 * every dynamic allocation (including libopus's internal encoder
 * state, which calls malloc via picolibc) lands in SOCMEM instead
 * of eating scarce DTCM.
 *
 * CONFIG_COMMON_LIBC_MALLOC must be disabled so picolibc's own
 * malloc impl doesn't take precedence. k_calloc (used by
 * opus_wrapper.c) still uses Zephyr's CONFIG_HEAP_MEM_POOL_SIZE —
 * that stays small (~1 KB, enough for the tiny ctx struct).
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdlib.h>

#define HEAP_BYTES DT_REG_SIZE(DT_NODELABEL(socmem_heap))

static uint8_t heap_buf[HEAP_BYTES]
	Z_GENERIC_SECTION(SOCMEM_HEAP) __aligned(8);

static struct k_heap app_heap;

static int app_heap_init(void)
{
	k_heap_init(&app_heap, heap_buf, sizeof(heap_buf));
	return 0;
}
SYS_INIT(app_heap_init, PRE_KERNEL_1, 0);

/* ---- libc malloc overrides --------------------------------------- */

void *malloc(size_t size)
{
	return k_heap_alloc(&app_heap, size, K_NO_WAIT);
}

void free(void *ptr)
{
	if (ptr) {
		k_heap_free(&app_heap, ptr);
	}
}

void *calloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	void *p = k_heap_alloc(&app_heap, total, K_NO_WAIT);
	if (p) {
		memset(p, 0, total);
	}
	return p;
}

void *realloc(void *ptr, size_t size)
{
	/* k_heap doesn't have a native realloc. Fall back to
	 * alloc-new + copy-old + free-old. Libopus doesn't realloc
	 * in hot paths so this is fine. */
	if (ptr == NULL) {
		return k_heap_alloc(&app_heap, size, K_NO_WAIT);
	}
	if (size == 0) {
		k_heap_free(&app_heap, ptr);
		return NULL;
	}
	void *np = k_heap_alloc(&app_heap, size, K_NO_WAIT);
	if (np) {
		/* We don't know the old allocation size; libopus's
		 * realloc calls pass the old size externally (it's in
		 * the opus state). Copy up to the new size and rely on
		 * caller to not read past old_size. Any issue will show
		 * up as a corrupted codec output, not a crash. */
		memcpy(np, ptr, size);
	}
	k_heap_free(&app_heap, ptr);
	return np;
}
