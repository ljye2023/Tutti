// csrc/resolvers/local_file/raid0_resolver.h
//
// A LocalFileResolver for a filesystem on an md RAID0 over NVMe namespaces.
// Shared by the config-driven resolver factory and the preset assembly so the
// two cannot drift: both accept exactly the arrays md_raid0::probe() accepts
// and both map md slots to namespaces by block device number.

#pragma once

#include "csrc/common/md_raid0.h"
#include "csrc/resolvers/local_file/resolver.h"

#include <sys/stat.h>

#include <memory>
#include <string>
#include <vector>

namespace tutti::resolvers::local_file {

struct Raid0MemberNamespace {
    std::string block_path;  // e.g. /dev/snvme0n1
    std::string pci_bdf;
    std::uint32_t namespace_id = 1;
    std::uint32_t block_size = 4096;
};

// nullptr with *why set when `mount` is not a usable md RAID0 whose members
// are exactly `namespaces` (any order).
inline std::unique_ptr<LocalFileResolver> make_raid0_resolver(
    const std::string& mount,
    const std::vector<Raid0MemberNamespace>& namespaces,
    const std::string& data_path_key,
    std::string* why) {
    namespace md = tutti::detail::md_raid0;
    namespace payload = tutti::payloads::ext4_local_nvme;
    auto fail = [&](std::string message) {
        if (why) *why = std::move(message);
        return std::unique_ptr<LocalFileResolver>();
    };
    if (namespaces.size() < 2) return fail("RAID0 needs at least two namespaces");
    md::Geometry geometry;
    std::string probe_error;
    if (!md::probe_path(mount, &geometry, &probe_error)) {
        return fail(mount + " is not a usable md RAID0: " + probe_error);
    }
    if (geometry.members.size() != namespaces.size()) {
        return fail(geometry.md_name + " has " +
                    std::to_string(geometry.members.size()) + " members but " +
                    std::to_string(namespaces.size()) +
                    " NVMe namespaces are configured on " + mount);
    }
    auto layout = std::make_shared<payload::Raid0Layout>();
    layout->chunk_bytes = geometry.chunk_bytes;
    layout->member_data_bytes = geometry.member_data_bytes;
    std::vector<bool> used(namespaces.size(), false);
    for (const md::Member& member : geometry.members) {
        int match = -1;
        for (std::size_t i = 0; i < namespaces.size(); ++i) {
            struct stat st {};
            if (used[i] || ::stat(namespaces[i].block_path.c_str(), &st) != 0)
                continue;
            if (S_ISBLK(st.st_mode) && st.st_rdev == member.rdev) {
                match = static_cast<int>(i);
                break;
            }
        }
        if (match < 0) {
            return fail(geometry.md_name + " member " + member.block_name +
                        " is not one of the configured NVMe namespaces");
        }
        used[static_cast<std::size_t>(match)] = true;
        const auto& ns = namespaces[static_cast<std::size_t>(match)];
        if (ns.block_size != namespaces.front().block_size) {
            return fail("RAID0 members differ in logical block size");
        }
        layout->members.push_back(
            {{ns.pci_bdf, ns.namespace_id, ns.block_size},
             member.data_offset_bytes});
    }
    return std::make_unique<LocalFileResolver>(
        geometry.md_name, 0, namespaces.front().block_size,
        BackingDeviceConfig{"/dev/" + geometry.md_name, 0},
        kFiemapMaxExtentsPerCall, data_path_key, std::move(layout));
}

}  // namespace tutti::resolvers::local_file
