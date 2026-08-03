/*
 * peer_memory.c -- GPU peer-memory (NVIDIA P2P) isolation unit.
 *
 * This is the ONLY translation unit in the snvme module that includes
 * nv-p2p.h and calls nvidia_p2p_* functions.  All interaction with the
 * NVIDIA GPU driver's P2P API is funneled through the peer_memory_ops
 * table declared in peer_memory.h.
 *
 * The NVIDIA symbols are resolved at module-load time via __symbol_get
 * (not at link time), so snvme.ko has no build-time dependency on the
 * NVIDIA driver module -- only on its header (nv-p2p.h).  If nv-p2p.h
 * is missing at compile time, the build fails with an explicit #error
 * rather than silently producing a module without pin capability.
 *
 * SPDX-License-Identifier: GPL-2.0
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/mutex.h>

#include "peer_memory.h"
#include "nvfs-core.h"

/* -----------------------------------------------------------------------
 * NVIDIA header dependency guard
 *
 * nv-p2p.h provides the struct definitions (nvidia_p2p_page_table,
 * nvidia_p2p_dma_mapping) and the function prototypes that we resolve
 * at runtime.  Its path is supplied via the module ccflags
 * (-I<driver_include>, configured by CMake from the /usr/src/nvidia-*
 * tree).  If it cannot be found, fail loudly.
 * ----------------------------------------------------------------------- */
#if defined(__has_include)
#  if __has_include("nv-p2p.h")
#    include "nv-p2p.h"
#  else
#    error "nv-p2p.h not found: NVIDIA P2P headers are required to build" \
           " peer_memory. Ensure the NVIDIA driver source tree is installed" \
           " (e.g. /usr/src/nvidia-*/) and its path is passed via the" \
           " module ccflags (-I<driver_include>). A module built without" \
           " these headers would have NO GPU pinning capability."
#  endif
#else
   /* Older compilers without __has_include: let #include fail naturally. */
#  include "nv-p2p.h"
#endif

/* Function-pointer typedefs for the NVIDIA P2P symbols resolved at
 * runtime via __symbol_get.  Previously declared in nvfs-p2p.h; moved
 * here when the P2P unit was consolidated into peer_memory. */
typedef int (*nvidia_p2p_dma_unmap_pages_fptr) (struct pci_dev*,
		struct nvidia_p2p_page_table*,
		struct nvidia_p2p_dma_mapping*);
typedef int (*nvidia_p2p_get_pages_fptr) (uint64_t, uint32_t,
		uint64_t,
		uint64_t ,
		struct nvidia_p2p_page_table **,
		void (*free_callback)(void *data),
		void *);
typedef int (*nvidia_p2p_put_pages_fptr)(uint64_t, uint32_t,
		uint64_t,
		struct nvidia_p2p_page_table *);
typedef int (*nvidia_p2p_dma_map_pages_fptr)(struct pci_dev *,
		        struct nvidia_p2p_page_table *,
			struct nvidia_p2p_dma_mapping **);
typedef int (*nvidia_p2p_free_dma_mapping_fptr)(struct nvidia_p2p_dma_mapping *);
typedef int (*nvidia_p2p_free_page_table_fptr)(struct nvidia_p2p_page_table *);

int nvfs_dbg_enabled = 0;

#ifdef HAVE_MODULE_MUTEX
extern struct mutex module_mutex;
#endif

/* -----------------------------------------------------------------------
 * Resolved NVIDIA P2P function pointers (filled by peer_init).
 * ----------------------------------------------------------------------- */
static nvidia_p2p_dma_unmap_pages_fptr  nvidia_p2p_dma_unmap_pages_p = NULL;
static nvidia_p2p_get_pages_fptr        nvidia_p2p_get_pages_p = NULL;
static nvidia_p2p_put_pages_fptr        nvidia_p2p_put_pages_p = NULL;
static nvidia_p2p_dma_map_pages_fptr    nvidia_p2p_dma_map_pages_p = NULL;
static nvidia_p2p_free_dma_mapping_fptr nvidia_p2p_free_dma_mapping_p = NULL;
static nvidia_p2p_free_page_table_fptr  nvidia_p2p_free_page_table_p = NULL;

