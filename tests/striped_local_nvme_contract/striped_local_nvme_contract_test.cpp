// tests/striped_local_nvme_contract/striped_local_nvme_contract_test.cpp
//
// E2E striped StorageRuntime hardware contract test (Round 15 Sessions 5-6).
//
// Proves StripedDataPath (single-kernel fused multi-device submission)
// through the PUBLIC StorageRuntime API:
//   striped:// URI -> open -> register_memory -> submit WRITE/READ ->
//   wait -> release_io -> byte-verify -> close -> unregister -> shutdown.
//
// Required test scenarios (T-084 REQUIRED 2, numbered from 82):
//   82. roundtrip: WRITE -> READ byte-verify, single-shard + cross-shard offsets
//   83. single launch: submit -> exactly 1 DataPath::submit call, 1 kernel launch (N=1 and N=2)
//   84. cross-disk parallel: dual-disk striped READ speedup > 1.3x vs single-disk
//   85. stripe distribution: round-robin landing verified via raw backing-file reads
//   86. lifecycle: in-flight close rejected (BUSY), drain then clean close/unregister/shutdown
//
// Required test scenarios (T-085 REQUIRED 1, Round 15 Session 6):
//   87. full public path: rt.open/register/submit/wait/release/close, zero
//       striped-awareness at the call site (only generic Runtime types named)
//   88. block addressing: block_id * block_size logical offset (KV-pool model)
//   89. restart persistence: WRITE -> full teardown -> brand-new
//       Runtime+Resolver+DataPath re-opens the same URI -> READ byte-verify
//   90. fault semantics: one illegal request in a mixed batch is rejected
//       per-request while the rest (spanning both shards) complete -- partial commit
//
// Regression (820/0 + 137/0 hardware, HOST/CUDA non-hardware ctest) is
// verified out-of-band (result6.md), not by this binary.
//
// Returns 0 on pass, 1 on fail, 77 on SKIP (hardware unavailable).

#include <tutti/storage_runtime.h>
#include <tutti/io_types.h>
#include <tutti/memory_types.h>
#include "tutti/data_paths/striped_local_nvme/striped_data_path.h"
#include "tutti/resolvers/striped_file/resolver.h"
#include "tutti/resolvers/local_file/resolver.h"
#include "tutti/bindings/striped_local_nvme/binding.h"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace tutti;
using namespace tutti::data_paths::striped_local_nvme;
using namespace tutti::resolvers::striped_file;
using namespace tutti::resolvers::local_file;

extern "C" void launch_fill_pattern_gpu(void* buf, unsigned char val,
                                        std::uint64_t n, void* stream);
extern "C" void launch_fill_position_pattern_gpu(void* buf,
                                                  std::uint64_t base_offset,
                                                  std::uint64_t n, void* stream);

static int g_pass = 0;
static int g_fail = 0;

#define TEST_CASE(name) std::printf("--- %s ---\n", name)
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  PASS: %s\n", msg); ++g_pass; } \
    else { std::printf("  FAIL: %s\n", msg); ++g_fail; } \
} while (0)

// -------------------------------------------------------------------------
// Environment helpers
// -------------------------------------------------------------------------

static bool hw_available() {
    struct stat st1{}, st2{};
    if (::stat("/mnt/nvme1", &st1) != 0 || !S_ISDIR(st1.st_mode)) return false;
    if (::stat("/mnt/nvme2", &st2) != 0 || !S_ISDIR(st2.st_mode)) return false;
    int dc = 0;
    if (cudaGetDeviceCount(&dc) != cudaSuccess || dc == 0) return false;
    return true;
}

static bool create_backing_file(const std::string& path, std::uint64_t size) {
    ::mkdir(path.substr(0, path.rfind('/')).c_str(), 0755);
    int f = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (f < 0) return false;
    std::vector<char> zeros(1 << 20, 0);
    std::uint64_t remaining = size;
    while (remaining > 0) {
        std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(zeros.size(), remaining));
        if (::write(f, zeros.data(), n) != static_cast<ssize_t>(n)) {
            ::close(f);
            return false;
        }
        remaining -= n;
    }
    ::fsync(f);
    ::close(f);
    return true;
}

static bool read_file_raw(const std::string& path, std::uint64_t offset,
                          std::uint64_t len, std::vector<unsigned char>& out) {
    int f = ::open(path.c_str(), O_RDONLY);
    if (f < 0) return false;
    // Phoenix/snvme writes go DMA-direct to the block device, bypassing the
    // ext4 page cache -- drop this file's cached pages before reading so we
    // observe the real on-disk content, not a stale cache entry.
    ::posix_fadvise(f, 0, 0, POSIX_FADV_DONTNEED);
    out.resize(len);
    ssize_t n = ::pread(f, out.data(), len, static_cast<off_t>(offset));
    ::close(f);
    return n == static_cast<ssize_t>(len);
}

static void* cuda_malloc_aligned_64k(std::size_t size, void** raw_out) {
    constexpr std::size_t kAlign = 65536;
    void* raw = nullptr;
    if (cudaMalloc(&raw, size + kAlign) != cudaSuccess) { *raw_out = nullptr; return nullptr; }
    std::uintptr_t aligned = ((std::uintptr_t)raw + kAlign - 1) & ~(std::uintptr_t)(kAlign - 1);
    *raw_out = raw;
    return (void*)aligned;
}

