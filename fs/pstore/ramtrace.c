#include <linux/ring_buffer.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/pstore.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <linux/of.h>
#include <linux/ramtrace.h>
#include <generated/utsrelease.h>
static unsigned long long mem_address;
module_param_hw(mem_address, ullong, other, 0400);
MODULE_PARM_DESC(mem_address,
		"start of reserved RAM used to store trace data");

static ulong mem_size;
module_param(mem_size, ulong, 0400);
MODULE_PARM_DESC(mem_size,
		"size of reserved RAM used to store trace data");

static unsigned int on;
module_param(on, uint, 0600);
MODULE_PARM_DESC(mem_type,
		"set to 1 to indicate trace to use persistent memory");

struct ramtrace_context {
	phys_addr_t phys_addr;
	unsigned long size;
	void *vaddr;
	spinlock_t lock;
	struct ramtrace_freelist *freelist;
	struct page *metadata;
	struct page **bitmap_pages;
	struct tr_persistent_info *persist_info;
	void *base_address;
	int num_bitmap_per_cpu, cpu;
	int allocated;
};

static struct ramtrace_context trace_ctx = {
	.size = 0,
};

static struct platform_device *dummy;

bool is_ramtrace_available(void)
{
	return (trace_ctx.size) ? 1 : 0;
}


void ramtrace_dump_bitmap(unsigned long long *bitmap_page)
{
	int j;
	int count = 0;
	unsigned long long *time_stamp = (unsigned long long *)(trace_ctx.base_address + (354 * PAGE_SIZE));
	for(j = 0; j < PAGE_SIZE/sizeof(long long); j=j+4)
		if (bitmap_page[j] || bitmap_page[j + 1] || bitmap_page[j+2] || bitmap_page[j+3])
			printk(KERN_ERR "ramtrace %px : %llx %llx %llx %llx\n", bitmap_page + j, bitmap_page[j], bitmap_page[j+1], bitmap_page[j+2], bitmap_page[j+3]);

	for(j = 0; j < PAGE_SIZE/sizeof(long long); j++)
	{
		unsigned long long k = bitmap_page[j];
		while(k)
		{
			count += k & 1;
			k = k >> 1;
		}
	}
	printk(KERN_ERR "pages allocated %d \n", count);
	printk(KERN_ERR "first timestamp %llu \n", *time_stamp); 
}

void ramtrace_dump(void)
{
	int i;
	printk(KERN_ERR "base_address %px\n", trace_ctx.base_address);
	for (i = 0; i < trace_ctx.cpu * trace_ctx.num_bitmap_per_cpu ; i++)
	{
	
		printk(KERN_ERR "Dumping bitmap  %d\n", i);

		ramtrace_dump_bitmap((unsigned long long *)page_address(trace_ctx.bitmap_pages[i]));

	}
}


static void ramtrace_init_bitmap(unsigned int npages)
{
	int i;
	unsigned long flags;
	struct ramtrace_freelist *freelist = trace_ctx.freelist;
	trace_ctx.bitmap_pages = kmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);

	spin_lock_irqsave(&trace_ctx.lock, flags);
        for(i = 0; i < npages; i++)
	{
		struct ramtrace_freelist *freelist_node = list_next_entry(freelist, list);
		void *page = page_address(freelist_node->page);
		memset(page, 0, PAGE_SIZE);
		trace_ctx.bitmap_pages[i] = freelist_node->page;
		list_del(&freelist_node->list);
		kfree(freelist_node);
	}
	spin_unlock_irqrestore(&trace_ctx.lock, flags);

	trace_ctx.base_address = page_address(trace_ctx.bitmap_pages[npages - 1]) + PAGE_SIZE;

}


static void ramtrace_write_int(int **buffer, int n)
{
	**buffer = n;
	(*buffer)++;
}

