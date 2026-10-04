// csrc/storage_objects/slot_placement_policy.cpp

#include "csrc/storage_objects/slot_placement_policy.h"

namespace tutti::storage_objects {
namespace {

std::string join(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    if (dir.back() == '/') return dir + leaf;
    return dir + "/" + leaf;
}

} // namespace

FixedSegmentFilePlacement::FixedSegmentFilePlacement(
    std::vector<std::string> mounts, std::string subdir,
    std::uint64_t slot_bytes, std::uint64_t file_bytes,
    std::uint64_t header_bytes)
    : mounts_(std::move(mounts)),
      subdir_(std::move(subdir)),
      slot_bytes_(slot_bytes),
      file_bytes_(file_bytes),
      header_bytes_(header_bytes),
      slots_per_file_(slot_bytes == 0 ? 0 : file_bytes / slot_bytes) {}

// Files are named by number, not by key: binding a key to a slot is a metadata
// event (header plus checkpoint), so a slot's path is stable across reuse and
// path-keyed caches upstream never go cold.
std::string FixedSegmentFilePlacement::path_for_slot(std::uint64_t slot) const {
    const std::uint64_t device = slot % mounts_.size();
    const std::uint64_t file_id = (slot / mounts_.size()) / slots_per_file_;
    return join(join(mounts_[device], subdir_), std::to_string(file_id) + ".seg");
}

std::string FixedSegmentFilePlacement::uri_for_slot(std::uint64_t slot) const {
    return "file://" + path_for_slot(slot);
}

std::uint64_t FixedSegmentFilePlacement::header_offset_for_slot(
    std::uint64_t slot) const {
    return (slot / mounts_.size()) % slots_per_file_ * slot_bytes_;
}

std::uint64_t FixedSegmentFilePlacement::payload_offset_for_slot(
    std::uint64_t slot) const {
    return header_offset_for_slot(slot) + header_bytes_;
}

} // namespace tutti::storage_objects
