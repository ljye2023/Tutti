// tutti/tests/io_engine/stripe_manager_test.cpp
// Standalone unit test for io_engine::StripeManager (Layer 4 stripe mapper).
//
// Pure-math contract: map() splits a logical byte range on a contiguous-per-shard
// (linear concatenation) layout into one SubIo per shard touched. This test asserts
// exact field values, not just SubIo counts. No gtest -- plain <cassert> + prints,
// mirroring tutti/io_engine/tests/layer4_simple_test.cpp.
//
// Compiled by CMake together with ../../io_engine/src/stripe_manager.cpp, so the
// logic itself is NOT redefined here.

#include <iostream>
#include <cassert>
#include <cstdint>
#include <vector>
#include "io_engine/include/stripe_manager.h"

using namespace tutti::io_engine;

// Build the canonical 3-shard layout used across several cases. Uses non-identity
// vdev_index values {5, 2, 9} so we can prove vdev_index is read from the shard
// record rather than recomputed from shard_index. Each shard is 100 bytes, total 300.
static StripeLayout make_layout_300() {
    StripeLayout layout;
    layout.shards.push_back(ShardGeometry{5, 100});  // shard 0: logical [0,100)
    layout.shards.push_back(ShardGeometry{2, 100});  // shard 1: logical [100,200)
    layout.shards.push_back(ShardGeometry{9, 100});  // shard 2: logical [200,300)
    return layout;
}

// (a) Empty layout -> map returns false and leaves out_subios empty.
void test_empty_layout() {
    std::cout << "\n[TEST] Empty layout -> false" << std::endl;

    StripeManager mgr;
    StripeLayout empty;  // no shards
    std::vector<SubIo> out;

    bool ok = mgr.map(empty, 0, 10, out);
    assert(!ok);
    assert(out.empty());
    assert(empty.total_bytes() == 0);

    std::cout << "  \xE2\x9C\x93 Empty layout rejected, out_subios empty" << std::endl;
}

// (b) length == 0 -> false, even on a valid non-empty layout.
void test_zero_length() {
    std::cout << "\n[TEST] length == 0 -> false" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();
    std::vector<SubIo> out;

    bool ok = mgr.map(layout, 50, 0, out);
    assert(!ok);
    assert(out.empty());

    std::cout << "  \xE2\x9C\x93 Zero-length request rejected" << std::endl;
}

// (c) Out of bounds: logical_offset + length > total_bytes() -> false.
void test_out_of_bounds() {
    std::cout << "\n[TEST] offset + length > total -> false" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();  // total = 300
    std::vector<SubIo> out;

    // 290 + 20 = 310 > 300: rejected.
    bool ok = mgr.map(layout, 290, 20, out);
    assert(!ok);
    assert(out.empty());

    // Offset itself past the end: 300 + 1 with total 300 -> false.
    ok = mgr.map(layout, 300, 1, out);
    assert(!ok);
    assert(out.empty());

    std::cout << "  \xE2\x9C\x93 Out-of-bounds range rejected, out_subios empty" << std::endl;
}

// (d) Single-shard request fully inside shard 0.
void test_single_shard_inside() {
    std::cout << "\n[TEST] single shard, fully inside shard 0" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();
    std::vector<SubIo> out;

    // [10, 30) lies wholly inside shard 0 ([0,100)).
    bool ok = mgr.map(layout, 10, 20, out);
    assert(ok);
    assert(out.size() == 1);

    assert(out[0].shard_index == 0);
    assert(out[0].vdev_index == 5);           // shard 0's vdev
    assert(out[0].shard_byte_offset == 10);   // 10 - prefix[0]=0
    assert(out[0].byte_length == 20);
    assert(out[0].region_byte_offset == 0);   // first piece

    std::cout << "  \xE2\x9C\x93 Single in-shard piece has exact fields" << std::endl;
}

