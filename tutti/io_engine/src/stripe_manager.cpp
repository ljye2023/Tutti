// Implementation of io_engine::StripeManager and StripeLayout::total_bytes.
//
// Pure byte-space math for the contiguous-per-shard (linear concatenation)
// stripe layout described in stripe_manager.h. Stateless, no backend, no I/O.

#include "io_engine/include/stripe_manager.h"

namespace tutti {
namespace io_engine {

uint64_t StripeLayout::total_bytes() const {
    uint64_t total = 0;
    for (const ShardGeometry& shard : shards) {
        total += shard.shard_bytes;
    }
    return total;
}

bool StripeManager::map(const StripeLayout& layout,
                        uint64_t logical_offset,
                        uint64_t length,
                        std::vector<SubIo>& out_subios) const {
    out_subios.clear();

    // Reject empty requests and empty layouts up front.
    if (length == 0 || layout.shards.empty()) {
        return false;
    }

    const uint64_t total = layout.total_bytes();

    // Bounds + overflow guard. We must verify logical_offset + length <= total
    // without ever computing logical_offset + length (which can wrap uint64).
    // Since total is finite, logical_offset alone must be <= total; given that,
    // (total - logical_offset) is a safe non-negative headroom we compare
    // against length. This rejects out-of-range ranges and any offset+length
    // that would overflow, because such a sum necessarily exceeds total.
    if (logical_offset > total || length > total - logical_offset) {
        return false;
    }

    // Walk shards, accumulating the prefix sum, and emit one SubIo per shard the
    // range [logical_offset, logical_offset + length) intersects. `cursor` is the
    // logical byte position we still need to place; it advances shard by shard,
    // and (cursor - logical_offset) gives region_byte_offset for each piece.
    const uint64_t region_end = logical_offset + length;  // safe: guarded above
    uint64_t prefix = 0;   // logical start of the current shard
    uint64_t cursor = logical_offset;

    for (uint32_t i = 0; i < layout.shards.size() && cursor < region_end; ++i) {
        const ShardGeometry& shard = layout.shards[i];
        const uint64_t shard_start = prefix;
        const uint64_t shard_end = prefix + shard.shard_bytes;  // safe: <= total
        prefix = shard_end;

        // Skip shards entirely before the range's current position. This also
        // skips zero-byte shards, whose [shard_start, shard_end) is empty.
        if (cursor >= shard_end) {
            continue;
        }

        // cursor lies within [shard_start, shard_end): emit the piece that runs
        // from cursor to the smaller of the shard boundary and the range end.
        const uint64_t piece_end = shard_end < region_end ? shard_end : region_end;

        SubIo sub;
        sub.shard_index = i;
        sub.vdev_index = shard.vdev_index;
        sub.shard_byte_offset = cursor - shard_start;
        sub.byte_length = piece_end - cursor;
        sub.region_byte_offset = cursor - logical_offset;
        out_subios.push_back(sub);

        cursor = piece_end;
    }

    return true;
}

} // namespace io_engine
} // namespace tutti