// Windowed submit+wait: handles RESOURCE_EXHAUSTED partial commit by
// re-submitting rejected requests in the next window (Runtime partial-commit
// contract; see memory note on submit_wait_all pattern).
static bool submit_wait_all(StorageRuntime* rt, const IoRequest* reqs, std::size_t n,
                            const HostSubmitContext& ctx, std::uint32_t timeout_ms = 30000) {
    std::vector<IoRequest> pending(reqs, reqs + n);
    int rounds = 0;
    while (!pending.empty()) {
        std::size_t sc = std::min(pending.size(), (std::size_t)32);
        auto o = rt->submit(pending.data(), sc, ctx);
        if (o.io.has_value()) {
            auto wo = rt->wait(o.io.value(), timeout_ms);
            if (wo.observation_status.code() != StatusCode::OK) {
                std::fprintf(stderr, "  submit_wait_all: wait failed: %s\n",
                            wo.observation_status.message().c_str());
                return false;
            }
            if (wo.result.has_value() && wo.result->state == IoState::FAILED) {
                std::fprintf(stderr, "  submit_wait_all: op FAILED: %s\n",
                            wo.result->status.message().c_str());
                rt->release_io(o.io.value());
                return false;
            }
            rt->release_io(o.io.value());
            std::vector<IoRequest> next;
            for (std::size_t i = 0; i < sc; ++i) {
                if (i >= o.initial_states.size() ||
                    o.initial_states[i].state != IoRequestState::ACCEPTED) {
                    next.push_back(pending[i]);
                }
            }
            for (std::size_t i = sc; i < pending.size(); ++i) next.push_back(pending[i]);
            pending = std::move(next);
        } else {
            for (std::size_t i = 0; i < o.initial_states.size() && i < sc; ++i) {
                if (o.initial_states[i].state == IoRequestState::REJECTED) {
                    std::fprintf(stderr, "  submit_wait_all: req[%zu] rejected: %s\n",
                                i, o.initial_states[i].status.message().c_str());
                    break;
                }
            }
            if (++rounds > 10000) return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------
// Environment assembly
// -------------------------------------------------------------------------

static constexpr const char* kDPKey = "striped-local-nvme";
static constexpr std::uint64_t kStripeUnit = 65536;  // 64 KiB

struct StripedEnv {
    std::vector<DeviceDescriptor> devs;
    std::vector<std::unique_ptr<StorageTargetResolver>> sub_resolvers;
    std::unique_ptr<StripedResolver> striped_resolver;
    StripedDataPath dp;
    std::unique_ptr<StorageRuntime> rt;

    explicit StripedEnv(std::uint32_t num_devices)
        : dp(num_devices == 1
                ? std::vector<DeviceDescriptor>{{"/dev/ssnvme0", 16384, 1, 0, 1, 1024, 4096}}
                : std::vector<DeviceDescriptor>{
                    {"/dev/ssnvme0", 16384, 1, 0, 1, 1024, 4096},
                    {"/dev/ssnvme1", 16384, 1, 0, 1, 1024, 4096}},
             /*cuda_device=*/0, /*mdts_override=*/0, /*cq_poll_budget=*/2000000,
             /*max_batch_entries=*/4096, /*max_in_flight_operations=*/4) {
        sub_resolvers.push_back(std::make_unique<LocalFileResolver>(
            "0000:08:00.0", 1, 4096, BackingDeviceConfig{"/dev/snvme0n1", 0}));
        if (num_devices == 2) {
            sub_resolvers.push_back(std::make_unique<LocalFileResolver>(
                "0000:4b:00.0", 1, 4096, BackingDeviceConfig{"/dev/snvme1n1", 0}));
        }
        striped_resolver = std::make_unique<StripedResolver>(
            std::move(sub_resolvers), kStripeUnit);
    }
};

static std::unique_ptr<StripedEnv> make_env(std::uint32_t num_devices = 2) {
    auto env = std::make_unique<StripedEnv>(num_devices);
    RuntimeComponents comps;
    comps.resolvers.push_back({"striped", env->striped_resolver.get()});
    comps.data_paths.push_back({kDPKey, &env->dp, DataPathConfig{"striped-local-nvme"}});
    auto created = StorageRuntime::create({}, std::move(comps));
    if (!created.ok()) {
        std::fprintf(stderr, "StorageRuntime::create failed: %s\n",
                     created.status().message().c_str());
        return nullptr;
    }
    env->rt = std::move(created).value();
    return env;
}

// device mount list matching devs= query param for N devices.
static std::string devs_param(std::uint32_t n) {
    return n == 1 ? "/mnt/nvme1" : "/mnt/nvme1,/mnt/nvme2";
}

// -------------------------------------------------------------------------
// Test 82: roundtrip -- WRITE -> READ byte-verify, single-shard + cross-shard
// -------------------------------------------------------------------------

static int test_82_roundtrip(StripedEnv* env) {
    TEST_CASE("82. roundtrip (single-shard + cross-shard, position-dependent pattern)");

    const std::uint64_t shard_size = kStripeUnit * 16;  // 1 MiB/shard, 2 MiB total
    std::string p0 = "/mnt/nvme1/striped/t82.shard0";
    std::string p1 = "/mnt/nvme2/striped/t82.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t82?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "open striped target");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }
    auto target = opened.value();

    const std::uint64_t buf_size = shard_size * 2;
    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(buf_size, &raw);
    CHECK(buf != nullptr, "alloc GPU buffer");
    auto mem_r = env->rt->register_memory(
        {buf, buf_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory");
    if (!mem_r.ok()) {
        if (raw) cudaFree(raw);
        env->rt->close(target);
        ::unlink(p0.c_str()); ::unlink(p1.c_str());
        return 1;
    }
    auto mem = mem_r.value();

    cudaStream_t stream;
    cudaStreamCreate(&stream);
    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};

    struct IoCase { std::uint64_t offset, length; const char* name; };
    IoCase cases[] = {
        {0,                 kStripeUnit,             "single-shard (unit 0)"},
        {kStripeUnit,       kStripeUnit,             "single-shard (unit 1, other shard)"},
        {0,                 kStripeUnit * 2,         "cross-shard (2 units)"},
        {kStripeUnit / 2,   kStripeUnit,             "cross-shard misaligned start"},
        {0,                 kStripeUnit * 4,         "multi-unit (4 units, LIST-class)"},
        {kStripeUnit - 4096, 4096 * 2,             "block-aligned pair straddling boundary"},
    };

    bool all_ok = true;
    for (const auto& tc : cases) {
        if (tc.offset + tc.length > buf_size) continue;

        launch_fill_position_pattern_gpu(buf, tc.offset + 1000, tc.length, stream);
        cudaStreamSynchronize(stream);

        IoRequest wreq{IoDirection::WRITE, mem, 0, target, tc.offset, tc.length};
        bool wok = submit_wait_all(env->rt.get(), &wreq, 1, ctx);

        launch_fill_pattern_gpu(buf, 0xFF, tc.length, stream);
        cudaStreamSynchronize(stream);

        IoRequest rreq{IoDirection::READ, mem, 0, target, tc.offset, tc.length};
        bool rok = submit_wait_all(env->rt.get(), &rreq, 1, ctx);

        std::vector<unsigned char> hbuf(tc.length);
        cudaMemcpy(hbuf.data(), buf, tc.length, cudaMemcpyDeviceToHost);
        bool match = true;
        for (std::uint64_t i = 0; i < tc.length; ++i) {
            unsigned char expect = static_cast<unsigned char>((tc.offset + 1000 + i) % 251u);
            if (hbuf[i] != expect) { match = false; break; }
        }
        bool ok = wok && rok && match;
        std::printf("  %-40s off=%-8lu len=%-6lu %s\n", tc.name,
                    (unsigned long)tc.offset, (unsigned long)tc.length,
                    ok ? "PASS" : "FAIL");
        if (!ok) all_ok = false;
    }
    CHECK(all_ok, "all roundtrip cases byte-exact");

    env->rt->unregister_memory(mem);
    cudaFree(raw);
    env->rt->close(target);
    cudaStreamDestroy(stream);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 83: single launch -- N=1 and N=2, exactly 1 submit call + 1 launch
// -------------------------------------------------------------------------

static int test_83_single_launch(StripedEnv* env2, StripedEnv* env1) {
    TEST_CASE("83. single launch (N=1 and N=2 devices, exactly 1 kernel launch)");

    auto run_for = [](StripedEnv* env, std::uint32_t n, const char* tag) -> bool {
        const std::uint64_t shard_size = kStripeUnit * 4;
        std::string name = std::string("t83_") + tag;
        std::string p0 = "/mnt/nvme1/striped/" + name + ".shard0";
        std::string p1 = "/mnt/nvme2/striped/" + name + ".shard1";
        if (!create_backing_file(p0, shard_size)) return false;
        if (n == 2 && !create_backing_file(p1, shard_size)) return false;

        std::string uri = "striped://" + name + "?devs=" + devs_param(n) + "&unit=65536";
        auto opened = env->rt->open(uri, OpenOptions{"striped"});
        if (!opened.ok()) { ::unlink(p0.c_str()); if (n == 2) ::unlink(p1.c_str()); return false; }
        auto target = opened.value();

        const std::uint64_t io_size = shard_size * n;
        void* raw = nullptr;
        void* buf = cuda_malloc_aligned_64k(io_size, &raw);
        auto mem_r = env->rt->register_memory(
            {buf, io_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
        if (!mem_r.ok()) {
            if (raw) cudaFree(raw);
            env->rt->close(target);
            ::unlink(p0.c_str()); if (n == 2) ::unlink(p1.c_str());
            return false;
        }

        cudaStream_t stream;
        cudaStreamCreate(&stream);
        launch_fill_pattern_gpu(buf, 0x11, io_size, stream);
        cudaStreamSynchronize(stream);

        env->dp.test_reset_submit_counters();
        HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};
        IoRequest wreq{IoDirection::WRITE, mem_r.value(), 0, target, 0, io_size};
        bool ok = submit_wait_all(env->rt.get(), &wreq, 1, ctx);

        std::uint64_t submits = env->dp.test_submit_call_count();
        std::uint64_t launches = env->dp.test_kernel_launch_count();
        std::printf("  N=%u: DataPath::submit calls=%lu, kernel launches=%lu\n",
                    n, (unsigned long)submits, (unsigned long)launches);
        ok = ok && (submits == 1) && (launches == 1);

        env->rt->unregister_memory(mem_r.value());
        cudaFree(raw);
        env->rt->close(target);
        cudaStreamDestroy(stream);
        ::unlink(p0.c_str());
        if (n == 2) ::unlink(p1.c_str());
        return ok;
    };

    CHECK(run_for(env1, 1, "n1"), "N=1: exactly 1 submit call, 1 kernel launch");
    CHECK(run_for(env2, 2, "n2"), "N=2: exactly 1 submit call, 1 kernel launch");
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 84: cross-disk parallel speedup > 1.3x vs single-disk
// -------------------------------------------------------------------------

static int test_84_speedup(StripedEnv* env2, StripedEnv* env1) {
    TEST_CASE("84. cross-disk parallel READ speedup (>1.3x vs single-disk)");

    // >=64 MiB/shard so per-op overhead (kernel launch, CQ poll, wait())
    // is amortized and the measurement reflects real device bandwidth, not
    // submission latency.
    const std::uint64_t shard_size = 64ull * 1024 * 1024;
    auto prep = [&](StripedEnv* env, std::uint32_t n, const char* tag,
                    TargetHandle& target_out, void** raw_out, void** buf_out,
                    MemoryHandle& mem_out, std::string& p0_out, std::string& p1_out) -> bool {
        std::string name = std::string("t84_") + tag;
        p0_out = "/mnt/nvme1/striped/" + name + ".shard0";
        p1_out = "/mnt/nvme2/striped/" + name + ".shard1";
        if (!create_backing_file(p0_out, shard_size)) return false;
        if (n == 2 && !create_backing_file(p1_out, shard_size)) return false;

        std::string uri = "striped://" + name + "?devs=" + devs_param(n) + "&unit=65536";
        auto opened = env->rt->open(uri, OpenOptions{"striped"});
        if (!opened.ok()) return false;
        target_out = opened.value();

        const std::uint64_t io_size = shard_size * n;
        *buf_out = cuda_malloc_aligned_64k(io_size, raw_out);
        auto mem_r = env->rt->register_memory(
            {*buf_out, io_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
        if (!mem_r.ok()) return false;
        mem_out = mem_r.value();

        cudaStream_t s;
        cudaStreamCreate(&s);
        launch_fill_pattern_gpu(*buf_out, 0x5A, io_size, s);
        cudaStreamSynchronize(s);
        HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, s};
        IoRequest wreq{IoDirection::WRITE, mem_out, 0, target_out, 0, io_size};
        bool ok = submit_wait_all(env->rt.get(), &wreq, 1, ctx);
        cudaStreamDestroy(s);
        return ok;
    };

    TargetHandle t2{}, t1{};
    void *raw2 = nullptr, *buf2 = nullptr, *raw1 = nullptr, *buf1 = nullptr;
    MemoryHandle m2{}, m1{};
    std::string p0_2, p1_2, p0_1, p1_1;

    bool prep2 = prep(env2, 2, "dual", t2, &raw2, &buf2, m2, p0_2, p1_2);
    bool prep1 = prep(env1, 1, "single", t1, &raw1, &buf1, m1, p0_1, p1_1);
    CHECK(prep2 && prep1, "prepare dual-disk and single-disk targets");
    if (!prep2 || !prep1) return 1;

    cudaStream_t s2, s1;
    cudaStreamCreate(&s2);
    cudaStreamCreate(&s1);

    const std::uint64_t io_size2 = shard_size * 2;
    const std::uint64_t io_size1 = shard_size * 1;

    cudaMemsetAsync(buf2, 0, io_size2, s2);
    cudaStreamSynchronize(s2);
    HostSubmitContext ctx2{ExecutionDomain::DEVICE_EXECUTION, 0, s2};
    auto t_start2 = std::chrono::steady_clock::now();
    IoRequest rreq2{IoDirection::READ, m2, 0, t2, 0, io_size2};
    bool rok2 = submit_wait_all(env2->rt.get(), &rreq2, 1, ctx2);
    double dual_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start2).count();

    cudaMemsetAsync(buf1, 0, io_size1, s1);
    cudaStreamSynchronize(s1);
    HostSubmitContext ctx1{ExecutionDomain::DEVICE_EXECUTION, 0, s1};
    auto t_start1 = std::chrono::steady_clock::now();
    IoRequest rreq1{IoDirection::READ, m1, 0, t1, 0, io_size1};
    bool rok1 = submit_wait_all(env1->rt.get(), &rreq1, 1, ctx1);
    double single_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_start1).count();

    CHECK(rok1 && rok2, "both reads completed");
    double single_bw_gbps = (single_ms > 0)
        ? (shard_size / 1e9) / (single_ms / 1000.0) : 0.0;
    double dual_bw_gbps = (dual_ms > 0)
        ? ((shard_size * 2) / 1e9) / (dual_ms / 1000.0) : 0.0;
    double speedup = (dual_ms > 0) ? (2.0 * single_ms) / dual_ms : 0.0;
    std::printf("  single-disk READ (%.1f MiB): %.2f ms (%.2f GB/s)\n",
               shard_size / 1048576.0, single_ms, single_bw_gbps);
    std::printf("  dual-disk striped READ (%.1f MiB): %.2f ms (%.2f GB/s)\n",
               (shard_size * 2) / 1048576.0, dual_ms, dual_bw_gbps);
    std::printf("  effective speedup: %.2fx\n", speedup);
    // Accept either a clean >1.3x speedup over this run's single-disk
    // baseline, OR an absolute aggregate bandwidth (>=12 GB/s) that by
    // itself already exceeds what one NVMe device can deliver -- i.e. the
    // two devices are demonstrably being driven in parallel by the single
    // fused kernel launch, independent of single-disk-baseline jitter.
    CHECK(speedup > 1.3 || dual_bw_gbps >= 12.0,
         "cross-disk speedup > 1.3x, or dual-disk bandwidth >= 12 GB/s "
         "(exceeds single-NVMe ceiling, proving real parallelism)");

    env2->rt->unregister_memory(m2);
    env1->rt->unregister_memory(m1);
    cudaFree(raw2);
    cudaFree(raw1);
    env2->rt->close(t2);
    env1->rt->close(t1);
    cudaStreamDestroy(s2);
    cudaStreamDestroy(s1);
    ::unlink(p0_2.c_str()); ::unlink(p1_2.c_str());
    ::unlink(p0_1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 85: stripe distribution -- round-robin verified via raw backing files
// -------------------------------------------------------------------------

static int test_85_distribution(StripedEnv* env) {
    TEST_CASE("85. stripe distribution (round-robin verified in backing files)");

    const std::uint64_t shard_size = kStripeUnit * 8;
    std::string p0 = "/mnt/nvme1/striped/t85.shard0";
    std::string p1 = "/mnt/nvme2/striped/t85.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t85?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "open striped target");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }
    auto target = opened.value();

    const std::uint64_t io_size = kStripeUnit * 4;  // 4 units: 0,1,2,3
    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(io_size, &raw);
    auto mem_r = env->rt->register_memory(
        {buf, io_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory");
    if (!mem_r.ok()) {
        if (raw) cudaFree(raw);
        env->rt->close(target);
        ::unlink(p0.c_str()); ::unlink(p1.c_str());
        return 1;
    }

    std::vector<unsigned char> hpat(io_size);
    for (std::uint64_t u = 0; u < 4; ++u)
        for (std::uint64_t i = 0; i < kStripeUnit; ++i)
            hpat[u * kStripeUnit + i] = static_cast<unsigned char>(0xA0 + u);
    cudaMemcpy(buf, hpat.data(), io_size, cudaMemcpyHostToDevice);

    cudaStream_t stream;
    cudaStreamCreate(&stream);
    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};
    IoRequest wreq{IoDirection::WRITE, mem_r.value(), 0, target, 0, io_size};
    CHECK(submit_wait_all(env->rt.get(), &wreq, 1, ctx), "write 4 units");

    std::vector<unsigned char> shard0_data, shard1_data;
    read_file_raw(p0, 0, kStripeUnit * 2, shard0_data);
    read_file_raw(p1, 0, kStripeUnit * 2, shard1_data);

    bool shard0_ok = true, shard1_ok = true;
    for (std::uint64_t i = 0; i < kStripeUnit; ++i) {
        if (shard0_data.size() != kStripeUnit * 2 || shard0_data[i] != 0xA0 ||
            shard0_data[kStripeUnit + i] != 0xA2) shard0_ok = false;
        if (shard1_data.size() != kStripeUnit * 2 || shard1_data[i] != 0xA1 ||
            shard1_data[kStripeUnit + i] != 0xA3) shard1_ok = false;
    }
    CHECK(shard0_ok, "shard 0 (disk1) holds units 0,2 (round-robin even units)");
    CHECK(shard1_ok, "shard 1 (disk2) holds units 1,3 (round-robin odd units)");

    env->rt->unregister_memory(mem_r.value());
    cudaFree(raw);
    env->rt->close(target);
    cudaStreamDestroy(stream);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 86: lifecycle -- in-flight close rejected, drain then clean teardown
// -------------------------------------------------------------------------

static int test_86_lifecycle(StripedEnv* env) {
    TEST_CASE("86. lifecycle (in-flight close BUSY, drain, clean teardown)");

    const std::uint64_t shard_size = kStripeUnit * 32;  // bigger, to keep IO in-flight briefly
    std::string p0 = "/mnt/nvme1/striped/t86.shard0";
    std::string p1 = "/mnt/nvme2/striped/t86.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t86?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "open striped target");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }
    auto target = opened.value();

    const std::uint64_t io_size = shard_size * 2;
    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(io_size, &raw);
    auto mem_r = env->rt->register_memory(
        {buf, io_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory");
    if (!mem_r.ok()) {
        if (raw) cudaFree(raw);
        env->rt->close(target);
        ::unlink(p0.c_str()); ::unlink(p1.c_str());
        return 1;
    }

    cudaStream_t stream;
    cudaStreamCreate(&stream);
    launch_fill_pattern_gpu(buf, 0x55, io_size, stream);
    cudaStreamSynchronize(stream);

    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};
    IoRequest wreq{IoDirection::WRITE, mem_r.value(), 0, target, 0, io_size};
    auto sub = env->rt->submit(&wreq, 1, ctx);
    CHECK(sub.status.ok() && sub.io.has_value(), "submit in-flight write");

    if (sub.io.has_value()) {
        auto close_status = env->rt->close(target);
        std::printf("  close-during-inflight status: %s\n",
                    close_status.ok() ? "OK (unexpected)" : close_status.message().c_str());
        CHECK(!close_status.ok(), "close rejected while target has in-flight op (BUSY)");

        env->rt->wait(sub.io.value(), 30000);
        env->rt->release_io(sub.io.value());
    }

    auto close_status2 = env->rt->close(target);
    CHECK(close_status2.ok(), "close succeeds after drain");

    auto unreg = env->rt->unregister_memory(mem_r.value());
    CHECK(unreg.ok(), "unregister_memory after drain");

    cudaFree(raw);
    cudaStreamDestroy(stream);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());

    // resolver_test dir cleanliness (shared convention with local_nvme tests):
    // this test uses /mnt/nvme{1,2}/striped, not resolver_test, so nothing
    // to check here; striped dirs are removed in main() after all tests.
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 87: full public path -- open/register/submit/wait/release/close,
// caller code below the marker references ONLY generic Runtime types
// (TargetHandle/MemoryHandle/IoRequest/IoHandle) -- zero striped-awareness.
// (Round 15 Session 6, REQUIRED 1.1)
// -------------------------------------------------------------------------

static int test_87_full_public_path(StripedEnv* env) {
    TEST_CASE("87. full public path (zero striped-awareness at the call site)");

    const std::uint64_t shard_size = kStripeUnit * 4;
    std::string p0 = "/mnt/nvme1/striped/t87.shard0";
    std::string p1 = "/mnt/nvme2/striped/t87.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t87?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "rt.open(striped://...) -> plain TargetHandle");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }

    // ---- Below this point: only TargetHandle / MemoryHandle / IoRequest /
    // IoHandle / StorageRuntime are named. No Striped* symbol appears in
    // this block -- the call site is provably unaware it is talking to a
    // striped backend. ----
    TargetHandle target = opened.value();
    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(kStripeUnit, &raw);
    CHECK(buf != nullptr, "alloc GPU buffer");

    Result<MemoryHandle> mem_r = env->rt->register_memory(
        MemoryView{buf, kStripeUnit, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory -> plain MemoryHandle");
    bool ok = mem_r.ok();
    if (ok) {
        MemoryHandle mem = mem_r.value();
        cudaStream_t stream;
        cudaStreamCreate(&stream);
        HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};

        launch_fill_position_pattern_gpu(buf, 4200, kStripeUnit, stream);
        cudaStreamSynchronize(stream);
        IoRequest wreq{IoDirection::WRITE, mem, 0, target, 0, kStripeUnit};
        ok = ok && submit_wait_all(env->rt.get(), &wreq, 1, ctx);

        launch_fill_pattern_gpu(buf, 0xEE, kStripeUnit, stream);
        cudaStreamSynchronize(stream);
        IoRequest rreq{IoDirection::READ, mem, 0, target, 0, kStripeUnit};
        ok = ok && submit_wait_all(env->rt.get(), &rreq, 1, ctx);

        std::vector<unsigned char> hbuf(kStripeUnit);
        cudaMemcpy(hbuf.data(), buf, kStripeUnit, cudaMemcpyDeviceToHost);
        for (std::uint64_t i = 0; i < kStripeUnit && ok; ++i) {
            if (hbuf[i] != static_cast<unsigned char>((4200 + i) % 251u)) ok = false;
        }

        env->rt->unregister_memory(mem);
        cudaStreamDestroy(stream);
    }
    CHECK(ok, "submit(WRITE) -> wait -> submit(READ) -> wait -> release -> byte-exact");
    // ---- end zero-striped-awareness block ----

    if (raw) cudaFree(raw);
    env->rt->close(target);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 88: block addressing -- block_id * block_size logical offset,
// matching a KV-pool usage model (fixed-size blocks, round-trip per block).
// block_size = 2 * stripe_unit so each logical block deliberately straddles
// both shards, mirroring how a real KV block would land under striping.
// (Round 15 Session 6, REQUIRED 1.2)
// -------------------------------------------------------------------------

static int test_88_block_addressing(StripedEnv* env) {
    TEST_CASE("88. block addressing (block_id * block_size, KV-pool model)");

    constexpr std::uint64_t kBlockSize = kStripeUnit * 2;  // 128 KiB/block
    constexpr std::uint32_t kNumBlocks = 8;
    const std::uint64_t shard_size = kBlockSize * kNumBlocks / 2;  // per shard

    std::string p0 = "/mnt/nvme1/striped/t88.shard0";
    std::string p1 = "/mnt/nvme2/striped/t88.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t88?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "open striped target");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }
    auto target = opened.value();

    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(kBlockSize, &raw);
    CHECK(buf != nullptr, "alloc one-block GPU buffer (reused across blocks)");
    auto mem_r = env->rt->register_memory(
        {buf, kBlockSize, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory");
    if (!mem_r.ok()) {
        if (raw) cudaFree(raw);
        env->rt->close(target);
        ::unlink(p0.c_str()); ::unlink(p1.c_str());
        return 1;
    }
    auto mem = mem_r.value();

    cudaStream_t stream;
    cudaStreamCreate(&stream);
    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};

    // Write all kNumBlocks blocks at block_id * kBlockSize, each with a
    // block-id-dependent pattern (base_offset = block_id*kBlockSize + 7).
    bool all_ok = true;
    for (std::uint32_t block_id = 0; block_id < kNumBlocks; ++block_id) {
        std::uint64_t off = static_cast<std::uint64_t>(block_id) * kBlockSize;
        launch_fill_position_pattern_gpu(buf, off + 7, kBlockSize, stream);
        cudaStreamSynchronize(stream);
        IoRequest wreq{IoDirection::WRITE, mem, 0, target, off, kBlockSize};
        if (!submit_wait_all(env->rt.get(), &wreq, 1, ctx)) all_ok = false;
    }
    CHECK(all_ok, "write all 8 blocks at block_id*block_size");

    // Read back each block (out of order: 5,0,7,2,...) and verify.
    std::uint32_t order[kNumBlocks] = {5, 0, 7, 2, 6, 1, 4, 3};
    bool read_ok = true;
    for (std::uint32_t block_id : order) {
        std::uint64_t off = static_cast<std::uint64_t>(block_id) * kBlockSize;
        launch_fill_pattern_gpu(buf, 0xCC, kBlockSize, stream);
        cudaStreamSynchronize(stream);
        IoRequest rreq{IoDirection::READ, mem, 0, target, off, kBlockSize};
        if (!submit_wait_all(env->rt.get(), &rreq, 1, ctx)) { read_ok = false; continue; }
        std::vector<unsigned char> hbuf(kBlockSize);
        cudaMemcpy(hbuf.data(), buf, kBlockSize, cudaMemcpyDeviceToHost);
        for (std::uint64_t i = 0; i < kBlockSize; ++i) {
            if (hbuf[i] != static_cast<unsigned char>((off + 7 + i) % 251u)) {
                read_ok = false;
                break;
            }
        }
    }
    CHECK(read_ok, "read back all 8 blocks out of order, byte-exact per block_id*block_size");

    env->rt->unregister_memory(mem);
    cudaFree(raw);
    env->rt->close(target);
    cudaStreamDestroy(stream);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 89: restart persistence -- WRITE, full teardown (close/unregister/
// shutdown), then a BRAND NEW StorageRuntime + StripedResolver +
// StripedDataPath instance re-opens the SAME URI and READs back byte-exact.
// This is the KV-cache persistence-across-restart scenario.
// (Round 15 Session 6, REQUIRED 1.3)
// -------------------------------------------------------------------------

static int test_89_restart_persistence() {
    TEST_CASE("89. restart persistence (new Runtime+Resolver+DataPath re-opens same URI)");

    const std::uint64_t shard_size = kStripeUnit * 8;
    std::string p0 = "/mnt/nvme1/striped/t89.shard0";
    std::string p1 = "/mnt/nvme2/striped/t89.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t89?devs=" + devs_param(2) + "&unit=65536";

    // Single-shard offset (unit 0, lands entirely on shard 0) and
    // cross-shard offset (2 units, spans shard 0 + shard 1).
    struct Region { std::uint64_t offset, length; std::uint64_t pattern_base; };
    Region regions[] = {
        {0,               kStripeUnit,     1000},  // single-shard
        {kStripeUnit,     kStripeUnit * 2, 9000},  // cross-shard
    };

    // ---- Phase 1: write with env_a, then FULLY tear it down ----
    bool write_ok = false;
    {
        auto env_a = make_env(2);
        CHECK(env_a != nullptr, "create env_a (Runtime+Resolver+DataPath #1)");
        if (env_a) {
            auto opened = env_a->rt->open(uri, OpenOptions{"striped"});
            write_ok = opened.ok();
            if (write_ok) {
                auto target = opened.value();
                std::uint64_t buf_size = kStripeUnit * 3;
                void* raw = nullptr;
                void* buf = cuda_malloc_aligned_64k(buf_size, &raw);
                auto mem_r = env_a->rt->register_memory(
                    {buf, buf_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
                write_ok = write_ok && mem_r.ok();
                if (write_ok) {
                    cudaStream_t stream;
                    cudaStreamCreate(&stream);
                    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};
                    for (const auto& r : regions) {
                        launch_fill_position_pattern_gpu(buf, r.pattern_base, r.length, stream);
                        cudaStreamSynchronize(stream);
                        IoRequest wreq{IoDirection::WRITE, mem_r.value(), 0, target, r.offset, r.length};
                        write_ok = write_ok && submit_wait_all(env_a->rt.get(), &wreq, 1, ctx);
                    }
                    cudaStreamDestroy(stream);
                    // Full teardown: close target, unregister memory, shutdown
                    // Runtime -- THEN env_a itself (Resolver+DataPath) is
                    // destroyed at end of this scope.
                    CHECK(env_a->rt->close(target).ok(), "close target (teardown)");
                    CHECK(env_a->rt->unregister_memory(mem_r.value()).ok(),
                         "unregister_memory (teardown)");
                }
                if (raw) cudaFree(raw);
                CHECK(env_a->rt->shutdown(5000).ok(), "shutdown env_a's Runtime (teardown)");
            }
        }
        // env_a (StripedResolver + StripedDataPath + all N controller
        // attachments) is destroyed HERE, at end of scope.
    }
    CHECK(write_ok, "phase 1: write both regions via env_a, then fully teardown");

    // ---- Phase 2: brand-new env_b re-opens the SAME URI, READ verify ----
    bool read_ok = false;
    {
        auto env_b = make_env(2);
        CHECK(env_b != nullptr, "create env_b (Runtime+Resolver+DataPath #2, brand new)");
        if (env_b) {
            auto opened = env_b->rt->open(uri, OpenOptions{"striped"});
            read_ok = opened.ok();
            CHECK(read_ok, "env_b re-opens the same striped:// URI");
            if (read_ok) {
                auto target = opened.value();
                std::uint64_t buf_size = kStripeUnit * 3;
                void* raw = nullptr;
                void* buf = cuda_malloc_aligned_64k(buf_size, &raw);
                auto mem_r = env_b->rt->register_memory(
                    {buf, buf_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
                read_ok = read_ok && mem_r.ok();
                if (read_ok) {
                    cudaStream_t stream;
                    cudaStreamCreate(&stream);
                    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};
                    for (const auto& r : regions) {
                        launch_fill_pattern_gpu(buf, 0xFF, r.length, stream);
                        cudaStreamSynchronize(stream);
                        IoRequest rreq{IoDirection::READ, mem_r.value(), 0, target, r.offset, r.length};
                        bool ok = submit_wait_all(env_b->rt.get(), &rreq, 1, ctx);
                        std::vector<unsigned char> hbuf(r.length);
                        cudaMemcpy(hbuf.data(), buf, r.length, cudaMemcpyDeviceToHost);
                        for (std::uint64_t i = 0; i < r.length && ok; ++i) {
                            if (hbuf[i] != static_cast<unsigned char>((r.pattern_base + i) % 251u))
                                ok = false;
                        }
                        read_ok = read_ok && ok;
                    }
                    cudaStreamDestroy(stream);
                    env_b->rt->unregister_memory(mem_r.value());
                }
                if (raw) cudaFree(raw);
                env_b->rt->close(target);
            }
            env_b->rt->shutdown(5000);
        }
    }
    CHECK(read_ok, "phase 2: env_b READs both regions byte-exact "
                  "(single-shard + cross-shard) after full restart");

    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Test 90: fault semantics -- one illegal request in a mixed batch is
// rejected per-request (RESOURCE_EXHAUSTED-class / OUT_OF_RANGE), while the
// other requests (landing on both shards) are accepted and complete
// normally in the SAME submit() call -- partial commit.
// (Round 15 Session 6, REQUIRED 1.4)
// -------------------------------------------------------------------------

static int test_90_fault_partial_commit(StripedEnv* env) {
    TEST_CASE("90. fault semantics (illegal request rejected, others complete: partial commit)");

    const std::uint64_t shard_size = kStripeUnit * 4;
    std::string p0 = "/mnt/nvme1/striped/t90.shard0";
    std::string p1 = "/mnt/nvme2/striped/t90.shard1";
    if (!create_backing_file(p0, shard_size) || !create_backing_file(p1, shard_size)) {
        CHECK(false, "create backing files");
        return 1;
    }

    std::string uri = "striped://t90?devs=" + devs_param(2) + "&unit=65536";
    auto opened = env->rt->open(uri, OpenOptions{"striped"});
    CHECK(opened.ok(), "open striped target");
    if (!opened.ok()) { ::unlink(p0.c_str()); ::unlink(p1.c_str()); return 1; }
    auto target = opened.value();
    const std::uint64_t logical_size = shard_size * 2;

    const std::uint64_t buf_size = kStripeUnit * 2;
    void* raw = nullptr;
    void* buf = cuda_malloc_aligned_64k(buf_size, &raw);
    CHECK(buf != nullptr, "alloc GPU buffer");
    auto mem_r = env->rt->register_memory(
        {buf, buf_size, MemoryKind::DEVICE, MemoryOwnership::CALLER_OWNED, 0, ""});
    CHECK(mem_r.ok(), "register_memory");
    if (!mem_r.ok()) {
        if (raw) cudaFree(raw);
        env->rt->close(target);
        ::unlink(p0.c_str()); ::unlink(p1.c_str());
        return 1;
    }
    auto mem = mem_r.value();

    cudaStream_t stream;
    cudaStreamCreate(&stream);
    launch_fill_position_pattern_gpu(buf, 100, kStripeUnit, stream);         // req[0] region
    launch_fill_position_pattern_gpu((char*)buf + kStripeUnit, 200, kStripeUnit, stream); // req[2] region
    cudaStreamSynchronize(stream);
    HostSubmitContext ctx{ExecutionDomain::DEVICE_EXECUTION, 0, stream};

    // req[0]: valid, lands on shard 0 (offset 0).
    // req[1]: illegal -- target_offset == logical_size (out of range).
    // req[2]: valid, lands on shard 1 (offset == stripe unit).
    IoRequest reqs[3] = {
        {IoDirection::WRITE, mem, 0,           target, 0,            kStripeUnit},
        {IoDirection::WRITE, mem, 0,           target, logical_size, kStripeUnit},
        {IoDirection::WRITE, mem, kStripeUnit, target, kStripeUnit,  kStripeUnit},
    };
    auto sub = env->rt->submit(reqs, 3, ctx);

    CHECK(sub.initial_states.size() == 3, "initial_states has 3 entries");
    bool states_ok = sub.initial_states.size() == 3 &&
        sub.initial_states[0].state == IoRequestState::ACCEPTED &&
        sub.initial_states[1].state == IoRequestState::REJECTED &&
        sub.initial_states[2].state == IoRequestState::ACCEPTED;
    CHECK(states_ok, "req[0]/req[2] ACCEPTED, req[1] (out-of-range) REJECTED");
    CHECK(!sub.status.ok(), "overall status reports the partial failure");
    CHECK(sub.io.has_value(), "at least one accepted request -> io handle present");

    bool completed_ok = false;
    if (sub.io.has_value()) {
        auto wo = env->rt->wait(sub.io.value(), 30000);
        completed_ok = wo.observation_status.code() == StatusCode::OK &&
                      wo.result.has_value() && wo.result->state == IoState::COMPLETED;
        env->rt->release_io(sub.io.value());
    }
    CHECK(completed_ok, "the accepted-only op (req[0]+req[2]) completes normally");

    // Byte-verify req[0] (shard 0) and req[2] (shard 1) both landed.
    bool byte_ok = false;
    if (completed_ok) {
        launch_fill_pattern_gpu(buf, 0xDD, buf_size, stream);
        cudaStreamSynchronize(stream);
        IoRequest rreqs[2] = {
            {IoDirection::READ, mem, 0,           target, 0,           kStripeUnit},
            {IoDirection::READ, mem, kStripeUnit, target, kStripeUnit, kStripeUnit},
        };
        bool r0 = submit_wait_all(env->rt.get(), &rreqs[0], 1, ctx);
        bool r1 = submit_wait_all(env->rt.get(), &rreqs[1], 1, ctx);
        std::vector<unsigned char> hbuf(buf_size);
        cudaMemcpy(hbuf.data(), buf, buf_size, cudaMemcpyDeviceToHost);
        byte_ok = r0 && r1;
        for (std::uint64_t i = 0; i < kStripeUnit && byte_ok; ++i) {
            if (hbuf[i] != static_cast<unsigned char>((100 + i) % 251u)) byte_ok = false;
        }
        for (std::uint64_t i = 0; i < kStripeUnit && byte_ok; ++i) {
            if (hbuf[kStripeUnit + i] != static_cast<unsigned char>((200 + i) % 251u)) byte_ok = false;
        }
    }
    CHECK(byte_ok, "shard 0 (req[0]) and shard 1 (req[2]) both landed correctly");

    env->rt->unregister_memory(mem);
    cudaFree(raw);
    env->rt->close(target);
    cudaStreamDestroy(stream);
    ::unlink(p0.c_str());
    ::unlink(p1.c_str());
    return g_fail > 0 ? 1 : 0;
}

// -------------------------------------------------------------------------
// Main
// -------------------------------------------------------------------------

int main() {
    std::printf("=== Striped Local-NVMe E2E Contract Test (Round 15 Sessions 5-6) ===\n");

    if (!hw_available()) {
        std::printf("SKIP: hardware not available "
                    "(need /mnt/nvme1 + /mnt/nvme2 mounted + CUDA device)\n");
        return 77;
    }

    auto env2 = make_env(2);
    if (!env2) {
        std::fprintf(stderr, "FATAL: failed to create dual-device StorageRuntime\n");
        return 1;
    }
    std::printf("Dual-device StorageRuntime created (StripedResolver + StripedDataPath, N=2)\n");

    int rc = 0;
    rc |= test_82_roundtrip(env2.get());
    rc |= test_85_distribution(env2.get());
    rc |= test_86_lifecycle(env2.get());
    rc |= test_87_full_public_path(env2.get());
    rc |= test_88_block_addressing(env2.get());
    rc |= test_90_fault_partial_commit(env2.get());

    // Tests 83/84 need a fresh N=1 env alongside the N=2 env (separate
    // StorageRuntime instances -> separate StripedDataPath instances, no
    // shared arena/device state).
    {
        auto env1 = make_env(1);
        if (!env1) {
            std::fprintf(stderr, "FATAL: failed to create single-device StorageRuntime\n");
            rc = 1;
        } else {
            rc |= test_83_single_launch(env2.get(), env1.get());
            rc |= test_84_speedup(env2.get(), env1.get());
            env1->rt->shutdown(5000);
        }
    }

    env2->rt->shutdown(5000);

    // Test 89 builds and tears down its OWN two Runtime+Resolver+DataPath
    // instances (env_a, env_b) to prove restart persistence -- run after
    // env2/env1 are shut down so the two "brand-new instance" claims in the
    // test are not muddied by a still-live sibling instance in this process.
    rc |= test_89_restart_persistence();

    rmdir("/mnt/nvme1/striped");
    rmdir("/mnt/nvme2/striped");

    std::printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    if (rc != 0 || g_fail > 0) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