int init_ramtrace_pages(int cpu, unsigned long npages, const char *tracer, int trace_clock)
{
	const char kernel_version[] = UTS_RELEASE;
	struct ramtrace_freelist *freelist_node;
	void *metapage;
	unsigned long flags;
	int n_bitmap = 0;
	int ramtrace_pages;

	ramtrace_pages = (trace_ctx.size >> PAGE_SHIFT) - 1;
	printk(KERN_ALERT "ramtrace %d npages %lu\n", ramtrace_pages, npages);
	if (ramtrace_pages < npages)
		return 0;
	while ((ramtrace_pages - cpu) >= 0)
	{
		ramtrace_pages -= ((PAGE_SIZE << 3) + cpu);
		n_bitmap++;
	}
	printk(KERN_ALERT "n_bitmap : %d\n", n_bitmap);
	spin_lock_irqsave(&trace_ctx.lock, flags);
	freelist_node = list_next_entry(trace_ctx.freelist, list);
	metapage = page_address(freelist_node->page);
	list_del(&freelist_node->list);
        spin_unlock_irqrestore(&trace_ctx.lock, flags);

	trace_ctx.metadata = metapage;
	ramtrace_write_int((int **)&metapage, cpu);
	ramtrace_write_int((int **)&metapage, trace_clock);
	ramtrace_write_int((int **)&metapage, n_bitmap);
	sprintf(metapage, "%s", kernel_version);
	metapage += strlen(kernel_version) +1;
	sprintf(metapage, "%s", tracer);

	kfree(freelist_node);
	trace_ctx.cpu = cpu;
	trace_ctx.num_bitmap_per_cpu = n_bitmap;
	trace_ctx.allocated=0;
	ramtrace_init_bitmap(cpu * n_bitmap);
	return 1;
}

static void ramtrace_set_bit(char *bitmap, int index)
{
	bitmap[index >> 3] |= (1 << index % 8); 
}

static bool ramtrace_is_allocated(char *bitmap, int index)
{
	return bitmap[index >> 3] & (1 << index % 8);
}

static void ramtrace_reset_bit(char *bitmap, int index)
{
	bitmap[index >> 3] &= ~(1 << index % 8);
}


struct page* ramtrace_alloc_page(int cpu)
{
	struct page *page = NULL;
	struct ramtrace_freelist *freelist = trace_ctx.freelist;
	
	if (!list_empty(&freelist->list))
	{
		struct ramtrace_freelist *freelist_node;
		char *bitmap_page;
		void *address;
		unsigned long page_num;
		unsigned long flags;
		int index, bitmap_page_index;
		
                spin_lock_irqsave(&trace_ctx.lock, flags);
		freelist_node = list_next_entry(freelist, list);
		list_del(&freelist_node->list);
		spin_unlock_irqrestore(&trace_ctx.lock, flags);

		page = freelist_node->page;
		address = page_address(page);
		memset(address, 0, PAGE_SIZE);
                page_num = (address - trace_ctx.base_address) >> PAGE_SHIFT;
		bitmap_page_index = page_num >> (PAGE_SHIFT + 3);
		bitmap_page = (char *)page_address(trace_ctx.bitmap_pages[trace_ctx.num_bitmap_per_cpu * cpu + bitmap_page_index]);
	        index = page_num - (bitmap_page_index << (PAGE_SHIFT + 3));		
		ramtrace_set_bit(bitmap_page, index);
		trace_ctx.allocated++;

	}
	else
		printk(KERN_ERR "empty page %d \n", trace_ctx.allocated);
	return page;

}

void ramtrace_free_page(void *page_address, int cpu)
{
	struct page *page = virt_to_page(page_address);
	void *metapage;
       int index;	


        unsigned long page_num = (page_address - trace_ctx.base_address) >> PAGE_SHIFT;
	int bitmap_page_index = page_num >> (PAGE_SHIFT + 3);
	if (page_address == NULL)
		return;
	metapage = (char *)page_address(trace_ctx.bitmap_pages[trace_ctx.num_bitmap_per_cpu * cpu + bitmap_page_index]);
	index = page_num - (bitmap_page_index << (PAGE_SHIFT + 3));		
//printk(KERN_ERR "free page cpu %d index %d page_num %d page_address %px base address %px", cpu, index, page_num, page_address, trace_ctx.base_address);
	if (ramtrace_is_allocated(metapage, index))
	{
	        struct ramtrace_freelist *freelist_node =  	
			kmalloc(sizeof(struct ramtrace_freelist), GFP_KERNEL);
		unsigned long flags;
		freelist_node->page = page;
		spin_lock_irqsave(&trace_ctx.lock, flags);
		list_add_tail(&freelist_node->list,&(trace_ctx.freelist->list));
		spin_unlock_irqrestore(&trace_ctx.lock, flags);

		ramtrace_reset_bit(metapage, index);
		trace_ctx.allocated--;
	}

}


static int ramtrace_parse_dt(struct platform_device *pdev,
			    struct ramtrace_platform_data *pdata)
{
	struct resource *res;
	

