// nvme_real_hw_test.cpp -- Layer 2 NVMe real-hardware integration test.
//
// Unlike the vendor-neutral suite in tutti/tests/device_manager/ (which drives
// mock drivers), this test exercises the *real* NVMe path end-to-end against a
// physical controller (default 0000:b1:00.0):
//
//   daemon (B3 owner: chrdev_create + kernel_ioq_cap + SNVM_DEVICE_BIND + probe)
//     -> gRPC Connect  -> libnvm nvm_ctrl_attach_client
//     -> nvm_create_group -> nvm_add_user_queue -> GPU-memory Write/Read/verify
//     -> nvm_destroy_group -> nvm_ctrl_free_client -> Disconnect
//
// The test does NOT link libnvm/CUDA/gRPC.  It shells out to the prebuilt
// nvmeservice_client example binary, which is the only in-tree driver of the
// full libnvm + GPU IO path.  This keeps the test's own link surface trivial
// and matches how the path is actually run in practice.
//
// The daemon needs root (PCI ownership + mount), so it is brought up
// out-of-band by run_real_hw_test.sh.  This test is the unprivileged client
// half and connects over the daemon's gRPC endpoint.
//
// Everything is GTEST_SKIP()'d unless the operator explicitly opts in, so the
// target is safe to leave in the default build / ctest run on hardware-less
// nodes:
//
//   TUTTI_NVME_REAL_HW=1        required for any tier to run
//   TUTTI_NVME_ALLOW_DESTRUCTIVE=1  required for the GPU-IO tier (writes data)
//
// Overridable knobs (env):
//   TUTTI_NVME_CLIENT_BIN   path to nvmeservice_client (default: build-time)
//   TUTTI_NVME_ENDPOINT     gRPC endpoint (default 127.0.0.1:50051)
//   TUTTI_NVME_DEVICE_ID    daemon device_id (default 0)
//   TUTTI_NVME_CUDA         CUDA device id (default 0)
//   TUTTI_NVME_PCI          expected PCI addr (default 0000:b1:00.0)

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

// ── env helpers ───────────────────────────────────────────────────────────

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

bool env_flag(const char* key) {
    const char* v = std::getenv(key);
    return v && std::string(v) == "1";
}

bool path_exists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }
bool path_x(const std::string& p)      { return ::access(p.c_str(), X_OK) == 0; }

// Resolve the client binary: env override wins, else the build-time define.
std::string client_bin() {
#ifdef TUTTI_NVME_CLIENT_BIN
    return env_or("TUTTI_NVME_CLIENT_BIN", TUTTI_NVME_CLIENT_BIN);
#else
    return env_or("TUTTI_NVME_CLIENT_BIN", "nvmeservice_client");
#endif
}

std::string endpoint()  { return env_or("TUTTI_NVME_ENDPOINT", "127.0.0.1:50051"); }
std::string device_id() { return env_or("TUTTI_NVME_DEVICE_ID", "0"); }
std::string cuda_id()   { return env_or("TUTTI_NVME_CUDA", "0"); }
std::string want_pci()  { return env_or("TUTTI_NVME_PCI", "0000:b1:00.0"); }

// ── run the client, capture combined stdout+stderr and exit code ──────────

struct ClientResult {
    int         exit_code = -1;
    bool        spawned   = false;   // false if popen itself failed
    std::string output;
};

ClientResult run_client(const std::string& args) {
    ClientResult r;
    // Combine streams: the grant banner is on stdout, the [OK]/[FAIL] IO steps
    // are on stderr; we want both for assertions and diagnostics.
    std::string cmd = client_bin() + " --endpoint " + endpoint() + " " + args + " 2>&1";

    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    r.spawned = true;

    std::array<char, 4096> buf;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        r.output.append(buf.data(), n);
    }
    int status = ::pclose(pipe);
    // Mirror the shell's convention: WEXITSTATUS on normal exit.
    if (status == -1)          r.exit_code = -1;
    else if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    else                        r.exit_code = 128;  // killed by signal
    return r;
}

// ── shared skip guard: called at the top of every tier ────────────────────
// Returns true if a precondition is missing (caller should GTEST_SKIP()).
// Writes the reason into `why`.

bool should_skip(std::string& why) {
    if (!env_flag("TUTTI_NVME_REAL_HW")) {
        why = "TUTTI_NVME_REAL_HW != 1 (real-hardware test opt-in not set)";
        return true;
    }
    if (!path_exists("/dev/snvm_control")) {
        why = "/dev/snvm_control absent (snvme kernel module not loaded)";
        return true;
    }
    const std::string pci_sysfs = "/sys/bus/pci/devices/" + want_pci();
    if (!path_exists(pci_sysfs)) {
        why = "PCI device " + want_pci() + " not present (" + pci_sysfs + ")";
        return true;
    }
    if (!path_x(client_bin())) {
        why = "client binary not executable: " + client_bin();
        return true;
    }
    return false;
}

// Probe: can we reach the daemon at all?  Uses --list-only (non-destructive).
// Returns true when the daemon answered with at least one device.
bool daemon_reachable(std::string& detail) {
    ClientResult r = run_client("--device " + device_id() + " --list-only");
    detail = r.output;
    if (!r.spawned)       { detail = "popen failed to spawn client"; return false; }
    if (r.exit_code != 0) return false;
    // The client prints "No devices returned. Is the daemon running?" on an
    // empty list; treat presence of "device_id=" as the positive signal.
    return r.output.find("device_id=") != std::string::npos;
}

}  // namespace