// (e) Request spanning exactly 2 shards -> 2 SubIos with exact fields.
void test_span_two_shards() {
    std::cout << "\n[TEST] span exactly 2 shards" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();  // shards: [0,100)v5 [100,200)v2 [200,300)v9
    std::vector<SubIo> out;

    // Request [50, 150): starts mid shard 0, ends mid shard 1 -> 2 pieces.
    bool ok = mgr.map(layout, 50, 100, out);
    assert(ok);
    assert(out.size() == 2);

    // Piece 0: shard 0, bytes [50,100) -> 50 bytes at shard offset 50, region 0.
    assert(out[0].shard_index == 0);
    assert(out[0].vdev_index == 5);
    assert(out[0].shard_byte_offset == 50);
    assert(out[0].byte_length == 50);
    assert(out[0].region_byte_offset == 0);

    // Piece 1: shard 1, bytes [100,150) -> 50 bytes at shard offset 0, region 50.
    assert(out[1].shard_index == 1);
    assert(out[1].vdev_index == 2);
    assert(out[1].shard_byte_offset == 0);
    assert(out[1].byte_length == 50);
    assert(out[1].region_byte_offset == 50);

    std::cout << "  \xE2\x9C\x93 Two-shard span split at boundary 100 with exact fields" << std::endl;
}

// (f) Request spanning 3+ shards with partial head and partial tail.
void test_span_three_shards() {
    std::cout << "\n[TEST] span 3 shards, partial head + partial tail" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();  // [0,100)v5 [100,200)v2 [200,300)v9
    std::vector<SubIo> out;

    // Request [50, 250): partial shard 0, full shard 1, partial shard 2.
    bool ok = mgr.map(layout, 50, 200, out);
    assert(ok);
    assert(out.size() == 3);

    // Head: shard 0 bytes [50,100) -> 50 bytes, shard offset 50, region 0.
    assert(out[0].shard_index == 0);
    assert(out[0].vdev_index == 5);
    assert(out[0].shard_byte_offset == 50);
    assert(out[0].byte_length == 50);
    assert(out[0].region_byte_offset == 0);

    // Middle: shard 1 full [100,200) -> 100 bytes, shard offset 0, region 50.
    assert(out[1].shard_index == 1);
    assert(out[1].vdev_index == 2);
    assert(out[1].shard_byte_offset == 0);
    assert(out[1].byte_length == 100);
    assert(out[1].region_byte_offset == 50);

    // Tail: shard 2 bytes [200,250) -> 50 bytes, shard offset 0, region 150.
    assert(out[2].shard_index == 2);
    assert(out[2].vdev_index == 9);
    assert(out[2].shard_byte_offset == 0);
    assert(out[2].byte_length == 50);
    assert(out[2].region_byte_offset == 150);

    std::cout << "  \xE2\x9C\x93 Three-shard span head/middle/tail exact fields" << std::endl;
}

// (g) Unequal shard sizes: verify prefix-sum arithmetic on a last-shard remainder.
void test_unequal_shards_last_remainder() {
    std::cout << "\n[TEST] unequal shards, last-shard remainder" << std::endl;

    StripeManager mgr;
    StripeLayout layout;
    layout.shards.push_back(ShardGeometry{5, 100});  // shard 0: [0,100)
    layout.shards.push_back(ShardGeometry{2, 50});   // shard 1: [100,150)
    layout.shards.push_back(ShardGeometry{9, 30});   // shard 2: [150,180)  total=180
    std::vector<SubIo> out;

    assert(layout.total_bytes() == 180);

    // Request [140, 180): tail of shard 1 + all of shard 2 (the remainder shard).
    bool ok = mgr.map(layout, 140, 40, out);
    assert(ok);
    assert(out.size() == 2);

    // Piece 0: shard 1, bytes [140,150) -> 10 bytes at shard offset 40 (140-100), region 0.
    assert(out[0].shard_index == 1);
    assert(out[0].vdev_index == 2);
    assert(out[0].shard_byte_offset == 40);
    assert(out[0].byte_length == 10);
    assert(out[0].region_byte_offset == 0);

    // Piece 1: shard 2, bytes [150,180) -> full 30-byte remainder, shard offset 0, region 10.
    assert(out[1].shard_index == 2);
    assert(out[1].vdev_index == 9);
    assert(out[1].shard_byte_offset == 0);
    assert(out[1].byte_length == 30);
    assert(out[1].region_byte_offset == 10);

    std::cout << "  \xE2\x9C\x93 Unequal shards, remainder tail mapped exactly" << std::endl;
}

