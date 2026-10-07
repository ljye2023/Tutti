#include "mount_manager.h"

#include "tutti_verbose.h"

#include "csrc/common/md_raid0.h"

#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mntent.h>
#include <sys/sysmacros.h>   // major()/minor(): device identity for R3's check
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace nvmeservice {

namespace {

// Read /proc/<pid>/comm (process name, max 15 chars + NUL).
std::string read_comm(uint32_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/comm";
    std::ifstream f(path);
    std::string comm;
    if (f.is_open()) {
        std::getline(f, comm);
    }
    if (comm.empty()) comm = "?";
    return comm;
}

// Check if a string starts with a prefix.
bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

// Check if a path is a prefix of another path (mount_path prefix match).
// e.g. "/mnt/nvme1" is a prefix of "/mnt/nvme1/foo" but NOT of "/mnt/nvme10".
bool path_is_prefix(const std::string& path, const std::string& prefix) {
    if (!starts_with(path, prefix)) return false;
    // Ensure the char after the prefix is '/' or end-of-string, so
    // "/mnt/nvme1" doesn't match "/mnt/nvme10".
    if (path.size() == prefix.size()) return true;
    return path[prefix.size()] == '/';
}

std::string decode_mountinfo_escapes(const std::string& field) {
    // \040 = space, \011 = tab, \012 = newline, \134 = backslash.
    std::string out;
    out.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] == '0') {
            const int hi = field[i + 2] - '0';
            const int lo = field[i + 3] - '0';
            if (hi >= 0 && hi <= 7 && lo >= 0 && lo <= 7) {
                out += static_cast<char>(hi * 8 + lo);
                i += 3;
                continue;
            }
        }
        out += field[i];
    }
    return out;
}

bool parse_major_minor(const std::string& field, unsigned long* major_out,
                       unsigned long* minor_out) {
    const size_t colon = field.find(':');
    if (colon == std::string::npos) return false;
    try {
        *major_out = std::stoul(field.substr(0, colon));
        *minor_out = std::stoul(field.substr(colon + 1));
    } catch (...) {
        return false;
    }
    return true;
}

// Parses one /proc/self/mountinfo line into the fields this file needs.
//
// Field layout (proc(5)): mount_id parent_id major:minor root mount_point
// mount_options [optional fields...] - fs_type source super_options
//
// Parsed as tokens rather than by positional scanning because the optional
// fields between mount_options and the "-" separator are variable in count:
// the previous approach walked four spaces and assumed the next token was the
// mount point, which held only because nothing after it mattered.
bool parse_mountinfo_line(const std::string& line, MountEntry* out) {
    if (out == nullptr) return false;
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t space = line.find(' ', pos);
        if (space == std::string::npos) {
            tokens.push_back(line.substr(pos));
            break;
        }
        if (space > pos) tokens.push_back(line.substr(pos, space - pos));
        pos = space + 1;
    }
    // minimum: id parent dev root mp opts - fstype source superopts
    if (tokens.size() < 10) return false;

    if (!parse_major_minor(tokens[2], &out->major, &out->minor)) return false;
    out->mount_point = decode_mountinfo_escapes(tokens[4]);

    // The separator is a lone "-" token; everything after it is fs_type,
    // source, super_options.
    size_t sep = 0;
    bool found_sep = false;
    for (size_t i = 6; i + 2 < tokens.size(); ++i) {
        if (tokens[i] == "-") { sep = i; found_sep = true; break; }
    }
    if (!found_sep) return false;
    out->fs_type = tokens[sep + 1];
    out->source = tokens[sep + 2];
    return true;
}

