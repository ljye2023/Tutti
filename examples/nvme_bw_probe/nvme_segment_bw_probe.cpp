#define _GNU_SOURCE

#include <tutti/tutti_runtime.h>
#include <tutti/storage_runtime.h>
#include <tutti/io_types.h>
#include <tutti/cuda_like.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <numeric>
#include <string>
#include <vector>

using namespace tutti;

#define OK(...) do { std::printf("[ OK ] " __VA_ARGS__); std::printf("\n"); } while (0)
#define FAIL(...) do { std::fprintf(stderr, "[FAIL] " __VA_ARGS__); std::fprintf(stderr, "\n"); return 1; } while (0)
#define CUDA_OK(call) do { cudaError_t e = (call); if (e != cudaSuccess) FAIL("%s: %s", #call, cudaGetErrorString(e)); } while (0)

static double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

static std::int64_t epoch_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static bool create_file(const std::string& path, std::uint64_t bytes) {
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_DIRECT, 0644);
    if (fd < 0) return false;
    if (::fallocate(fd, 0, 0, static_cast<off_t>(bytes)) != 0) {
        ::close(fd);
        return false;
    }
    void* raw = nullptr;
    if (::posix_memalign(&raw, 4096, 4 * 1024 * 1024) != 0) {
        ::close(fd);
        return false;
    }
    std::memset(raw, 0, 4 * 1024 * 1024);
    std::uint64_t written = 0;
    while (written < bytes) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(4 * 1024 * 1024, bytes - written));
        if (::pwrite(fd, raw, n, static_cast<off_t>(written)) !=
            static_cast<ssize_t>(n)) {
            std::free(raw);
            ::close(fd);
            return false;
        }
        written += n;
    }
    std::free(raw);
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

static bool exact_file(const std::string& path, std::uint64_t bytes) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 &&
           static_cast<std::uint64_t>(st.st_size) == bytes;
}