// ── Tier 0: ListDevices -- proves daemon B3 bring-up of the real controller ─
//
// Non-destructive, no GPU.  The daemon must already own 0000:b1:00.0; the
// client lists it and we assert the reported PCI matches the target and the
// namespace/block metadata look sane.

TEST(NvmeRealHw, ListDevicesReportsTargetPci) {
    std::string why;
    if (should_skip(why)) GTEST_SKIP() << why;

    std::string detail;
    if (!daemon_reachable(detail)) {
        GTEST_SKIP() << "daemon not reachable on " << endpoint()
                     << " (start it via run_real_hw_test.sh). Detail:\n" << detail;
    }

    ClientResult r = run_client("--device " + device_id() + " --list-only");
    ASSERT_TRUE(r.spawned) << "failed to spawn client " << client_bin();
    EXPECT_EQ(r.exit_code, 0) << "client --list-only output:\n" << r.output;

    // The daemon's ListDevices banner includes "pci=0000:b1:00.0".
    EXPECT_NE(r.output.find("pci=" + want_pci()), std::string::npos)
        << "expected pci=" << want_pci() << " in list output:\n" << r.output;
    // A real controller reports a namespace and a /dev/ssnvme* path.
    EXPECT_NE(r.output.find("ns="), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("snvme=/dev/ssnvme"), std::string::npos) << r.output;
}

// ── Tier 1: Connect + attach + create + destroy (no data IO) ───────────────
//
// Non-destructive.  Drives the real gRPC Connect grant plus the libnvm
// attach/create-group/destroy-group lifecycle via --skip-io.  This is the
// strongest assertion that does not write to the namespace or need an
// arch-matched GPU kernel.

TEST(NvmeRealHw, ConnectAttachSkipIoLifecycle) {
    std::string why;
    if (should_skip(why)) GTEST_SKIP() << why;

    std::string detail;
    if (!daemon_reachable(detail)) {
        GTEST_SKIP() << "daemon not reachable on " << endpoint()
                     << ". Detail:\n" << detail;
    }

    ClientResult r = run_client("--device " + device_id() +
                                " --cuda " + cuda_id() + " --skip-io");
    ASSERT_TRUE(r.spawned) << "failed to spawn client " << client_bin();
    EXPECT_EQ(r.exit_code, 0) << "client --skip-io output:\n" << r.output;

    // Grant banner + libnvm attach markers must appear.
    EXPECT_NE(r.output.find("granted_queues:"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("nvm_ctrl_attach_client"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("nvm_create_group"), std::string::npos) << r.output;
    EXPECT_NE(r.output.find("nvm_destroy_group"), std::string::npos) << r.output;
    // No IO step should have failed.
    EXPECT_EQ(r.output.find("[FAIL]"), std::string::npos)
        << "a libnvm step reported [FAIL]:\n" << r.output;
}

// ── Tier 2: full GPU-memory Write/Read/verify (DESTRUCTIVE) ─────────────────
//
// Writes/reads/verifies TEST_NR_IO blocks at LBA 2621440 (10 GiB offset) on
// the namespace.  Gated behind TUTTI_NVME_ALLOW_DESTRUCTIVE=1.  Requires the
// client to be built for the host GPU arch (L40S = sm_89); an arch mismatch
// surfaces as cudaErrorNoKernelImageForDevice, which we diagnose explicitly.

TEST(NvmeRealHw, GpuIoWriteReadVerifyDestructive) {
    std::string why;
    if (should_skip(why)) GTEST_SKIP() << why;

    if (!env_flag("TUTTI_NVME_ALLOW_DESTRUCTIVE")) {
        GTEST_SKIP() << "TUTTI_NVME_ALLOW_DESTRUCTIVE != 1 -- refusing to write "
                        "to the namespace on " << want_pci();
    }

    std::string detail;
    if (!daemon_reachable(detail)) {
        GTEST_SKIP() << "daemon not reachable on " << endpoint()
                     << ". Detail:\n" << detail;
    }

    // --count 0 lets the daemon apply its default grant.
    ClientResult r = run_client("--device " + device_id() +
                                " --cuda " + cuda_id() + " --count 0");
    ASSERT_TRUE(r.spawned) << "failed to spawn client " << client_bin();

    // Diagnose the classic sm-arch mismatch before a bare exit-code assert.
    if (r.output.find("no kernel image is available") != std::string::npos ||
        r.output.find("NoKernelImage") != std::string::npos) {
        FAIL() << "CUDA kernel/GPU arch mismatch: the client has no cubin for "
                  "this GPU. Rebuild build_layer2 with "
                  "-DCMAKE_CUDA_ARCHITECTURES=89 (L40S). Output:\n" << r.output;
    }

    EXPECT_EQ(r.exit_code, 0) << "client full-IO output:\n" << r.output;
    EXPECT_NE(r.output.find("Write+Read+verify"), std::string::npos)
        << "expected the write/read/verify success step:\n" << r.output;
    EXPECT_EQ(r.output.find("[FAIL]"), std::string::npos)
        << "a libnvm/IO step reported [FAIL]:\n" << r.output;
    EXPECT_EQ(r.output.find("mismatch"), std::string::npos)
        << "read-back data mismatch:\n" << r.output;
}