static inline void peer_put_symbols(void)
{
	if (nvidia_p2p_dma_unmap_pages_p) {
		__symbol_put("nvidia_p2p_dma_unmap_pages");
		nvidia_p2p_dma_unmap_pages_p = NULL;
	}
	if (nvidia_p2p_get_pages_p) {
		__symbol_put("nvidia_p2p_get_pages");
		nvidia_p2p_get_pages_p = NULL;
	}
	if (nvidia_p2p_put_pages_p) {
		__symbol_put("nvidia_p2p_put_pages");
		nvidia_p2p_put_pages_p = NULL;
	}
	if (nvidia_p2p_dma_map_pages_p) {
		__symbol_put("nvidia_p2p_dma_map_pages");
		nvidia_p2p_dma_map_pages_p = NULL;
	}
	if (nvidia_p2p_free_dma_mapping_p) {
		__symbol_put("nvidia_p2p_free_dma_mapping");
		nvidia_p2p_free_dma_mapping_p = NULL;
	}
	if (nvidia_p2p_free_page_table_p) {
		__symbol_put("nvidia_p2p_free_page_table");
		nvidia_p2p_free_page_table_p = NULL;
	}
}

/* ----------------------------------------------------------------------- *
 * peer_memory_ops implementations
 *
 * Each op checks that its underlying symbol pointer was resolved; if
 * not, it returns -ENOMEM (matching the original wrapper semantics).
 * wrappers).  The opaque peer_page_table / peer_dma_mapping pointers
 * are cast to/from the NVIDIA types at this boundary.
 * ----------------------------------------------------------------------- */

