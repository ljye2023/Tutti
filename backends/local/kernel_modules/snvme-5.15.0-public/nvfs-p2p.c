/*
 * Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include "nvfs-p2p.h"
#include "nvfs-core.h"

int nvfs_dbg_enabled = 1;
module_param(nvfs_dbg_enabled, int, 0644);
MODULE_PARM_DESC(nvfs_dbg_enabled, "Enable debug logging for metax p2p operations");
#ifdef HAVE_MODULE_MUTEX
extern struct mutex module_mutex;
#endif

static metax_p2p_acquire_mem_fptr metax_p2p_acquire_mem_p = NULL;
static metax_p2p_get_mem_fptr metax_p2p_get_mem_p = NULL;
static metax_p2p_put_mem_fptr metax_p2p_put_mem_p = NULL;
static metax_p2p_release_mem_fptr metax_p2p_release_mem_p = NULL;
static metax_p2p_get_page_size_fptr metax_p2p_get_page_size_p = NULL;
static metax_p2p_get_bus_offset_fptr metax_p2p_get_bus_offset_p = NULL;

static inline void nvfs_metax_put_symbols(void) {
	if(metax_p2p_acquire_mem_p) {
		__symbol_put("metax_p2p_acquire_mem");
	}
	if(metax_p2p_get_mem_p) {
		__symbol_put("metax_p2p_get_mem");
	}
	if(metax_p2p_put_mem_p) {
		__symbol_put("metax_p2p_put_mem");
	}
	if(metax_p2p_release_mem_p) {
		__symbol_put("metax_p2p_release_mem");
	}
	if(metax_p2p_get_page_size_p) {
		__symbol_put("metax_p2p_get_page_size");
	}
	if(metax_p2p_get_bus_offset_p) {
		__symbol_put("metax_p2p_get_bus_offset");
	}
	metax_p2p_acquire_mem_p = NULL;
	metax_p2p_get_mem_p = NULL;
	metax_p2p_put_mem_p = NULL;
	metax_p2p_release_mem_p = NULL;
	metax_p2p_get_page_size_p = NULL;
	metax_p2p_get_bus_offset_p = NULL;
}

int nvfs_nvidia_p2p_init() {

#ifdef HAVE_MODULE_MUTEX
	mutex_lock(&module_mutex);
#endif
	if(metax_p2p_acquire_mem_p == NULL) {
		metax_p2p_acquire_mem_p = __symbol_get("metax_p2p_acquire_mem");
		if(metax_p2p_acquire_mem_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_acquire_mem \n");
			goto error;
		}
	}

	if(metax_p2p_get_mem_p == NULL) {
		metax_p2p_get_mem_p = __symbol_get("metax_p2p_get_mem");
		if(metax_p2p_get_mem_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_get_mem \n");
			goto error;
		}
	}

	if(metax_p2p_put_mem_p == NULL) {
		metax_p2p_put_mem_p = __symbol_get("metax_p2p_put_mem");
		if(metax_p2p_put_mem_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_put_mem \n");
			goto error;
		}
	}

	if(metax_p2p_release_mem_p == NULL) {
		metax_p2p_release_mem_p = __symbol_get("metax_p2p_release_mem");
		if(metax_p2p_release_mem_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_release_mem \n");
			goto error;
		}
	}

	if(metax_p2p_get_page_size_p == NULL) {
		metax_p2p_get_page_size_p = __symbol_get("metax_p2p_get_page_size");
		if(metax_p2p_get_page_size_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_get_page_size \n");
			goto error;
		}
	}

	if(metax_p2p_get_bus_offset_p == NULL) {
		metax_p2p_get_bus_offset_p = __symbol_get("metax_p2p_get_bus_offset");
		if(metax_p2p_get_bus_offset_p == NULL) {
			nvfs_err("Unable to find symbol: metax_p2p_get_bus_offset \n");
			goto error;
		}
	}
#ifdef HAVE_MODULE_MUTEX
	mutex_unlock(&module_mutex);
#endif
	return 0;
error:
#ifdef HAVE_MODULE_MUTEX
	mutex_unlock(&module_mutex);
#endif
	nvfs_metax_put_symbols();
	return -1;
}

void nvfs_nvidia_p2p_exit() {
#ifdef HAVE_MODULE_MUTEX
	mutex_lock(&module_mutex);
#endif
	nvfs_metax_put_symbols();
#ifdef HAVE_MODULE_MUTEX
	mutex_unlock(&module_mutex);
#endif
}

int nvfs_nvidia_p2p_dma_unmap_pages(struct pci_dev *peer,
		struct nvidia_p2p_page_table *page_table,
		struct nvidia_p2p_dma_mapping *dma_mapping) {
	if (dma_mapping != NULL) {
		if (dma_mapping->dma_addresses != NULL) {
			kfree(dma_mapping->dma_addresses);
		}
		kfree(dma_mapping);
	}
	return 0;
}

static int nvfs_metax_acquire_callback_wrapper(void *arg) {
	struct nvidia_p2p_page_table *pt = (struct nvidia_p2p_page_table *)arg;
	if (pt && pt->free_callback) {
		pt->free_callback(pt->data);
	}
	return 0;
}

int nvfs_nvidia_p2p_get_pages(uint64_t p2p_token, uint32_t va_space,
		uint64_t virtual_address,
		uint64_t length,
		struct nvidia_p2p_page_table **page_table,
		void (*free_callback)(void *data),
		void *data) {
	int err;
	struct nvidia_p2p_page_table *pt;
	uint32_t page_size;
	uint32_t entries;

	if(metax_p2p_acquire_mem_p == NULL || metax_p2p_get_mem_p == NULL ||
			metax_p2p_get_page_size_p == NULL) {
		return -ENOMEM;
	}

	pt = kmalloc(sizeof(struct nvidia_p2p_page_table), GFP_KERNEL);
	if (pt == NULL) {
		nvfs_err("Failed to allocate page table\n");
		return -ENOMEM;
	}
	pt->free_callback = free_callback;
	pt->data = data;
	err = metax_p2p_acquire_mem_p(virtual_address, length, &pt->handle, nvfs_metax_acquire_callback_wrapper, pt);
	nvfs_dbg("metax_p2p_acquire_mem_p: virtual_address=0x%llx, length=0x%llx, handle=%p, pt=%p, err=%d\n",
		 (unsigned long long)virtual_address, (unsigned long long)length, pt->handle, pt, err);
	if (err != 0) {
		nvfs_err("metax_p2p_acquire_mem failed: %d\n", err);
		kfree(pt);
		return err;
	}

	err = metax_p2p_get_mem_p(pt->handle, &pt->sgt);
	nvfs_dbg("metax_p2p_get_mem_p: handle=%p, sgt=%p, err=%d\n", pt->handle, pt->sgt, err);
	if (err != 0) {
		nvfs_err("metax_p2p_get_mem failed: %d\n", err);
		if(metax_p2p_release_mem_p)
			metax_p2p_release_mem_p(pt->handle);
		kfree(pt);
		return err;
	}

	page_size = metax_p2p_get_page_size_p(pt->handle);
	nvfs_dbg("metax_p2p_get_page_size_p: handle=%p, page_size=%u\n", pt->handle, page_size);
	if (page_size == 0) {
		page_size = 1 << 16;
	}
	pt->page_size = page_size;

	entries = length / page_size;
	pt->entries = entries;
        nvfs_dbg("metax_p2p_get_page_size_p: handle=%p, entries=%u\n", pt->handle, entries);
	*page_table = pt;
	return 0;
}

int nvfs_nvidia_p2p_put_pages(uint64_t p2p_token, uint32_t va_space,
		uint64_t virtual_address,
		struct nvidia_p2p_page_table *page_table) {
	if (page_table == NULL) {
		return 0;
	}

	if (page_table->sgt != NULL) {
		nvfs_dbg("metax_p2p_put_mem_p: handle=%p, sgt=%p\n", page_table->handle, page_table->sgt);
		if(metax_p2p_put_mem_p)
			metax_p2p_put_mem_p(page_table->handle, page_table->sgt);
	}

	if (page_table->handle != NULL) {
		if(metax_p2p_release_mem_p) {
			nvfs_dbg("metax_p2p_release_mem_p: handle=%p\n", page_table->handle);
			metax_p2p_release_mem_p(page_table->handle);
		}
	}

	kfree(page_table);
	return 0;
}

int nvfs_nvidia_p2p_dma_map_pages(struct pci_dev *peer,
	        struct nvidia_p2p_page_table *page_table,
			uint32_t request_page_size,
				uint32_t request_naddr,
		        struct nvidia_p2p_dma_mapping **dma_mapping) {
	struct nvidia_p2p_dma_mapping *mapping;
	uint64_t offset;
	uint32_t i;
	struct scatterlist *sg;
	uint64_t *dma_addrs;
	uint32_t entry = 0;

	if (page_table == NULL || page_table->sgt == NULL) {
		return -EINVAL;
	}

	if(metax_p2p_get_bus_offset_p == NULL) {
		return -ENOMEM;
	}

	mapping = kmalloc(sizeof(struct nvidia_p2p_dma_mapping), GFP_KERNEL);
	if (mapping == NULL) {
		nvfs_err("Failed to allocate dma mapping\n");
		return -ENOMEM;
	}

	offset = metax_p2p_get_bus_offset_p(page_table->handle);
	nvfs_dbg("metax_p2p_get_bus_offset_p: handle=%p, offset=0x%llx\n",
		 page_table->handle, (unsigned long long)offset);

	dma_addrs = kmalloc(request_naddr * sizeof(uint64_t), GFP_KERNEL);
	if (dma_addrs == NULL) {
		nvfs_err("Failed to allocate dma addresses array\n");
		kfree(mapping);
		return -ENOMEM;
	}

	for_each_sg(page_table->sgt->sgl, sg, page_table->sgt->nents, i) {
		//uint64_t addr = sg_phys(sg);
		uint64_t addr = sg->dma_address;
		uint32_t len = sg->length;
		uint32_t pages = (len - offset) / request_page_size;
		uint32_t j;

		for (j = 0; j < pages; j++) {
			if (entry < request_naddr) {
				dma_addrs[entry] = addr + offset + j * request_page_size;
				// nvfs_dbg("metax_p2p calc dma: handle=%p, sg=%p,index=%u, phy_addr=%llx, dma_addr=%llx, off 0x%x\n",
				// 	page_table->handle, sg, entry, addr, dma_addrs[entry], sg->offset);
				entry++;
			}
		}
	}

	mapping->dma_addresses = dma_addrs;
	mapping->entries = entry;
	page_table->virtual_entries = entry;

	*dma_mapping = mapping;
	return 0;
}

int nvfs_nvidia_p2p_free_dma_mapping(struct nvidia_p2p_dma_mapping *dma_mapping) {
	if (dma_mapping == NULL) {
		return 0;
	}
	if (dma_mapping->dma_addresses != NULL) {
		kfree(dma_mapping->dma_addresses);
	}
	kfree(dma_mapping);
	return 0;
}

int nvfs_nvidia_p2p_free_page_table(struct nvidia_p2p_page_table *page_table) {
	if (page_table == NULL) {
		return 0;
	}

	nvfs_dbg("nvfs_nvidia_p2p_free_page_table: %p\n", (void *)page_table, current->pid);
	kfree(page_table);
	return 0;
}
