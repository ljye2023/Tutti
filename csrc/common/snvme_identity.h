// csrc/common/snvme_identity.h
//
// Which PCI controller does an snvme character device drive?
//
// The character device number (/dev/ssnvmeN, snvm_chrdev_minor_ida) and the
// controller/block number (snvmeM, /dev/snvmeMn1, nvme_instance_ida) come
// from two unrelated kernel allocators. They coincide only when every
// controller is brought up in one pass in order; with several daemons, or
// after controllers were released and re-bound, they differ. Deriving one
// from the other sent GPU IO for one array to the members of another
// (2026-10-07: rank 0-3 KV landed on md0's members).
//
// The only authoritative link is the kernel's: NVM_GET_DEV_INFO on the
// character device returns the namespace's block disk name, whose sysfs
// device is the PCI function. These helpers use exactly that.

#pragma once

#include "csrc/include/uapi/tutti_snvme.h"

#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>
#include <string>

namespace tutti::detail::snvme_identity {

// PCI address ("0000:08:00.0") of the controller behind character device
// `chrdev`, or "" if it cannot be established.
inline std::string pci_of_chrdev(const std::string& chrdev) {
    const int fd = ::open(chrdev.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return "";
    struct nvm_ioctl_dev info;
    std::memset(&info, 0, sizeof(info));
    const int rc = ::ioctl(fd, NVM_GET_DEV_INFO, &info);
    ::close(fd);
    if (rc < 0 || info.abi_version != TUTTI_SNVME_ABI_VERSION) return "";
    char disk[DISK_NAME_LEN + 1] = {};
    std::memcpy(disk, info.disk_name, DISK_NAME_LEN);
    if (disk[0] == '\0' || std::strchr(disk, '/') != nullptr) return "";
    // /sys/block/<disk>/device -> controller (snvmeM); its device -> PCI.
    char resolved[PATH_MAX];
    const std::string link = std::string("/sys/block/") + disk + "/device/device";
    if (::realpath(link.c_str(), resolved) == nullptr) return "";
    const char* base = std::strrchr(resolved, '/');
    return base ? std::string(base + 1) : std::string();
}

// The /dev/ssnvme* character device whose controller is `pci_addr`, or ""
// (none, or more than one -- never guess).
inline std::string chrdev_for_pci(const std::string& pci_addr) {
    glob_t g{};
    if (::glob("/dev/ssnvme*", 0, nullptr, &g) != 0) return "";
    std::string found;
    int matches = 0;
    for (std::size_t i = 0; i < g.gl_pathc; ++i) {
        if (pci_of_chrdev(g.gl_pathv[i]) == pci_addr) {
            found = g.gl_pathv[i];
            ++matches;
        }
    }
    ::globfree(&g);
    return matches == 1 ? found : std::string();
}

}  // namespace tutti::detail::snvme_identity
