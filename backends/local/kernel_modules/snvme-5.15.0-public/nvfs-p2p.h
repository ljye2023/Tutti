/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
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

#ifndef NVFS_P2P_H
#define NVFS_P2P_H

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/pci.h>
#include "metax_p2p.h"

typedef int (*metax_p2p_acquire_mem_fptr)(uint64_t, size_t, void **, int (*)(void *), void *);
typedef int (*metax_p2p_get_mem_fptr)(void *, struct sg_table **);
typedef int (*metax_p2p_put_mem_fptr)(void *, struct sg_table *);
typedef void (*metax_p2p_release_mem_fptr)(void *);
typedef uint32_t (*metax_p2p_get_page_size_fptr)(void *);
typedef uint64_t (*metax_p2p_get_bus_offset_fptr)(void *);

struct nvidia_p2p_page_table {
	void *handle;
	struct sg_table *sgt;
	uint32_t entries;
	uint32_t virtual_entries;
	uint32_t page_size;
    void (*free_callback)(void *data);
    void *data;
};
typedef struct nvidia_p2p_page_table nvidia_p2p_page_table_t;

struct nvidia_p2p_dma_mapping {
	uint64_t *dma_addresses;
	uint32_t entries;
};
typedef struct nvidia_p2p_dma_mapping nvidia_p2p_dma_mapping_t;

int nvfs_nvidia_p2p_dma_unmap_pages(struct pci_dev *peer,
		struct nvidia_p2p_page_table *page_table,
		struct nvidia_p2p_dma_mapping *dma_mapping);
int nvfs_nvidia_p2p_get_pages(uint64_t p2p_token, uint32_t va_space,
		uint64_t virtual_address,
		uint64_t length,
		struct nvidia_p2p_page_table **page_table,
		void (*free_callback)(void *data),
		void *data);
int nvfs_nvidia_p2p_put_pages(uint64_t p2p_token, uint32_t va_space,
		uint64_t virtual_address,
		struct nvidia_p2p_page_table *page_table);
int nvfs_nvidia_p2p_dma_map_pages(struct pci_dev *peer,
		        struct nvidia_p2p_page_table *page_table,
				uint32_t request_page_size,
				uint32_t request_naddr,
			        struct nvidia_p2p_dma_mapping **dma_mapping);
int nvfs_nvidia_p2p_free_dma_mapping(struct nvidia_p2p_dma_mapping *dma_mapping);
int nvfs_nvidia_p2p_free_page_table(struct nvidia_p2p_page_table *page_table);

int nvfs_nvidia_p2p_init(void);
void nvfs_nvidia_p2p_exit(void);

#endif