static int peer_init(void)
{
#ifdef HAVE_MODULE_MUTEX
	mutex_lock(&module_mutex);
#endif
	if (nvidia_p2p_dma_unmap_pages_p == NULL) {
		nvidia_p2p_dma_unmap_pages_p = __symbol_get("nvidia_p2p_dma_unmap_pages");
		if (nvidia_p2p_dma_unmap_pages_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_dma_unmap_pages \n");
			goto error;
		}
	}
	if (nvidia_p2p_get_pages_p == NULL) {
		nvidia_p2p_get_pages_p = __symbol_get("nvidia_p2p_get_pages");
		if (nvidia_p2p_get_pages_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_get_pages \n");
			goto error;
		}
	}
	if (nvidia_p2p_put_pages_p == NULL) {
		nvidia_p2p_put_pages_p = __symbol_get("nvidia_p2p_put_pages");
		if (nvidia_p2p_put_pages_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_put_pages \n");
			goto error;
		}
	}
	if (nvidia_p2p_dma_map_pages_p == NULL) {
		nvidia_p2p_dma_map_pages_p = __symbol_get("nvidia_p2p_dma_map_pages");
		if (nvidia_p2p_dma_map_pages_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_dma_map_pages \n");
			goto error;
		}
	}
	if (nvidia_p2p_free_dma_mapping_p == NULL) {
		nvidia_p2p_free_dma_mapping_p = __symbol_get("nvidia_p2p_free_dma_mapping");
		if (nvidia_p2p_free_dma_mapping_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_free_dma_mapping \n");
			goto error;
		}
	}
	if (nvidia_p2p_free_page_table_p == NULL) {
		nvidia_p2p_free_page_table_p = __symbol_get("nvidia_p2p_free_page_table");
		if (nvidia_p2p_free_page_table_p == NULL) {
			nvfs_err("Unable to find symbol: nvidia_p2p_free_page_table \n");
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
	peer_put_symbols();
	return -1;
}

static void peer_exit(void)
{
#ifdef HAVE_MODULE_MUTEX
	mutex_lock(&module_mutex);
#endif
	peer_put_symbols();
#ifdef HAVE_MODULE_MUTEX
	mutex_unlock(&module_mutex);
#endif
}

static int peer_get_pages(uint64_t p2p_token, uint32_t va_space,
			  uint64_t vaddr, uint64_t length,
			  struct peer_page_table **pt,
			  void (*free_cb)(void *data), void *data)
{
	struct nvidia_p2p_page_table *npt = NULL;
	int ret;

	if (!nvidia_p2p_get_pages_p)
		return -ENOMEM;

	ret = nvidia_p2p_get_pages_p(p2p_token, va_space, vaddr, length,
				     &npt, free_cb, data);
	if (ret == 0)
		*pt = (struct peer_page_table *)npt;
	return ret;
}

static int peer_put_pages(uint64_t p2p_token, uint32_t va_space,
			  uint64_t vaddr, struct peer_page_table *pt)
{
	if (!nvidia_p2p_put_pages_p)
		return -ENOMEM;
	return nvidia_p2p_put_pages_p(p2p_token, va_space, vaddr,
				      (struct nvidia_p2p_page_table *)pt);
}

static int peer_dma_map_pages(struct pci_dev *peer,
			      struct peer_page_table *pt,
			      struct peer_dma_mapping **dm)
{
	struct nvidia_p2p_dma_mapping *ndm = NULL;
	int ret;

	if (!nvidia_p2p_dma_map_pages_p)
		return -ENOMEM;

	ret = nvidia_p2p_dma_map_pages_p(peer,
					 (struct nvidia_p2p_page_table *)pt,
					 &ndm);
	if (ret == 0)
		*dm = (struct peer_dma_mapping *)ndm;
	return ret;
}

static int peer_dma_unmap_pages(struct pci_dev *peer,
				struct peer_page_table *pt,
				struct peer_dma_mapping *dm)
{
	if (!nvidia_p2p_dma_unmap_pages_p)
		return -ENOMEM;
	return nvidia_p2p_dma_unmap_pages_p(peer,
					    (struct nvidia_p2p_page_table *)pt,
					    (struct nvidia_p2p_dma_mapping *)dm);
}

static int peer_free_dma_mapping(struct peer_dma_mapping *dm)
{
	if (!nvidia_p2p_free_dma_mapping_p)
		return -ENOMEM;
	return nvidia_p2p_free_dma_mapping_p((struct nvidia_p2p_dma_mapping *)dm);
}

static int peer_free_page_table(struct peer_page_table *pt)
{
	if (!nvidia_p2p_free_page_table_p)
		return -ENOMEM;
	return nvidia_p2p_free_page_table_p((struct nvidia_p2p_page_table *)pt);
}

/* -----------------------------------------------------------------------
 * Field accessors (the body reads page-count and DMA addresses).
 * ----------------------------------------------------------------------- */
uint32_t peer_memory_pt_entries(const struct peer_page_table *pt)
{
	return ((const struct nvidia_p2p_page_table *)pt)->entries;
}

const uint64_t *peer_memory_dm_addresses(const struct peer_dma_mapping *dm)
{
	return ((const struct nvidia_p2p_dma_mapping *)dm)->dma_addresses;
}

/* -----------------------------------------------------------------------
 * The single ops instance.
 * ----------------------------------------------------------------------- */
const struct peer_memory_ops peer_memory_ops = {
	.init              = peer_init,
	.exit              = peer_exit,
	.get_pages         = peer_get_pages,
	.put_pages         = peer_put_pages,
	.dma_map_pages     = peer_dma_map_pages,
	.dma_unmap_pages   = peer_dma_unmap_pages,
	.free_dma_mapping  = peer_free_dma_mapping,
	.free_page_table   = peer_free_page_table,
};