	dev_dbg(&pdev->dev, "using Device Tree\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev,
			"failed to locate DT /reserved-memory resource\n");
		return -EINVAL;
	}

	pdata->mem_size = resource_size(res);
	pdata->mem_address = res->start;

	return 0;
}

static int ramtrace_read_int(int **buffer)
{
	int data = **buffer;
	(*buffer)++;
	return data;
}

static char* ramtrace_read_string(char **buffer)
{
	int len = strlen(*buffer) + 1;
	if (len > 1)
	{
		char *s = kmalloc(len, GFP_KERNEL);
		strncpy(s, *buffer, len);
		*buffer = (*buffer) + len;
		return s;
	}
	return NULL;

}

static struct list_head* ramtrace_read_bitmap_per_cpu(int n_bitmap, unsigned long long *bitmap, void *first_page)
{
	int j, k;
	struct list_head *pages = kmalloc(sizeof(struct list_head), GFP_KERNEL);
	INIT_LIST_HEAD(pages);
	
	for(k = 0; k <  n_bitmap; k++)
	{
	bitmap = (void *)(bitmap) + PAGE_SIZE * k;		
	for(j = 0; j < PAGE_SIZE/sizeof(long long); j++)
	{
		struct ramtrace_page_list *list_page;
		unsigned long long k = bitmap[j];
		int count = 0;
		while (k)
		{
			if (k & 1)
			{
				list_page = kzalloc(sizeof(struct ramtrace_page_list), GFP_KERNEL);
				list_page->page = (first_page + (j * sizeof(long long) * 8 + count) * PAGE_SIZE);
				list_add_tail(&list_page->list, pages);
			}
			count++;
			k = k >> 1;
		}

	}
	}
	return pages;
}

static struct list_head* ramtrace_read_bitmap(int n_cpu, int n_bitmap)
{
	int i;
        void *base_address = (trace_ctx.vaddr + PAGE_SIZE) + (n_cpu * n_bitmap * PAGE_SIZE);
	struct list_head *per_cpu_list;	
	for (i = 0; i < n_cpu; i++)
	{
		per_cpu_list = ramtrace_read_bitmap_per_cpu(n_bitmap, trace_ctx.vaddr + PAGE_SIZE + i * n_bitmap * PAGE_SIZE, base_address);
		if (per_cpu_list)
			ring_buffer_order_pages(per_cpu_list);

	}
	return per_cpu_list;

}

struct tr_persistent_info* tr_info_from_persistent(void)
{
	return trace_ctx.persist_info;
}	


static void print_persist(void)
{
	if (trace_ctx.persist_info)
	{
		struct tr_persistent_info *tr = trace_ctx.persist_info;
		struct ramtrace_page_list *data_page;
		struct list_head *pages = tr->data_pages;

		pr_info("tracer %s cpu %d trace_clock %d \n", tr->tracer_name, tr->nr_cpus, tr->trace_clock);


		list_for_each_entry(data_page, pages, list)
		{
			u64 *ts = (u64 *)(data_page->page);
			printk(KERN_INFO "timestamp: %llu\n", *ts);
		}

	}

}

static void ramtrace_read_pages(void)
{
	void *metapage = trace_ctx.vaddr;


	int n_cpu = ramtrace_read_int((int **)&metapage);
	int trace_clock = ramtrace_read_int((int **)&metapage);
	int n_bitmap = ramtrace_read_int((int **)&metapage);
	char *kernel_version = ramtrace_read_string((char **)&metapage);
	char *tracer = ramtrace_read_string((char **)&metapage);
	struct list_head *ordered_pages;
	struct tr_persistent_info *persist = NULL;

	if (kernel_version && tracer)
	{
		pr_info("kernel_version %s tracer %s %d %d \n", kernel_version, tracer, n_cpu, n_bitmap);
	
		ordered_pages = ramtrace_read_bitmap(n_cpu, n_bitmap);
		persist = kmalloc(sizeof(struct tr_persistent_info), GFP_KERNEL);
	
		persist->tracer_name = tracer;
		persist->trace_clock = trace_clock;
		persist->nr_cpus = n_cpu;	
		persist->data_pages = ordered_pages;
	}
	trace_ctx.persist_info = persist;
	print_persist();
}