// Runs argv[0] (PATH lookup) without a shell; returns the exit status, or -1
// when it could not be started.
int run_program(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (::posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Filesystem on `block_device` from its superblock magic: "ext4" (ext2/3/4,
// 0xEF53 at byte 1080) or "xfs" ("XFSB" at byte 0); "" for anything else, so
// the caller fails closed instead of mounting an unknown filesystem.
std::string detect_fs_type(const std::string& block_device) {
    const int fd = ::open(block_device.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0) return "";
    void* buf = nullptr;
    std::string type;
    if (::posix_memalign(&buf, 4096, 4096) == 0 &&
        ::pread(fd, buf, 4096, 0) == 4096) {
        const auto* b = static_cast<const unsigned char*>(buf);
        if (std::memcmp(b, "XFSB", 4) == 0) {
            type = "xfs";
        } else if (b[1080] == 0x53 && b[1081] == 0xEF) {
            type = "ext4";
        }
    }
    std::free(buf);
    ::close(fd);
    return type;
}

// The md array (e.g. "md127") holding block device `path`, "" when none.
std::string md_holder(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) return "";
    const std::string dir = "/sys/dev/block/" + std::to_string(major(st.st_rdev)) +
                            ":" + std::to_string(minor(st.st_rdev)) + "/holders";
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (starts_with(name, "md")) return name;
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// md RAID0
// ---------------------------------------------------------------------------

std::string MountManager::assemble_raid0(const std::string& name,
                                         const std::vector<std::string>& members,
                                         std::string* error) {
    namespace md = tutti::detail::md_raid0;
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return std::string();
    };
    // The member block devices appeared moments ago; let udev finish (its
    // md rule may be assembling the array right now).
    (void)run_program({"udevadm", "settle", "--timeout=10"});

    auto common_holder = [&](std::string* holder) {
        *holder = md_holder(members.front());
        for (const auto& m : members) {
            if (md_holder(m) != *holder) return false;
        }
        return true;
    };
    std::string holder;
    if (!common_holder(&holder)) {
        return fail("members of " + name + " are held by different md arrays");
    }
    if (!holder.empty()) {
        std::string state;
        std::ifstream f("/sys/block/" + holder + "/md/array_state");
        std::getline(f, state);
        if (state == "inactive") {
            // A partial incremental assembly; start over from the members.
            (void)run_program({"mdadm", "--stop", "/dev/" + holder});
            holder.clear();
        }
    }
    if (holder.empty()) {
        std::vector<std::string> args = {"mdadm", "--assemble", "/dev/md/" + name,
                                         "--run"};
        args.insert(args.end(), members.begin(), members.end());
        const int rc = run_program(args);
        (void)run_program({"udevadm", "settle", "--timeout=10"});
        if (rc != 0 || !common_holder(&holder) || holder.empty()) {
            return fail("mdadm --assemble " + name + " failed (rc=" +
                        std::to_string(rc) + "); if the members carry no md "
                        "superblock yet, create the array once with "
                        "scripts/tutti-raid0-create.sh");
        }
        TUTTI_INFO("mount_manager: assembled %s as /dev/%s\n", name.c_str(),
                   holder.c_str());
    } else {
        TUTTI_INFO("mount_manager: adopted %s already assembled as /dev/%s\n",
                   name.c_str(), holder.c_str());
    }
    const std::string device = "/dev/" + holder;
    owned_arrays_.push_back(device);

    // Same acceptance rule the clients' resolver applies: single-zone raid0
    // over exactly these members, nothing else.
    struct stat st {};
    md::Geometry geometry;
    std::string why;
    if (::stat(device.c_str(), &st) != 0 ||
        !md::probe(st.st_rdev, &geometry, &why)) {
        return fail(device + " is not a usable RAID0: " + why);
    }
    if (geometry.members.size() != members.size()) {
        return fail(device + " has " + std::to_string(geometry.members.size()) +
                    " members, configured " + std::to_string(members.size()));
    }
    return device;
}

bool MountManager::adopt_mount(const std::string& block_device,
                               const std::string& mount_path) {
    MountEntry entry;
    struct stat st {};
    if (!lookup_mount(mount_path, &entry) || ::stat(block_device.c_str(), &st) != 0) {
        return false;
    }
    const std::string fs_type = detect_fs_type(block_device);
    if (fs_type.empty() ||
        !existing_mount_acceptable(entry, fs_type, major(st.st_rdev),
                                   minor(st.st_rdev), nullptr)) {
        return false;
    }
    for (const auto& owned : owned_mounts_) {
        if (owned.mount_path == mount_path) return true;
    }
    owned_mounts_.push_back({block_device, mount_path});
    TUTTI_INFO("mount_manager: %s at %s was mounted by someone else; taking "
               "ownership (the daemon stops this array)\n",
               block_device.c_str(), mount_path.c_str());
    return true;
}

int MountManager::stop_arrays() {
    int failed = 0;
    for (auto it = owned_arrays_.rbegin(); it != owned_arrays_.rend(); ++it) {
        if (run_program({"mdadm", "--stop", *it}) == 0) {
            TUTTI_INFO("mount_manager: stopped %s\n", it->c_str());
        } else {
            std::fprintf(stderr, "mount_manager: mdadm --stop %s failed\n",
                         it->c_str());
            ++failed;
        }
    }
    owned_arrays_.clear();
    return failed;
}

// ---------------------------------------------------------------------------
// Existing-mount validation
// ---------------------------------------------------------------------------

bool existing_mount_acceptable(const MountEntry& entry,
                               const std::string& expected_fs_type,
                               unsigned long expected_major,
                               unsigned long expected_minor,
                               std::string* reason) {
    const auto fail = [reason](std::string message) {
        if (reason != nullptr) *reason = std::move(message);
        return false;
    };

    if (entry.fs_type != expected_fs_type) {
        return fail("is mounted as '" + entry.fs_type + "', expected '" +
                    expected_fs_type + "'");
    }
    // Device identity, not path text: two paths can name the same device and
    // one path can be re-pointed at a different device between runs. major:minor
    // is what the kernel actually mounted.
    if (entry.major != expected_major || entry.minor != expected_minor) {
        return fail("is mounted from device " + std::to_string(entry.major) +
                    ":" + std::to_string(entry.minor) + " (source '" +
                    entry.source + "'), expected " +
                    std::to_string(expected_major) + ":" +
                    std::to_string(expected_minor));
    }
    return true;
}

// Looks up the mountinfo entry for an exact mount point.
bool MountManager::lookup_mount(const std::string& mount_path, MountEntry* out) {
    std::ifstream f("/proc/self/mountinfo");
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        MountEntry entry;
        if (!parse_mountinfo_line(line, &entry)) continue;
        if (entry.mount_point == mount_path) {
            if (out != nullptr) *out = entry;
            return true;
        }
    }
    return false;
}

MountManager::MountManager(const UnmountRetryConfig& retry_cfg)
    : retry_cfg_(retry_cfg) {}

bool MountManager::is_mounted(const std::string& mount_path) {
    // Shares the tokenising parser with lookup_mount() rather than keeping a
    // second, subtly different one. The old version scanned forward four
    // spaces and took the next token as the mount point, which only worked
    // because it never needed anything after it. (Round 17 S1 introduced it
    // when /proc/self/mountinfo replaced /etc/mtab.)
    return lookup_mount(mount_path, nullptr);
}

MountResult MountManager::mount_one(const std::string& block_device,
                                    const std::string& mount_path) {
    MountResult res;
    res.block_device = block_device;

    // 1. Create the mount point, including configured parent directories.
    // ServiceState publishes ACCEL<n> view targets only after mount(2), so
    // backing mount-point preparation belongs entirely to MountManager.
    std::error_code ec;
    std::filesystem::create_directories(mount_path, ec);
    if (ec) {
        res.error = "mkdir " + mount_path + " failed: " + ec.message();
        return res;
    }

    // "" when the device holds neither; an existing mount then never matches
    // and mount(2) below is refused.
    const std::string fs_type = detect_fs_type(block_device);

    struct stat st;
    if (::stat(mount_path.c_str(), &st) != 0) {
        res.error = "stat " + mount_path + " failed: " +
                    std::strerror(errno);
        return res;
    }
    if (!S_ISDIR(st.st_mode)) {
        res.error = mount_path + " is not a directory";
        return res;
    }

    // 2. If already mounted (by a previous operator or daemon), adopt it ONLY
    //    if it is the mount we asked for.
    //
    //    Accepting any pre-existing mount was the bug: the daemon goes on to
    //    publish accelerator views on that filesystem and hand its extents to
    //    the storage path, so a tmpfs, an overlay, or a stale mount from a
    //    previous layout would be silently treated as the target NVMe. Fail
    //    closed instead -- an operator can then look at the path and decide.
    if (is_mounted(mount_path)) {
        MountEntry entry;
        if (!lookup_mount(mount_path, &entry)) {
            // is_mounted() said yes using the same parser, so this is a race
            // (unmounted in between). Treat as not mounted.
            TUTTI_INFO("mount_manager: %s disappeared between checks\n",
                       mount_path.c_str());
        } else {
            // Device numbers of the device we were asked to mount. A failed
            // stat is itself disqualifying: we cannot prove the existing mount
            // is ours, and guessing yes is what this check exists to stop.
            struct stat dev_st;
            if (::stat(block_device.c_str(), &dev_st) != 0) {
                res.error = "stat " + block_device + " failed: " +
                            std::strerror(errno) +
                            " (cannot verify existing mount at " + mount_path + ")";
                return res;
            }
            std::string reason;
            if (!existing_mount_acceptable(entry, fs_type,
                                           major(dev_st.st_rdev),
                                           minor(dev_st.st_rdev), &reason)) {
                res.error = mount_path + " already exists but " + reason +
                            "; refusing to adopt it. Unmount it or point the "
                            "configuration elsewhere.";
                TUTTI_INFO("mount_manager: %s\n", res.error.c_str());
                return res;
            }
            res.already_mounted = true;
            TUTTI_INFO("mount_manager: %s already mounted at %s from %s "
                       "(verified, not taking ownership)\n",
                       block_device.c_str(), mount_path.c_str(),
                       entry.source.c_str());
            return res;
        }
    }

    // 3. mount(2) — the detected filesystem, default options plus noatime.
    if (fs_type.empty()) {
        res.error = block_device + " holds no ext4 or xfs filesystem";
        TUTTI_INFO("mount_manager: %s (continuing without mount)\n", res.error.c_str());
        return res;
    }
    int rc = ::mount(block_device.c_str(), mount_path.c_str(), fs_type.c_str(),
                     MS_NOATIME, nullptr);
    if (rc != 0) {
        res.error = "mount(" + block_device + ", " + mount_path + ", " +
                    fs_type + ") failed: " + std::strerror(errno);
        TUTTI_INFO("mount_manager: %s (continuing without mount)\n", res.error.c_str());
        return res;
    }

    // 4. Record ownership.
    res.mounted_by_daemon = true;
    owned_mounts_.push_back({block_device, mount_path});
    TUTTI_INFO("mount_manager: mounted %s at %s (owned)\n",
               block_device.c_str(), mount_path.c_str());
    return res;
}

int MountManager::try_umount_(const std::string& mount_path) {
    // MNT_FORCE would corrupt data; never use it here.  Use plain umount2.
    // If the caller wants a lazy unmount (MNT_DETACH), they can add a flag
    // later; for now plain umount2(0).
    int rc = ::umount2(mount_path.c_str(), 0);
    if (rc == 0) return 0;
    return errno;
}

std::vector<MountHolder> MountManager::scan_holders(const std::string& mount_path) {
    std::vector<MountHolder> holders;
    DIR* proc = ::opendir("/proc");
    if (!proc) return holders;

    struct dirent* de;
    while ((de = ::readdir(proc)) != nullptr) {
        // Only numeric entries are PIDs.
        bool is_pid = true;
        for (const char* p = de->d_name; *p; ++p) {
            if (*p < '0' || *p > '9') { is_pid = false; break; }
        }
        if (!is_pid) continue;

        uint32_t pid = static_cast<uint32_t>(std::strtoul(de->d_name, nullptr, 10));
        if (pid == 0) continue;

        std::string comm = read_comm(pid);

        // --- Check /proc/<pid>/fd/ for open files on the mount ---
        {
            std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
            DIR* fdp = ::opendir(fd_dir.c_str());
            if (fdp) {
                struct dirent* fde;
                while ((fde = ::readdir(fdp)) != nullptr) {
                    if (fde->d_name[0] == '.') continue;
                    std::string link_path = fd_dir + "/" + fde->d_name;
                    char buf[4096];
                    ssize_t n = ::readlink(link_path.c_str(), buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        std::string target(buf);
                        if (path_is_prefix(target, mount_path)) {
                            holders.push_back({pid, comm, "fd",
                                               "fd=" + std::string(fde->d_name) +
                                               " -> " + target});
                        }
                    }
                }
                ::closedir(fdp);
            }
        }

        // --- Check /proc/<pid>/cwd ---
        {
            std::string cwd_link = "/proc/" + std::to_string(pid) + "/cwd";
            char buf[4096];
            ssize_t n = ::readlink(cwd_link.c_str(), buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                std::string target(buf);
                if (path_is_prefix(target, mount_path)) {
                    holders.push_back({pid, comm, "cwd", target});
                }
            }
        }

        // --- Check /proc/<pid>/maps for mmap'd files on the mount ---
        {
            std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
            std::ifstream mf(maps_path);
            std::string line;
            while (std::getline(mf, line)) {
                // maps lines: addr perms offset dev inode pathname
                // The pathname is the last whitespace-separated field
                // if it exists.  Check if it starts with mount_path.
                size_t last_space = line.find_last_of(' ');
                if (last_space == std::string::npos) continue;
                std::string pathname = line.substr(last_space + 1);
                if (path_is_prefix(pathname, mount_path)) {
                    holders.push_back({pid, comm, "maps", pathname});
                    break;  // one entry per pid for maps is enough
                }
            }
        }
    }
    ::closedir(proc);
    return holders;
}

void MountManager::report_holders_(const std::string& mount_path,
                                    const std::vector<MountHolder>& holders) {
    if (holders.empty()) {
        std::fprintf(stderr,
                     "mount_manager: %s busy but no holders found in /proc\n",
                     mount_path.c_str());
        return;
    }
    std::fprintf(stderr,
                 "mount_manager: %s busy — %zu holder(s):\n",
                 mount_path.c_str(), holders.size());
    for (const auto& h : holders) {
        std::fprintf(stderr,
                     "  PID=%-8u  comm=%-16s  type=%-5s  %s\n",
                     h.pid, h.comm.c_str(), h.holder_type.c_str(),
                     h.detail.c_str());
    }
    std::fprintf(stderr,
                 "  (Close these processes or their files, then the daemon "
                 "will retry. Send SIGTERM again to force-exit without unmounting.)\n");
    std::fflush(stderr);
}

int MountManager::unmount_all() {
    int remaining = 0;

    for (const auto& m : owned_mounts_) {
        bool unmounted = false;
        for (uint32_t attempt = 0; attempt < retry_cfg_.max; ++attempt) {
            if (force_exit_requested()) {
                std::fprintf(stderr,
                             "mount_manager: force-exit requested; "
                             "leaving %s mounted\n",
                             m.mount_path.c_str());
                ++remaining;
                unmounted = true;  // stop retrying this one
                break;
            }

            int err = try_umount_(m.mount_path);
            if (err == 0) {
                TUTTI_INFO("mount_manager: unmounted %s\n", m.mount_path.c_str());
                unmounted = true;
                break;
            }

            if (err == EINVAL) {
                // Not a mount point anymore (already gone or never was).
                TUTTI_INFO("mount_manager: %s not a mount point (EINVAL), skipping\n",
                           m.mount_path.c_str());
                unmounted = true;
                break;
            }

            if (err == EBUSY) {
                auto holders = scan_holders(m.mount_path);
                report_holders_(m.mount_path, holders);
                if (attempt + 1 < retry_cfg_.max) {
                    std::fprintf(stderr,
                                 "mount_manager: retrying %s in %u ms "
                                 "(attempt %u/%u)\n",
                                 m.mount_path.c_str(), retry_cfg_.interval_ms,
                                 attempt + 2, retry_cfg_.max);
                    std::fflush(stderr);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(retry_cfg_.interval_ms));
                }
            } else {
                // Other errno — log and skip.
                std::fprintf(stderr,
                             "mount_manager: umount %s failed: %s\n",
                             m.mount_path.c_str(), std::strerror(err));
                unmounted = true;  // can't do anything about it
                break;
            }
        }

        if (!unmounted) {
            std::fprintf(stderr,
                         "mount_manager: %s still busy after %u retries; "
                         "leaving mounted\n",
                         m.mount_path.c_str(), retry_cfg_.max);
            ++remaining;
        }
    }

    return remaining;
}

} // namespace nvmeservice