int main(int argc, char** argv) {
    std::string config;
    std::vector<std::string> mounts;
    std::uint32_t chunks = 512;
    std::uint32_t layers = 80;
    std::uint32_t io_kb = 64;
    std::uint32_t requests_per_chunk = 2;
    std::uint64_t file_bytes = 2ull * 1024 * 1024 * 1024;
    std::uint32_t slots_per_file_arg = 0;
    std::uint32_t header_kb = 4;
    std::uint32_t rounds = 2;
    bool fresh = false;
    bool prepare_only = false;
    bool reuse_only = false;
    bool check_only = false;
    bool shuffle_requests = false;
    bool interleave_layers = false;
    bool shuffle_across_layers = false;
    bool shuffle_per_disk = false;
    bool group_by_disk = false;
    bool random_lba = false;
    bool unique_random_lba = false;
    bool match_kv_phase = false;
    bool shuffle_kv_phases = false;
    int fixed_phase_kb = -1;
    std::uint32_t slot_permutation = 1;
    bool random_slot_permutation = false;
    bool rotate_layer_per_chunk = false;
    bool random_layer_per_chunk = false;
    bool metadata_zone = false;
    long addr_stride_kb = -1;
    long addr_base_kb = -1;
    std::uint32_t random_align_kb = 0;
    std::string directory_suffix;
    std::string barrier;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[FAIL] %s requires a value\n", name);
                std::exit(1);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--config")) config = next(a);
        else if (!std::strcmp(a, "--directory")) mounts.emplace_back(next(a));
        else if (!std::strcmp(a, "--chunks")) chunks = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--layers")) layers = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--io-kb")) io_kb = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--requests-per-chunk")) requests_per_chunk = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--segment-file-gib")) file_bytes = static_cast<std::uint64_t>(std::strtoull(next(a), nullptr, 10)) * 1024 * 1024 * 1024;
        else if (!std::strcmp(a, "--slots-per-file")) slots_per_file_arg = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--header-kb")) header_kb = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--rounds")) rounds = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--barrier")) barrier = next(a);
        else if (!std::strcmp(a, "--fresh")) fresh = true;
        else if (!std::strcmp(a, "--prepare-only")) prepare_only = true;
        else if (!std::strcmp(a, "--reuse-only")) reuse_only = true;
        else if (!std::strcmp(a, "--check-only")) check_only = true;
        else if (!std::strcmp(a, "--shuffle-requests")) shuffle_requests = true;
        else if (!std::strcmp(a, "--interleave-layers")) interleave_layers = true;
        else if (!std::strcmp(a, "--shuffle-across-layers")) shuffle_across_layers = true;
        else if (!std::strcmp(a, "--shuffle-per-disk")) shuffle_per_disk = true;
        else if (!std::strcmp(a, "--group-by-disk")) group_by_disk = true;
        else if (!std::strcmp(a, "--random-lba")) random_lba = true;
        else if (!std::strcmp(a, "--unique-random-lba")) {
            random_lba = true;
            unique_random_lba = true;
        }
        else if (!std::strcmp(a, "--match-kv-phase")) match_kv_phase = true;
        else if (!std::strcmp(a, "--shuffle-kv-phases")) {
            match_kv_phase = true;
            shuffle_kv_phases = true;
        }
        else if (!std::strcmp(a, "--fixed-phase-kb")) fixed_phase_kb = std::strtol(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--slot-permutation")) slot_permutation = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--random-slot-permutation")) random_slot_permutation = true;
        else if (!std::strcmp(a, "--rotate-layer-per-chunk")) rotate_layer_per_chunk = true;
        else if (!std::strcmp(a, "--random-layer-per-chunk")) random_layer_per_chunk = true;
        else if (!std::strcmp(a, "--metadata-zone")) metadata_zone = true;
        else if (!std::strcmp(a, "--directory-suffix")) directory_suffix = next(a);
        else if (!std::strcmp(a, "--addr-stride-kb")) addr_stride_kb = std::strtol(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--addr-base-kb")) addr_base_kb = std::strtol(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--random-align-kb")) random_align_kb = std::strtoul(next(a), nullptr, 10);
        else if (!std::strcmp(a, "--help")) {
            std::printf("usage: %s --directory MOUNT [--directory MOUNT ...] [options]\n"
                        "  --chunks N             chunks to read (default 512)\n"
                        "  --layers N             layers per chunk (default 80)\n"
                        "  --io-kb N              page IO size (default 64)\n"
                        "  --requests-per-chunk N requests per chunk/layer (1, 2 or 4; default 2)\n"
                        "  --segment-file-gib N   shared file size (default 2)\n"
                        "  --slots-per-file N     fixed chunks/file (e.g. 2048 or 4096)\n"
                        "  --header-kb N          aligned slot prefix (default 4; test 64)\n"
                        "  --rounds N             read repetitions (default 2)\n"
                        "  --barrier PATH          wait for PATH.go after creating PATH.ready.PID\n"
                        "  --fresh                recreate all files\n"
                        "  --prepare-only         create files without GPU registration or reads\n"
                        "  --reuse-only           fail rather than create missing files\n"
                        "  --check-only           validate existing files through runtime open\n"
                        "  --shuffle-requests     shuffle each layer's unchanged request set\n"
                        "  --interleave-layers    spread all layers across unchanged batch widths\n"
                        "  --shuffle-across-layers shuffle identical requests across 80 batches\n"
                        "  --shuffle-per-disk     shuffle identical requests per disk across batches\n"
                        "  --group-by-disk        group each batch's unchanged requests by disk\n"
                        "  --random-lba           random file offsets; unchanged batch/memory/IO size\n"
                        "  --unique-random-lba    random offsets without replacement per disk\n"
                        "  --match-kv-phase       preserve each KV request's 4 KiB alignment phase\n"
                        "  --shuffle-kv-phases    same phase histogram, randomized assignment\n"
                        "  --fixed-phase-kb N     use a fixed 4 KiB-aligned phase for random LBA\n"
                        "  --slot-permutation N   permute slot positions by coprime multiplier\n"
                        "  --random-slot-permutation  fixed-seed bijection within each file\n"
                        "  --rotate-layer-per-chunk  bijective layer offset per chunk\n"
                        "  --random-layer-per-chunk  seeded layer permutation per chunk\n"
                        "  --metadata-zone        headers packed before all payloads in each file\n"
                        "  --addr-stride-kb N     request address stride between slots (files unchanged)\n"
                        "  --addr-base-kb N       request address of slot 0 layer 0 (files unchanged)\n"
                        "  --random-align-kb N    random LBA granularity (power of 2 >= io; default io)\n", argv[0]);
            return 0;
        } else {
            FAIL("unknown argument: %s", a);
        }
    }
    if (mounts.empty()) FAIL("at least one --directory is required");
    if (random_lba && interleave_layers) FAIL("--random-lba and --interleave-layers are exclusive");
    if (match_kv_phase && !random_lba) FAIL("--match-kv-phase requires random LBA");
    if (fixed_phase_kb >= 0 && (!random_lba || match_kv_phase ||
        fixed_phase_kb >= static_cast<int>(random_align_kb ? random_align_kb : io_kb) ||
        fixed_phase_kb % 4))
        FAIL("--fixed-phase-kb requires random LBA and a valid 4 KiB phase");
    if ((shuffle_across_layers || shuffle_per_disk) && (interleave_layers || random_lba))
        FAIL("--shuffle-across-layers requires ordinary KV requests");
    if (shuffle_across_layers && shuffle_per_disk)
        FAIL("global and per-disk shuffle are exclusive");
    if (random_layer_per_chunk && rotate_layer_per_chunk)
        FAIL("layer rotation and random layer permutation are exclusive");
    if (prepare_only && reuse_only) FAIL("--prepare-only and --reuse-only are exclusive");
    if (check_only && (prepare_only || fresh)) FAIL("--check-only is incompatible with prepare/fresh");
    if (fresh && reuse_only) FAIL("--fresh and --reuse-only are exclusive");
    if (config.empty()) config = TUTTI_NVME_BW_PROBE_DEFAULT_CONFIG_STRIPED;
    if (chunks == 0 || layers == 0 || io_kb == 0 ||
        (requests_per_chunk != 1 && requests_per_chunk != 2 &&
         requests_per_chunk != 4) ||
        static_cast<std::uint64_t>(io_kb) * requests_per_chunk != 128)
        FAIL("geometry requires 128 KiB per chunk/layer (32K x4, 64K x2 or 128K x1)");

    const std::uint64_t page = static_cast<std::uint64_t>(io_kb) * 1024;
    const std::uint64_t segment_bytes = page * requests_per_chunk;
    if (header_kb < 4 || header_kb % 4)
        FAIL("--header-kb must be >=4 and 4 KiB aligned");
    const std::uint64_t header_bytes = static_cast<std::uint64_t>(header_kb) * 1024;
    const std::uint64_t payload_bytes = static_cast<std::uint64_t>(layers) * segment_bytes;
    const std::uint64_t slot_bytes = header_bytes + payload_bytes;
    if (slots_per_file_arg != 0) file_bytes = slots_per_file_arg * slot_bytes;
    if (file_bytes == 0) FAIL("invalid segment file size");
    if (file_bytes < slot_bytes || file_bytes % slot_bytes != 0) {
        FAIL("segment file size %llu is not a multiple of slot size %llu",
             static_cast<unsigned long long>(file_bytes),
             static_cast<unsigned long long>(slot_bytes));
    }
    const std::uint32_t slots_per_file = static_cast<std::uint32_t>(file_bytes / slot_bytes);
    const std::uint64_t metadata_zone_bytes =
        static_cast<std::uint64_t>(slots_per_file) * header_bytes;
    const std::uint64_t payload_zone_offset = metadata_zone_bytes;
    // Address-only override: the files stay exactly as prepared; only the
    // offsets the requests target change. Isolates "which LBAs are read"
    // from "how the files were laid out / allocated".
    const bool addr_override = addr_stride_kb >= 0 || addr_base_kb >= 0;
    if (addr_override) {
        if (addr_stride_kb < 0 || addr_base_kb < 0 || addr_stride_kb % 4 || addr_base_kb % 4)
            FAIL("--addr-stride-kb and --addr-base-kb must both be set and 4 KiB aligned");
        if (metadata_zone || random_lba || interleave_layers || shuffle_across_layers ||
            shuffle_per_disk || random_layer_per_chunk || rotate_layer_per_chunk)
            FAIL("--addr-* only combines with plain layer-mode KV requests");
    }
    const std::uint64_t addr_stride = addr_override ? static_cast<std::uint64_t>(addr_stride_kb) * 1024 : slot_bytes;
    const std::uint64_t addr_base = addr_override ? static_cast<std::uint64_t>(addr_base_kb) * 1024 : header_bytes;
    if (slot_permutation == 0 ||
        std::gcd(slot_permutation, slots_per_file) != 1)
        FAIL("--slot-permutation must be coprime to slots-per-file");
    std::vector<std::uint32_t> slot_order(slots_per_file);
    for (std::uint32_t i = 0; i < slots_per_file; ++i) slot_order[i] = i;
    if (random_slot_permutation) {
        std::mt19937_64 rng(7);
        std::shuffle(slot_order.begin(), slot_order.end(), rng);
    }
    if (addr_override) {
        std::uint64_t max_slot = 0;
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
            const std::uint32_t per_device_slot = chunk / mounts.size();
            max_slot = std::max<std::uint64_t>(max_slot, slot_order[
                (static_cast<std::uint64_t>(per_device_slot % slots_per_file) *
                 slot_permutation) % slots_per_file]);
        }
        if (addr_base + max_slot * addr_stride + payload_bytes > file_bytes)
            FAIL("--addr-* geometry exceeds the file");
    }
    std::vector<std::uint32_t> layer_order;
    if (random_layer_per_chunk) {
        layer_order.resize(static_cast<std::size_t>(chunks) * layers);
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
            const auto first = layer_order.begin() + static_cast<std::size_t>(chunk) * layers;
            for (std::uint32_t l = 0; l < layers; ++l) first[l] = l;
            std::mt19937_64 rng(7 + chunk);
            std::shuffle(first, first + layers, rng);
        }
    }
    const std::uint32_t file_count =
        (chunks + static_cast<std::uint64_t>(mounts.size()) * slots_per_file - 1) /
        (static_cast<std::uint64_t>(mounts.size()) * slots_per_file);
    const std::uint64_t entries_per_layer = static_cast<std::uint64_t>(chunks) * requests_per_chunk;

    auto runtime = TuttiRuntime::create(config);
    if (!runtime.ok()) FAIL("runtime: %s", runtime.status().message().c_str());
    auto owner = std::move(runtime).value();
    StorageRuntime* rt = owner->storage_runtime();
    if (rt == nullptr) FAIL("no storage runtime");
    CUDA_OK(cudaFree(0));
    CUDA_OK(cudaSetDevice(rt->accel_id()));
    OK("geometry: chunks=%u layers=%u io=%u KiB x%u slot=%.2f MiB segment_file=%.2f GiB slots/file/device=%u file_generations=%u total_files=%zu entries/layer=%llu",
       chunks, layers, io_kb, requests_per_chunk, static_cast<double>(slot_bytes) / (1 << 20),
       static_cast<double>(file_bytes) / (1ull << 30), slots_per_file, file_count,
       std::min<std::uint64_t>(chunks, static_cast<std::uint64_t>(mounts.size()) * file_count),
       static_cast<unsigned long long>(entries_per_layer));

    const std::string subdir = std::string("/nvme_segment_bw_probe_rotating_gpu") +
        std::to_string(rt->accel_id()) + "_" +
        std::to_string(slots_per_file) + "_" + std::to_string(slot_bytes) +
        (metadata_zone ? "_metadata" : "") + directory_suffix;
    for (const auto& mount : mounts) {
        const std::string dir = mount + subdir;
        if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
            FAIL("mkdir %s: %s", dir.c_str(), std::strerror(errno));
    }
    std::vector<std::string> paths;
    for (std::uint32_t f = 0; f < file_count; ++f) {
        for (std::size_t d = 0; d < mounts.size(); ++d) {
            if (static_cast<std::uint64_t>(f) * slots_per_file * mounts.size() + d >= chunks)
                break;
            const std::string path = mounts[d] + subdir + "/" +
                                     std::to_string(f) + ".seg";
            paths.push_back(path);
            if ((reuse_only || check_only) && !exact_file(path, file_bytes))
                FAIL("missing or wrong-size prepared file: %s", path.c_str());
            if (!reuse_only && !check_only && (fresh || !exact_file(path, file_bytes))) {
                if (!create_file(path, file_bytes))
                    FAIL("create %s: %s", path.c_str(), std::strerror(errno));
            }
        }
    }
    if (prepare_only) {
        OK("prepared %zu fixed files for GPU %d", paths.size(), rt->accel_id());
        return 0;
    }

    std::vector<TargetHandle> targets;
    for (const auto& path : paths) {
        auto opened = rt->open("file://" + path, OpenOptions{"file"});
        if (!opened.ok()) FAIL("open %s: %s", path.c_str(), opened.status().message().c_str());
        targets.push_back(opened.value());
    }
    if (check_only) {
        OK("validated %zu fixed files for GPU %d", targets.size(), rt->accel_id());
        return 0;
    }

    // Only 64K x2 matches the deployed vLLM pool layout. Other widths are
    // diagnostic layouts with the same 128 KiB per chunk/layer.
    const std::uint64_t pool_bytes = static_cast<std::uint64_t>(chunks) *
                                     requests_per_chunk * 5120 * 1024;
    std::vector<std::uint64_t> random_offsets;
    if (random_lba) {
        const std::uint64_t slots = (file_bytes - page) / page + 1;
        if (slots == 0) FAIL("file too small for random IO");
        random_offsets.resize(static_cast<std::size_t>(rounds) * layers * entries_per_layer);
        std::vector<std::uint64_t> phases;
        if (shuffle_kv_phases) {
            phases.reserve(random_offsets.size());
            for (std::uint32_t round = 0; round < rounds; ++round)
                for (std::uint32_t layer = 0; layer < layers; ++layer)
                    for (std::uint32_t chunk = 0; chunk < chunks; ++chunk)
                        for (std::uint32_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal)
                            phases.push_back(((chunk / mounts.size()) * slot_bytes + 4096) % page);
            std::mt19937_64 phase_rng(19);
            std::shuffle(phases.begin(), phases.end(), phase_rng);
        }
        if (unique_random_lba) {
            std::vector<std::vector<std::uint32_t>> order(mounts.size());
            std::vector<std::size_t> cursor(mounts.size(), 0);
            for (std::size_t d = 0; d < mounts.size(); ++d) {
                auto& positions = order[d];
                positions.resize(slots);
                for (std::uint32_t i = 0; i < slots; ++i) positions[i] = i;
                std::mt19937_64 rng(7 + d);
                std::shuffle(positions.begin(), positions.end(), rng);
            }
            for (std::uint32_t round = 0; round < rounds; ++round)
                for (std::uint32_t layer = 0; layer < layers; ++layer)
                    for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
                        const std::size_t device = chunk % mounts.size();
                        for (std::uint32_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal) {
                            if (cursor[device] >= order[device].size())
                                FAIL("unique random footprint exhausted on disk %zu", device);
                            const std::size_t index =
                                ((static_cast<std::size_t>(round) * layers + layer) * chunks +
                                 chunk) * requests_per_chunk + ordinal;
                            const std::uint64_t phase = shuffle_kv_phases
                                ? phases[index] : match_kv_phase
                                ? ((chunk / mounts.size()) * slot_bytes + 4096) % page
                                : (fixed_phase_kb >= 0 ? static_cast<std::uint64_t>(fixed_phase_kb) * 1024 : 0);
                            const std::uint64_t candidate =
                                order[device][cursor[device]++] * page + phase;
                            random_offsets[index] = candidate + page <= file_bytes
                                ? candidate : candidate - page;
                        }
                    }
        } else {
            std::mt19937_64 rng(7);
            const std::uint64_t grain = random_align_kb ? static_cast<std::uint64_t>(random_align_kb) * 1024 : page;
            if (grain < page || grain % page) FAIL("--random-align-kb must be a multiple of the IO size");
            const std::uint64_t grains = (file_bytes - page) / grain + 1;
            for (std::uint32_t round = 0; round < rounds; ++round)
                for (std::uint32_t layer = 0; layer < layers; ++layer)
                    for (std::uint32_t chunk = 0; chunk < chunks; ++chunk)
                        for (std::uint32_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal) {
                            const std::size_t index =
                                ((static_cast<std::size_t>(round) * layers + layer) * chunks +
                                 chunk) * requests_per_chunk + ordinal;
                            const std::uint64_t phase = shuffle_kv_phases
                                ? phases[index] : match_kv_phase
                                ? ((chunk / mounts.size()) * slot_bytes + 4096) % page
                                : (fixed_phase_kb >= 0 ? static_cast<std::uint64_t>(fixed_phase_kb) * 1024 : 0);
                            const std::uint64_t candidate = (rng() % grains) * grain + phase;
                            random_offsets[index] = candidate + page <= file_bytes
                                ? candidate : candidate - page;
                        }
        }
    }
    void* raw_pool = nullptr;
    CUDA_OK(cudaMalloc(&raw_pool, pool_bytes + 65536));
    void* pool = reinterpret_cast<void*>(
        (reinterpret_cast<std::uintptr_t>(raw_pool) + 65535) & ~std::uintptr_t(65535));
    auto mem = rt->register_memory({pool, pool_bytes, MemoryKind::DEVICE,
                                    MemoryOwnership::CALLER_OWNED,
                                    rt->accel_id(),
                                    TUTTI_COMPILED_ACCELERATOR_PROFILE, page});
    if (!mem.ok()) FAIL("register_memory: %s", mem.status().message().c_str());
    const MemoryHandle memory_ticket = mem.value();

    cudaStream_t stream = nullptr;
    CUDA_OK(cudaStreamCreate(&stream));
    cudaEvent_t ev_start = nullptr, ev_end = nullptr;
    CUDA_OK(cudaEventCreate(&ev_start));
    CUDA_OK(cudaEventCreate(&ev_end));
    HostSubmitContext context{ExecutionDomain::DEVICE_EXECUTION,
                              rt->accel_id(), stream};
    for (std::size_t d = 0; d < mounts.size() && d < targets.size(); ++d) {
        IoRequest warm{IoDirection::READ, memory_ticket, 0, targets[d], 0, page};
        auto submitted = rt->submit(&warm, 1, context);
        if (!submitted.io.has_value()) FAIL("warmup submit: %s", submitted.status.message().c_str());
        auto waited = rt->wait(submitted.io.value(), 60000);
        if (!waited.result || waited.result->state != IoState::COMPLETED) FAIL("warmup wait failed");
        rt->release_io(submitted.io.value());
    }
    if (!barrier.empty()) {
        const std::string ready = barrier + ".ready." + std::to_string(static_cast<long>(::getpid()));
        const int fd = ::open(ready.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) FAIL("barrier ready %s: %s", ready.c_str(), std::strerror(errno));
        ::close(fd);
        const auto waiting = std::chrono::steady_clock::now();
        while (::access((barrier + ".go").c_str(), F_OK) != 0) {
            if (elapsed(waiting) > 600) FAIL("barrier timed out: %s", barrier.c_str());
            ::usleep(1000);
        }
    }
    const std::int64_t start_ns = epoch_ns();
    auto start = std::chrono::steady_clock::now();
    std::uint64_t bytes_done = 0;
    double build_seconds = 0;
    double submit_wait_seconds = 0;
    double submit_seconds = 0;
    double wait_seconds = 0;
    double device_seconds = 0;
    std::vector<IoRequest> interleaved;
    if (interleave_layers || shuffle_across_layers || shuffle_per_disk) {
        // Same requests and number/size of submits as layer mode. Rotating
        // layer for each chunk breaks the global layer fence while retaining
        // per-chunk on-disk offsets and the same overall LBA footprint.
        interleaved.reserve(static_cast<std::size_t>(entries_per_layer) * layers);
        for (std::uint32_t batch = 0; batch < layers; ++batch) {
            for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
                const std::uint32_t layer = interleave_layers ? (batch + chunk) % layers : batch;
                const std::uint32_t device = chunk % mounts.size();
                const std::uint32_t per_device_slot = chunk / mounts.size();
                const std::uint32_t file = per_device_slot / slots_per_file;
                const std::uint32_t slot = slot_order[
                    (static_cast<std::uint64_t>(per_device_slot % slots_per_file) *
                     slot_permutation) % slots_per_file];
                const std::uint32_t physical_layer = random_layer_per_chunk
                    ? layer_order[static_cast<std::size_t>(chunk) * layers + layer]
                    : (rotate_layer_per_chunk ? (layer + chunk) % layers : layer);
                const std::uint64_t base = metadata_zone
                    ? payload_zone_offset + static_cast<std::uint64_t>(slot) * payload_bytes +
                          static_cast<std::uint64_t>(physical_layer) * segment_bytes
                    : static_cast<std::uint64_t>(slot) * slot_bytes + header_bytes +
                          static_cast<std::uint64_t>(physical_layer) * segment_bytes;
                for (std::uint32_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal) {
                    const std::uint64_t mem_offset =
                        ((static_cast<std::uint64_t>(chunk) * requests_per_chunk + ordinal) *
                         5120 * 1024 + static_cast<std::uint64_t>(layer) * page) % pool_bytes;
                    interleaved.push_back({IoDirection::READ, memory_ticket, mem_offset,
                                           targets[file * mounts.size() + device],
                                           base + static_cast<std::uint64_t>(ordinal) * page,
                                           page});
                }
            }
        }
        if (shuffle_across_layers) {
            std::mt19937_64 rng(7);
            std::shuffle(interleaved.begin(), interleaved.end(), rng);
        } else if (shuffle_per_disk) {
            std::vector<std::vector<IoRequest>> per_disk(mounts.size());
            for (std::size_t batch = 0; batch < layers; ++batch)
                for (std::size_t i = 0; i < entries_per_layer; ++i) {
                    const std::size_t device =
                        (i / requests_per_chunk) % mounts.size();
                    per_disk[device].push_back(interleaved[batch * entries_per_layer + i]);
                }
            for (std::size_t d = 0; d < mounts.size(); ++d) {
                std::mt19937_64 rng(7 + d);
                std::shuffle(per_disk[d].begin(), per_disk[d].end(), rng);
            }
            std::vector<std::size_t> cursor(mounts.size(), 0);
            for (std::size_t batch = 0; batch < layers; ++batch)
                for (std::size_t i = 0; i < entries_per_layer; ++i) {
                    const std::size_t device =
                        (i / requests_per_chunk) % mounts.size();
                    interleaved[batch * entries_per_layer + i] =
                        per_disk[device][cursor[device]++];
                }
        }
    }
    for (std::uint32_t round = 0; round < rounds; ++round) {
        for (std::uint32_t layer = 0; layer < layers; ++layer) {
            auto build_start = std::chrono::steady_clock::now();
            std::vector<IoRequest> reqs;
            if (interleave_layers || shuffle_across_layers || shuffle_per_disk) {
                const auto first = interleaved.begin() +
                    static_cast<std::size_t>(layer) * entries_per_layer;
                reqs.assign(first, first + entries_per_layer);
            } else {
                reqs.reserve(entries_per_layer);
                for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
                const std::uint32_t device = chunk % mounts.size();
                const std::uint32_t per_device_slot = chunk / mounts.size();
                const std::uint32_t file = per_device_slot / slots_per_file;
                const std::uint32_t slot = slot_order[
                    (static_cast<std::uint64_t>(per_device_slot % slots_per_file) *
                     slot_permutation) % slots_per_file];
                const std::uint32_t physical_layer = random_layer_per_chunk
                    ? layer_order[static_cast<std::size_t>(chunk) * layers + layer]
                    : (rotate_layer_per_chunk ? (layer + chunk) % layers : layer);
                const std::uint64_t base = metadata_zone
                    ? payload_zone_offset + static_cast<std::uint64_t>(slot) * payload_bytes +
                          static_cast<std::uint64_t>(physical_layer) * segment_bytes
                    : static_cast<std::uint64_t>(slot) * addr_stride + addr_base +
                          static_cast<std::uint64_t>(physical_layer) * segment_bytes;
                for (std::uint32_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal) {
                    const std::uint64_t mem_offset =
                        ((static_cast<std::uint64_t>(chunk) * requests_per_chunk + ordinal) *
                         5120 * 1024 + static_cast<std::uint64_t>(layer) * page) % pool_bytes;
                    const std::size_t request_id =
                        ((static_cast<std::size_t>(round) * layers + layer) * chunks +
                         chunk) * requests_per_chunk + ordinal;
                    IoRequest request{IoDirection::READ, memory_ticket, mem_offset,
                                      targets[file * mounts.size() + device],
                                      random_lba ? random_offsets[request_id] :
                                          base + static_cast<std::uint64_t>(ordinal) * page,
                                      page};
                    reqs.push_back(request);
                }
            }
            }
            if (shuffle_requests) {
                std::mt19937_64 rng(7 + static_cast<std::uint64_t>(round) * layers + layer);
                std::shuffle(reqs.begin(), reqs.end(), rng);
            }
            if (group_by_disk) {
                std::vector<IoRequest> grouped;
                grouped.reserve(reqs.size());
                for (std::size_t d = 0; d < mounts.size(); ++d)
                    for (std::size_t i = d * requests_per_chunk;
                         i < reqs.size(); i += mounts.size() * requests_per_chunk)
                        for (std::size_t ordinal = 0; ordinal < requests_per_chunk; ++ordinal)
                            grouped.push_back(reqs[i + ordinal]);
                reqs.swap(grouped);
            }
            build_seconds += elapsed(build_start);
            auto batch_start = std::chrono::steady_clock::now();
            CUDA_OK(cudaEventRecord(ev_start, stream));
            auto submitted = rt->submit(reqs.data(), reqs.size(), context);
            if (!submitted.io.has_value()) FAIL("submit: %s", submitted.status.message().c_str());
            submit_seconds += elapsed(batch_start);
            CUDA_OK(cudaEventRecord(ev_end, stream));
            auto wait_start = std::chrono::steady_clock::now();
            auto waited = rt->wait(submitted.io.value(), 60000);
            if (!waited.result || waited.result->state != IoState::COMPLETED) FAIL("wait failed");
            wait_seconds += elapsed(wait_start);
            for (std::size_t i = 0; i < submitted.initial_states.size(); ++i) {
                if (submitted.initial_states[i].state != IoRequestState::ACCEPTED)
                    FAIL("entry %zu rejected: %s", i,
                         submitted.initial_states[i].status.message().c_str());
            }
            rt->release_io(submitted.io.value());
            float device_ms = 0;
            CUDA_OK(cudaEventElapsedTime(&device_ms, ev_start, ev_end));
            device_seconds += device_ms / 1000.0;
            submit_wait_seconds += elapsed(batch_start);
            bytes_done += reqs.size() * page;
        }
    }
    const double seconds = elapsed(start);
    OK("LOOP epoch_start_ns=%lld epoch_end_ns=%lld bytes=%llu",
       static_cast<long long>(start_ns), static_cast<long long>(epoch_ns()),
       static_cast<unsigned long long>(bytes_done));
    OK("packed-segment throughput=%.2f GiB/s wall=%.3fs bytes=%.2f GiB",
       static_cast<double>(bytes_done) / (1ull << 30) / seconds, seconds,
       static_cast<double>(bytes_done) / (1ull << 30));
    OK("PHASE build=%.3fs submit_wait=%.3fs other=%.3fs batches=%u",
       build_seconds, submit_wait_seconds,
       seconds - build_seconds - submit_wait_seconds, rounds * layers);
    OK("TIMING host_submit=%.3fs host_wait=%.3fs device_stream=%.3fs",
       submit_seconds, wait_seconds, device_seconds);
    CUDA_OK(cudaEventDestroy(ev_start));
    CUDA_OK(cudaEventDestroy(ev_end));
    rt->unregister_memory(memory_ticket);
    cudaFree(raw_pool);
    return 0;
}