static int ramtrace_init_prz(struct ramtrace_context *ctx)
{

	struct page **pages;
	unsigned int page_count;
	pgprot_t prot;
	unsigned int i;
	struct ramtrace_freelist *freelist;

	page_count = DIV_ROUND_UP(ctx->size, PAGE_SIZE);

	prot = pgprot_noncached(PAGE_KERNEL);

	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages) {
		pr_err("%s: Failed to allocate array for %u pages\n",
		       __func__, page_count);
		return 0;
	}
        freelist = kzalloc(sizeof(struct ramtrace_freelist), GFP_KERNEL); 
	INIT_LIST_HEAD(&freelist->list);
	trace_ctx.freelist = freelist;
	for (i = 0; i < page_count; i++) {
		struct ramtrace_freelist *freelist_node = kmalloc(sizeof(struct ramtrace_freelist), GFP_KERNEL);
		phys_addr_t addr = ctx->phys_addr + i * PAGE_SIZE;
		pages[i] = pfn_to_page(addr >> PAGE_SHIFT);
		freelist_node->page = pages[i];
		list_add_tail(&freelist_node->list,&freelist->list);
	}
        spin_lock_init(&ctx->lock); 
	ctx->vaddr = vmap(pages, page_count, VM_MAP, prot);
	ramtrace_read_pages();
	kfree(pages);
	return 1;
}

static int ramtrace_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ramtrace_platform_data *pdata = dev->platform_data;
	struct ramtrace_platform_data pdata_local;
	struct ramtrace_context *cxt = &trace_ctx;
	int err = -EINVAL;

	/*
	 * Only a single ramtrace area allowed at a time, so fail extra
	 * probes.
	 */
	if (cxt->size) {
		pr_err("already initialized\n");
		goto fail_out;
	}

	if (dev_of_node(dev) && !pdata) {
		pdata = &pdata_local;
		memset(pdata, 0, sizeof(*pdata));

		err = ramtrace_parse_dt(pdev, pdata);
		if (err < 0)
			goto fail_out;
	}

	/* Make sure we didn't get bogus platform data pointer. */
	if (!pdata) {
		pr_err("NULL platform data\n");
		goto fail_out;
	}

	if (!pdata->mem_size) {
		pr_err("The memory size must be non-zero\n");
		goto fail_out;
	}

	cxt->size = pdata->mem_size;
	cxt->phys_addr = pdata->mem_address;

	err = ramtrace_init_prz(cxt);
	/*
	 * Update the module parameter variables as well so they are visible
	 * through /sys/module/ramoops/parameters/
	 */
	mem_size = pdata->mem_size;
	mem_address = pdata->mem_address;

	pr_info("using 0x%lx@0x%llx\n",
		cxt->size, (unsigned long long)cxt->phys_addr);

	return 0;

fail_out:
	return err;

}

static int ramtrace_remove(struct platform_device *pdev)
{
/* todo: Free the alloc'ed structures */	

	return 0;
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "ramtrace" },
	{}
};

static struct platform_driver ramtrace_driver = {
	.probe		= ramtrace_probe,
	.remove		= ramtrace_remove,
	.driver		= {
		.name		= "ramtrace",
		.of_match_table	= dt_match,
	},
};

static inline void ramtrace_unregister_dummy(void)
{
	platform_device_unregister(dummy);
	dummy = NULL;
}

static void __init ramtrace_register_dummy(void)
{
	struct ramtrace_platform_data pdata;

	/*
	 * Prepare a dummy platform data structure to carry the module
	 * parameters. If mem_size isn't set, then there are no module
	 * parameters, and we can skip this.
	 */
	if (!mem_size)
		return;

	pr_info("using module parameters\n");

	memset(&pdata, 0, sizeof(pdata));
	pdata.mem_size = mem_size;
	pdata.mem_address = mem_address;


	dummy = platform_device_register_data(NULL, "ramtrace", -1,
			&pdata, sizeof(pdata));
	if (IS_ERR(dummy)) {
		pr_info("could not create platform device: %ld\n",
			PTR_ERR(dummy));
		dummy = NULL;
		ramtrace_unregister_dummy();
	}
}

static int __init ramtrace_init(void)
{
	int ret;

	ramtrace_register_dummy();
	ret = platform_driver_register(&ramtrace_driver);
	if (ret != 0)
		ramtrace_unregister_dummy();

	return ret;
}
postcore_initcall(ramtrace_init);

static void __exit ramtrace_exit(void)
{
	platform_driver_unregister(&ramtrace_driver);
	ramtrace_unregister_dummy();
}
module_exit(ramtrace_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RAM trace buffer manager/driver");
