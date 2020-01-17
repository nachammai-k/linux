#include <linux/list.h>
struct ramtrace_platform_data {
	unsigned long	mem_size;
	phys_addr_t	mem_address;
};
struct ramtrace_freelist {
	struct list_head list;
	struct page* page;
};

struct page* ramtrace_alloc_page(int cpu);

void ramtrace_free_page(void *address, int cpu);

void ramtrace_dump(void);
