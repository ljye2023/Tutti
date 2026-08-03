/*
 * peer_memory.h -- GPU peer-memory (NVIDIA P2P) isolation unit.
 *
 * This unit is the SINGLE point of contact between the snvme module
 * and the NVIDIA GPU driver's P2P API.  The module body (map.c, pci.c)
 * calls only through the peer_memory_ops function-pointer table and
 * uses the opaque types declared here; it never references
 * nvidia_p2p_* / nv_p2p_* symbols or includes nv-p2p.h directly.
 *
 * The NVIDIA P2P types (struct nvidia_p2p_page_table,
 * struct nvidia_p2p_dma_mapping) are hidden behind the opaque
 * peer_page_table / peer_dma_mapping forward declarations.  Field
 * access is via the accessor functions below, implemented in
 * peer_memory.c (the only .c that includes nv-p2p.h).
 *
 * If the NVIDIA P2P header (nv-p2p.h) is not available at compile
 * time, peer_memory.c fails with an explicit #error -- the module
 * is never silently built without GPU pinning capability.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#ifndef SNVME_PEER_MEMORY_H
#define SNVME_PEER_MEMORY_H

#include <linux/types.h>

struct pci_dev;

/* Opaque handles for NVIDIA P2P page-table / dma-mapping objects.
 * Defined as nvidia_p2p_page_table / nvidia_p2p_dma_mapping inside
 * peer_memory.c; the rest of the module only sees these forward
 * declarations. */
struct peer_page_table;
struct peer_dma_mapping;

/*
 * peer_memory_ops -- function-pointer table for all GPU P2P operations.
 *
 * Every pin / unpin / dma-map / page-table operation in the module goes
 * through this table.  The implementation (peer_memory.c) resolves the
 * NVIDIA nvidia_p2p_* symbols at module-load time via __symbol_get and
 * routes the calls through these pointers.
 */
struct peer_memory_ops {
	/*
	 * init: resolve nvidia_p2p_* symbols.  Returns 0 on success,
	 *       -1 if any symbol is missing (module load should fail).
	 * exit: release all resolved symbols.
	 */
	int  (*init)(void);
	void (*exit)(void);

	/*
	 * get_pages: pin a GPU virtual-address range and obtain the
	 *            page table.  free_cb is invoked by the NVIDIA
	 *            driver if it force-reclaims the pages (e.g. on
	 *            process exit); data is passed back to free_cb.
	 */
	int  (*get_pages)(uint64_t p2p_token, uint32_t va_space,
			  uint64_t vaddr, uint64_t length,
			  struct peer_page_table **pt,
			  void (*free_cb)(void *data), void *data);

	/*
	 * put_pages: release a page table obtained via get_pages.
	 */
	int  (*put_pages)(uint64_t p2p_token, uint32_t va_space,
			  uint64_t vaddr, struct peer_page_table *pt);

	/*
	 * dma_map_pages: create a DMA mapping for a page table on a
	 *                given peer PCI device (NVMe controller).
	 * dma_unmap_pages: destroy a DMA mapping.
	 */
	int  (*dma_map_pages)(struct pci_dev *peer,
			      struct peer_page_table *pt,
			      struct peer_dma_mapping **dm);
	int  (*dma_unmap_pages)(struct pci_dev *peer,
				struct peer_page_table *pt,
				struct peer_dma_mapping *dm);

	/*
	 * free_dma_mapping / free_page_table: free objects after all
	 * references are dropped (used in force-release paths).
	 */
	int  (*free_dma_mapping)(struct peer_dma_mapping *dm);
	int  (*free_page_table)(struct peer_page_table *pt);
};

/* The single ops instance, defined in peer_memory.c. */
extern const struct peer_memory_ops peer_memory_ops;

/*
 * Accessors for fields the module body needs to read.
 * Implemented in peer_memory.c (which includes nv-p2p.h).
 */
uint32_t peer_memory_pt_entries(const struct peer_page_table *pt);
const uint64_t *peer_memory_dm_addresses(const struct peer_dma_mapping *dm);

#endif /* SNVME_PEER_MEMORY_H */
