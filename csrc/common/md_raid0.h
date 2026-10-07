#pragma once

// md RAID0 geometry from sysfs.
//
// Used where a filesystem may sit on an md RAID0 built over NVMe namespaces:
// the resolver needs the geometry for the second address hop (md offset ->
// member offset, see payloads/ext4_local_nvme/payload.h Raid0Layout) and the
// daemon checks an array it assembled against the same rules.
//
// Only the layout the kernel maps with plain arithmetic is accepted: raid0,
// one strip zone (all members the same chunk-rounded size), no reshape, the
// filesystem directly on the md device (not a partition of it). Everything
// else is reported as not usable, never guessed at.

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace tutti::detail::md_raid0 {

struct Member {
    std::string block_name;               // e.g. "snvme0n1"
    dev_t rdev = 0;                       // member block device number
    std::uint64_t data_offset_bytes = 0;  // md dev-*/offset
};

struct Geometry {
    std::string md_name;                  // e.g. "md127"
    std::uint64_t chunk_bytes = 0;
    std::uint64_t member_data_bytes = 0;  // per member, chunk-rounded
    std::vector<Member> members;          // index == md slot (rdN)
};

namespace detail {

inline bool read_line(const std::string& path, std::string* out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::getline(f, *out);
    while (!out->empty() && (out->back() == '\n' || out->back() == ' '))
        out->pop_back();
    return true;
}

inline bool read_u64(const std::string& path, std::uint64_t* out) {
    std::string s;
    if (!read_line(path, &s) || s.empty()) return false;
    try {
        std::size_t used = 0;
        *out = std::stoull(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

inline bool parse_dev(const std::string& s, dev_t* out) {
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos) return false;
    try {
        *out = makedev(std::stoul(s.substr(0, colon)),
                       std::stoul(s.substr(colon + 1)));
    } catch (...) {
        return false;
    }
    return true;
}

inline bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

}  // namespace detail

// Probes the block device `dev`. Returns true with `out` filled when it is a
// usable md RAID0. `is_md` (optional) tells "not md at all" apart from "md
// but unusable": the first lets callers fall back to the single-device path,
// the second must fail closed.
inline bool probe(dev_t dev, Geometry* out, std::string* error,
                  bool* is_md = nullptr) {
    using detail::fail;
    if (is_md) *is_md = false;
    const std::string sys = "/sys/dev/block/" + std::to_string(major(dev)) +
                            ":" + std::to_string(minor(dev));
    if (::access((sys + "/partition").c_str(), F_OK) == 0) {
        if (::access((sys + "/../md").c_str(), F_OK) == 0) {
            if (is_md) *is_md = true;
            return fail(error, "filesystem is on a partition of an md array; "
                               "make it directly on the md device");
        }
        return fail(error, "not an md device");
    }
    if (::access((sys + "/md").c_str(), F_OK) != 0) {
        return fail(error, "not an md device");
    }
    if (is_md) *is_md = true;

    Geometry g;
    {
        char buf[4096];
        const ssize_t n = ::readlink(sys.c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) return fail(error, "cannot resolve " + sys);
        buf[n] = '\0';
        const std::string link(buf);
        g.md_name = link.substr(link.find_last_of('/') + 1);
    }
    const std::string md = sys + "/md/";

    std::string level, state, reshape;
    if (!detail::read_line(md + "level", &level) || level != "raid0") {
        return fail(error, g.md_name + " is '" + level + "', only raid0 is "
                           "supported (direct member IO is only consistent "
                           "without parity/mirrors)");
    }
    if (!detail::read_line(md + "array_state", &state) ||
        (state != "clean" && state != "active" && state != "active-idle")) {
        return fail(error, g.md_name + " array_state is '" + state + "'");
    }
    if (detail::read_line(md + "reshape_position", &reshape) &&
        reshape != "none") {
        return fail(error, g.md_name + " is reshaping");
    }

    std::uint64_t raid_disks = 0, array_sectors = 0;
    if (!detail::read_u64(md + "chunk_size", &g.chunk_bytes) ||
        g.chunk_bytes == 0 || g.chunk_bytes % 4096 != 0) {
        return fail(error, g.md_name + " has an unusable chunk_size");
    }
    if (!detail::read_u64(md + "raid_disks", &raid_disks) || raid_disks < 2) {
        return fail(error, g.md_name + " needs at least two members");
    }
    if (!detail::read_u64(sys + "/size", &array_sectors)) {
        return fail(error, "cannot read " + g.md_name + " size");
    }

    for (std::uint64_t slot = 0; slot < raid_disks; ++slot) {
        char buf[256];
        const std::string rd = md + "rd" + std::to_string(slot);
        const ssize_t n = ::readlink(rd.c_str(), buf, sizeof(buf) - 1);
        if (n <= 0) return fail(error, g.md_name + " slot " +
                                       std::to_string(slot) + " is missing");
        buf[n] = '\0';
        const std::string dev_dir = md + buf;  // ".../md/dev-snvme0n1"
        Member m;
        m.block_name = std::string(buf).substr(4);  // strip "dev-"
        std::string devno;
        std::uint64_t offset_sectors = 0, size_kib = 0;
        if (!detail::read_line(dev_dir + "/block/dev", &devno) ||
            !detail::parse_dev(devno, &m.rdev) ||
            !detail::read_u64(dev_dir + "/offset", &offset_sectors) ||
            !detail::read_u64(dev_dir + "/size", &size_kib)) {
            return fail(error, "cannot read member " + m.block_name);
        }
        m.data_offset_bytes = offset_sectors * 512;
        const std::uint64_t rounded =
            size_kib * 1024 / g.chunk_bytes * g.chunk_bytes;
        if (slot == 0) {
            g.member_data_bytes = rounded;
        } else if (rounded != g.member_data_bytes) {
            return fail(error, g.md_name + " members differ in size (more "
                               "than one raid0 strip zone)");
        }
        g.members.push_back(std::move(m));
    }
    if (g.member_data_bytes == 0 ||
        array_sectors * 512 != g.member_data_bytes * raid_disks) {
        return fail(error, g.md_name + " size does not match a single strip "
                                       "zone of its members");
    }
    if (out) *out = std::move(g);
    return true;
}

// Probes the filesystem holding `path` (stat(path).st_dev).
inline bool probe_path(const std::string& path, Geometry* out,
                       std::string* error, bool* is_md = nullptr) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (is_md) *is_md = false;
        return detail::fail(error, "cannot stat " + path);
    }
    return probe(st.st_dev, out, error, is_md);
}

}  // namespace tutti::detail::md_raid0
