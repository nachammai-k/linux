#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/pstore.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/compiler.h>
#include <linux/of.h>
#include <linux/of_address.h>
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
	void *base_address;
	int k, cpu;
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
	for(j = 0; j < PAGE_SIZE/sizeof(long long); j=j+4)
		printk(KERN_ERR "ramtrace %px : %llx %llx %llx %llx\n", bitmap_page + j, bitmap_page[j], bitmap_page[j+1], bitmap_page[j+2], bitmap_page[j+3]);
}

void ramtrace_dump(void)
{
	int i;
	printk(KERN_ERR "base_address %px\n", trace_ctx.base_address);
	for (i = 0; i < trace_ctx.cpu; i++)
	{
		printk(KERN_ERR "Dumping bitmap cpu %d\n", i);
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

int init_ramtrace_pages(int cpu, const char *tracer, int trace_clock)
{
	const char kernel_version[] = UTS_RELEASE;
	struct ramtrace_freelist *freelist_node;
	void *metapage;
	unsigned long flags;

	spin_lock_irqsave(&trace_ctx.lock, flags);
	freelist_node = list_next_entry(trace_ctx.freelist, list);
	metapage = page_address(freelist_node->page);
	list_del(&freelist_node->list);
        spin_unlock_irqrestore(&trace_ctx.lock, flags);

	trace_ctx.metadata = metapage;
	ramtrace_write_int((int **)&metapage, cpu);
	ramtrace_write_int((int **)&metapage, trace_clock);
	ramtrace_write_int((int **)&metapage, 1);
	sprintf(metapage, "%s", kernel_version);
	metapage += strlen(kernel_version) +1;
	sprintf(metapage, "%s", tracer);

	kfree(freelist_node);
	trace_ctx.cpu = cpu;
	ramtrace_init_bitmap(cpu * 1);
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
		int index;
		unsigned long flags;
		
                spin_lock_irqsave(&trace_ctx.lock, flags);
		freelist_node = list_next_entry(freelist, list);
		list_del(&freelist_node->list);
		spin_unlock_irqrestore(&trace_ctx.lock, flags);

		page = freelist_node->page;
		address = page_address(page);
		memset(address, 0, PAGE_SIZE);
		bitmap_page = (char *)page_address(trace_ctx.bitmap_pages[cpu]);
                index = (address - trace_ctx.base_address) >> PAGE_SHIFT;
		ramtrace_set_bit(bitmap_page, index);
		printk(KERN_ERR "allocating page ramtrace %px\n", address);

	}
	return page;

}

void ramtrace_free_page(void *page_address, int cpu)
{
	struct page *page = virt_to_page(page_address);
	int index = (page_address - trace_ctx.base_address) >> PAGE_SHIFT;
	void *metapage = trace_ctx.bitmap_pages[cpu];
printk(KERN_ERR "free page cpu %d index %d", cpu, index);
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


static int ramtrace_init_prz(struct ramtrace_context *ctx)
{

	struct page **pages;
	unsigned int page_count;
	pgprot_t prot;
	unsigned int i;
	struct ramtrace_freelist *freelist;

	page_count = DIV_ROUND_UP(ctx->phys_addr, PAGE_SIZE);

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

	/*
	 * For backwards compatibility ramoops.ecc=1 means 16 bytes ECC
	 * (using 1 byte for ECC isn't much of use anyway).
	 */

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