// (h) Request exactly covering the whole target -> one full piece per shard.
void test_cover_whole_target() {
    std::cout << "\n[TEST] cover whole target [0, total)" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();
    std::vector<SubIo> out;

    bool ok = mgr.map(layout, 0, 300, out);
    assert(ok);
    assert(out.size() == 3);

    // Each shard contributes its full 100 bytes; region offsets are 0/100/200.
    assert(out[0].shard_index == 0 && out[0].vdev_index == 5);
    assert(out[0].shard_byte_offset == 0 && out[0].byte_length == 100 && out[0].region_byte_offset == 0);

    assert(out[1].shard_index == 1 && out[1].vdev_index == 2);
    assert(out[1].shard_byte_offset == 0 && out[1].byte_length == 100 && out[1].region_byte_offset == 100);

    assert(out[2].shard_index == 2 && out[2].vdev_index == 9);
    assert(out[2].shard_byte_offset == 0 && out[2].byte_length == 100 && out[2].region_byte_offset == 200);

    std::cout << "  \xE2\x9C\x93 Full-target request maps to one full piece per shard" << std::endl;
}

// (i) vdev_index is copied from ShardGeometry, not recomputed from shard_index.
// Uses vdev values {5,2,9} that differ from their positions {0,1,2}, and touches
// every shard so all three copied values are asserted.
void test_vdev_index_is_read_not_recomputed() {
    std::cout << "\n[TEST] vdev_index read from geometry, not index" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();  // vdevs {5,2,9} != positions {0,1,2}
    std::vector<SubIo> out;

    bool ok = mgr.map(layout, 0, 300, out);
    assert(ok);
    assert(out.size() == 3);

    // If placement were recomputed as shard_index, these would be 0/1/2 and fail.
    assert(out[0].shard_index == 0 && out[0].vdev_index == 5);
    assert(out[1].shard_index == 1 && out[1].vdev_index == 2);
    assert(out[2].shard_index == 2 && out[2].vdev_index == 9);

    // Explicitly prove vdev_index != shard_index for at least one shard.
    assert(out[0].vdev_index != out[0].shard_index);

    std::cout << "  \xE2\x9C\x93 vdev_index reflects placement {5,2,9}, not shard index" << std::endl;
}

// (j) out_subios is cleared on a false return even when the same vector was
// reused after a prior successful call.
void test_out_cleared_on_false_after_reuse() {
    std::cout << "\n[TEST] out_subios cleared on false after reuse" << std::endl;

    StripeManager mgr;
    StripeLayout layout = make_layout_300();
    std::vector<SubIo> out;

    // First: a successful call fills the vector.
    bool ok = mgr.map(layout, 50, 100, out);
    assert(ok);
    assert(out.size() == 2);

    // Reuse the same vector for an out-of-bounds request -> false, must be empty.
    ok = mgr.map(layout, 290, 20, out);
    assert(!ok);
    assert(out.empty());

    // Reuse again for a zero-length request -> false, still empty.
    // Re-fill first to prove the clear happens on this path too.
    ok = mgr.map(layout, 50, 100, out);
    assert(ok && out.size() == 2);
    ok = mgr.map(layout, 50, 0, out);
    assert(!ok);
    assert(out.empty());

    std::cout << "  \xE2\x9C\x93 Reused vector cleared on every false return" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "io_engine::StripeManager - Unit Test" << std::endl;
    std::cout << "==========================================" << std::endl;

    int passed = 0;
    int total = 10;

    try {
        test_empty_layout();                    passed++;
        test_zero_length();                     passed++;
        test_out_of_bounds();                   passed++;
        test_single_shard_inside();             passed++;
        test_span_two_shards();                 passed++;
        test_span_three_shards();               passed++;
        test_unequal_shards_last_remainder();   passed++;
        test_cover_whole_target();              passed++;
        test_vdev_index_is_read_not_recomputed(); passed++;
        test_out_cleared_on_false_after_reuse(); passed++;
    } catch (const std::exception& e) {
        std::cout << "\n\xE2\x9C\x97 Test failed with exception: " << e.what() << std::endl;
    }

    std::cout << "\n==========================================" << std::endl;
    std::cout << "Results: " << passed << "/" << total << " tests passed" << std::endl;
    std::cout << "==========================================" << std::endl;

    return (passed == total) ? 0 : 1;
}

