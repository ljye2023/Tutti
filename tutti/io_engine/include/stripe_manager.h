#ifndef TUTTI_IO_ENGINE_STRIPE_MANAGER_H_
#define TUTTI_IO_ENGINE_STRIPE_MANAGER_H_

// io_engine::StripeManager -- the IO-time, backend-agnostic logical->physical
// stripe mapper (Layer 4).
//
// This is the "read-side dual" of block_storage::StripeManager: allocation
// decides *where data goes*; this decides *where data already is*. It is pure
// math -- stateless, no backend, no HAL, no CUDA, no I/O. It maps a logical
// byte range on a striped target into physical sub-IOs, one per shard touched.
//
// Layering: this component must NOT depend on block_storage (that would be an
// illegal L4->L5 upward dependency). It therefore defines its own input type,
// StripeLayout, which the caller (eventually Block Storage) populates from its
// own shard records before opening the target for IO.
//
// Units: the mapper works purely in *bytes*. It never touches block size or
// LBAs. The 512-vs-4096 block-size reconciliation is the caller's job when it
// computes each shard's byte length and builds the per-shard backend target;
// keeping the mapper in bytes keeps it truly transport-agnostic.
//
// Logical layout: contiguous-per-shard (linear concatenation), matching the
// allocation-time StripeManager. Shard 0 holds logical bytes [0, s0), shard 1
// holds [s0, s0+s1), etc. There is NO interleaved round-robin. Placement is
// data-dependent (greedy least-loaded at allocation time), so the mapper must
// read the shard list -- it cannot recompute placement by index arithmetic.

#include <cstdint>
#include <vector>

namespace tutti {
namespace io_engine {

// One shard's geometry within a striped target. Backend-agnostic: the mapper
// only needs each shard's usable byte length (for the prefix-sum) plus the
// vdevice index to route the resulting sub-IO. Physical base LBA / block size
// live in the per-shard backend target the engine builds separately; the mapper
// does not need them.
struct ShardGeometry {
    // Dense index into the backend's vdevice roster (i.e. VDeviceHandle{vdev_index}).
    // Identifies which vdevice physically holds this shard. Read from the shard
    // record; not derivable by index arithmetic.
    uint32_t vdev_index = 0;

    // Usable logical bytes this shard holds -- one contiguous run. The caller
    // must compute this consistently (e.g. length_blocks * block_size, using a
    // single namespace-derived block size).
    uint64_t shard_bytes = 0;
};

// The full stripe layout of one open target: an ordered list of shards where
// index == logical shard number.
struct StripeLayout {
    std::vector<ShardGeometry> shards;

    // Total logical bytes of the target = sum of every shard's shard_bytes.
    uint64_t total_bytes() const;
};

// One physical piece of a logical request after mapping. Each SubIo lies wholly
// within a single shard; transport-size (MDTS) fan-out is the engine's job, not
// the mapper's.
struct SubIo {
    uint32_t shard_index = 0;         // index into StripeLayout::shards
    uint32_t vdev_index = 0;          // == shards[shard_index].vdev_index (convenience)
    uint64_t shard_byte_offset = 0;   // byte offset within that shard
    uint64_t byte_length = 0;         // size of this piece in bytes
    uint64_t region_byte_offset = 0;  // offset within the logical request
                                      // (0-based from the request's start),
                                      // for buffer slicing / ioaddr indexing
};

// Backend-agnostic IO-time stripe mapper. Stateless.
class StripeManager {
public:
    // Map a logical [logical_offset, logical_offset + length) range on `layout`
    // into physical sub-IOs, splitting at each shard boundary crossed. The
    // out_subios vector is cleared first, then filled with >= 1 entries in
    // ascending logical order on success.
    //
    // Returns false (leaving out_subios cleared) if:
    //   - length == 0
    //   - layout.shards is empty
    //   - logical_offset + length > layout.total_bytes()  (out of bounds; also
    //     rejects overflow of logical_offset + length)
    // Returns true otherwise.
    bool map(const StripeLayout& layout,
             uint64_t logical_offset,
             uint64_t length,
             std::vector<SubIo>& out_subios) const;
};

} // namespace io_engine
} // namespace tutti

#endif // TUTTI_IO_ENGINE_STRIPE_MANAGER_H_
