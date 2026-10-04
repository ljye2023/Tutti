#pragma once

// csrc/storage_objects/slot_placement_policy.h -- slot number -> media location.
//
// IMPLEMENTATION DETAIL. Reached only through the SPI.
//
// Placement is a pure function of the slot number. Slots rotate over the
// mounts (device = slot % N); each device packs a fixed number of its slots
// into one segment file <mount>/<subdir>/<file_id>.seg. A slot occupies one
// contiguous region of that file: the object header at the slot offset, the
// payload after the reserved prefix. An IO never spans disks.
//
// The path is the one the resolver maps (the URI carries the same path), so
// materialisation, header IO and DMA all address the same file.

#include <cstdint>
#include <string>
#include <vector>

#include <tutti/spi/storage_object_store.h>

namespace tutti::storage_objects {

class FixedSegmentFilePlacement final {
public:
    FixedSegmentFilePlacement(std::vector<std::string> mounts,
                              std::string subdir,
                              std::uint64_t slot_bytes,
                              std::uint64_t file_bytes,
                              std::uint64_t header_bytes = ObjectHeaderLayout::kHeaderBytes);

    bool geometry_valid() const { return !mounts_.empty() && slots_per_file_ > 0; }
    std::uint64_t file_bytes() const { return file_bytes_; }

    // Filesystem path of the segment file holding `slot`.
    std::string path_for_slot(std::uint64_t slot) const;
    // URI for the runtime's local-file resolver: file://<path_for_slot>.
    std::string uri_for_slot(std::uint64_t slot) const;
    // Offset of the slot's object header within its segment file.
    std::uint64_t header_offset_for_slot(std::uint64_t slot) const;
    // Offset of the slot's payload (segment 0) within its segment file.
    std::uint64_t payload_offset_for_slot(std::uint64_t slot) const;

private:
    std::vector<std::string> mounts_;
    std::string subdir_;
    std::uint64_t slot_bytes_ = 0;
    std::uint64_t file_bytes_ = 0;
    std::uint64_t header_bytes_ = ObjectHeaderLayout::kHeaderBytes;
    std::uint64_t slots_per_file_ = 0;
};

} // namespace tutti::storage_objects
