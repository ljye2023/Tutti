// tests/config_loader/config_loader_test.cpp
//
// Round 20 S1 — tutti_config loader unit tests.
// Pure host-side (no GPU/snsvm needed); tests:
//   1. parse priority chain: programmatic > config > env > default
//   2. RDMA placeholder → UNSUPPORTED
//   3. bad yaml → fail-closed
//   4. valid parse of all keys

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <tutti/config/tutti_config.h>

using namespace tutti::config;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { ++g_pass; } \
    else { std::printf("  FAIL: %s\n", msg); ++g_fail; } \
} while(0)

static std::string write_tmp(const std::string& content) {
    char tmpl[] = "/tmp/tutti_cfg_XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    write(fd, content.data(), content.size());
    close(fd);
    return std::string(tmpl);
}

int main() {
    // 1. Valid parse — all keys.
    {
        std::string yaml = R"(
gpu:
  vendor: nvidia
storage:
  backend: local-nvme
  default_stripe_unit: 524288
local_nvme:
  handle_cache_capacity: 64
  prp_cache_capacity: 128
  handle_cache_l2_capacity: 256
  max_in_flight_operations: 32
  max_batch_entries: 512
  num_user_queues: 8
  io_granularity: 524288
local_nvme_config: local_nvme_config.yaml
)";
        auto path = write_tmp(yaml);
        auto r = parse_tutti_config(path);
        CHECK(r.ok(), "valid parse: ok");
        if (r.ok()) {
            const auto& c = r.value();
            CHECK(c.gpu_vendor == "nvidia", "gpu_vendor");
            CHECK(c.storage_backend == "local-nvme", "storage_backend");
            CHECK(c.default_stripe_unit == 524288, "stripe_unit");
            CHECK(c.handle_cache_capacity == 64, "handle_cache_capacity");
            CHECK(c.prp_cache_capacity == 128, "prp_cache_capacity");
            CHECK(c.handle_cache_l2_capacity == 256, "l2_capacity");
            CHECK(c.max_in_flight_operations == 32, "max_in_flight");
            CHECK(c.max_batch_entries == 512, "max_batch_entries");
            CHECK(c.num_user_queues == 8, "num_user_queues");
            CHECK(c.io_granularity == 524288, "io_granularity");
            CHECK(c.local_nvme_config == "local_nvme_config.yaml",
                  "local_nvme_config link");
        }
        ::unlink(path.c_str());
    }

    // 2. RDMA placeholder → UNSUPPORTED.
    {
        std::string yaml = "storage:\n  backend: rdma\n";
        auto path = write_tmp(yaml);
        auto r = parse_tutti_config(path);
        CHECK(!r.ok(), "rdma: fail-closed");
        if (!r.ok()) {
            CHECK((int)r.status().code() == (int)tutti::StatusCode::UNSUPPORTED,
                  "rdma: UNSUPPORTED status");
        }
        ::unlink(path.c_str());
    }

    // 3. Bad yaml → fail-closed.
    {
        std::string yaml = "this is not: [valid: yaml\n";
        auto path = write_tmp(yaml);
        auto r = parse_tutti_config(path);
        CHECK(!r.ok(), "bad yaml: fail-closed");
        ::unlink(path.c_str());
    }

    // 4. Empty/minimal config → defaults.
    {
        std::string yaml = "";
        auto path = write_tmp(yaml);
        auto r = parse_tutti_config(path);
        CHECK(r.ok(), "empty yaml: ok (defaults)");
        if (r.ok()) {
            const auto& c = r.value();
            CHECK(c.gpu_vendor == "nvidia", "default vendor");
            CHECK(c.storage_backend == "local-nvme", "default backend");
            CHECK(c.handle_cache_capacity == 0, "default cache cap=0");
            CHECK(c.local_nvme_config.empty(), "default no link");
        }
        ::unlink(path.c_str());
    }

    // 5. Priority chain: programmatic > config > env > default.
    {
        // Config sets handle_cache_capacity=64.
        std::string yaml = "local_nvme:\n  handle_cache_capacity: 64\n";
        auto path = write_tmp(yaml);
        auto r = parse_tutti_config(path);
        CHECK(r.ok(), "priority: parse ok");
        if (r.ok()) {
            // 5a. programmatic override > config
            ProgrammaticOverrides ov;
            ov.handle_cache_capacity = 100;
            auto eff = resolve_cache_config(r.value(), ov);
            CHECK(eff.handle_cache_capacity == 100,
                  "priority: programmatic > config");

            // 5b. config > env (set env, no programmatic)
            setenv("TUTTI_HANDLE_CACHE_CAP", "200", 1);
            ProgrammaticOverrides ov2;  // all 0 = defer
            auto eff2 = resolve_cache_config(r.value(), ov2);
            CHECK(eff2.handle_cache_capacity == 64,
                  "priority: config > env");
            unsetenv("TUTTI_HANDLE_CACHE_CAP");

            // 5c. env > default (no config key, no programmatic)
            ParsedConfig empty;
            setenv("TUTTI_HANDLE_CACHE_CAP", "300", 1);
            auto eff3 = resolve_cache_config(empty, {});
            CHECK(eff3.handle_cache_capacity == 300,
                  "priority: env > default");
            unsetenv("TUTTI_HANDLE_CACHE_CAP");

            // 5d. default when nothing set
            auto eff4 = resolve_cache_config(empty, {});
            CHECK(eff4.handle_cache_capacity == 0,
                  "priority: default = 0");
        }
        ::unlink(path.c_str());
    }

    // 6. Non-existent file → fail-closed.
    {
        auto r = parse_tutti_config("/tmp/tutti_nonexistent_config.yaml");
        CHECK(!r.ok(), "non-existent file: fail-closed");
    }

    // 7. derive_local_nvme_devices — topology from nvmes[]+allowed_gpus.
    {
        std::string yaml = R"(
nvmes:
  - pci_addr: "0000:08:00.0"
    namespace_id: 1
    allowed_gpus: [0]
  - pci_addr: "0000:4b:00.0"
    namespace_id: 2
    allowed_gpus: [1, 2]
  - pci_addr: "0000:57:00.0"
)";
        auto path = write_tmp(yaml);
        auto r = derive_local_nvme_devices(path);
        CHECK(r.ok(), "derive: ok");
        if (r.ok()) {
            const auto& d = r.value();
            CHECK(d.size() == 3, "derive: 3 specs (1+2, third skipped)");
            if (d.size() == 3) {
                CHECK(d[0].snvme_dev == "/dev/ssnvme0" &&
                      d[0].cuda_device == 0, "derive: nvme0 -> gpu0");
                CHECK(d[1].snvme_dev == "/dev/ssnvme1" &&
                      d[1].cuda_device == 1 &&
                      d[1].namespace_id == 2, "derive: nvme1 -> gpu1 ns2");
                CHECK(d[2].cuda_device == 2, "derive: nvme1 -> gpu2");
            }
        }
        ::unlink(path.c_str());

        auto r2 = derive_local_nvme_devices("/tmp/tutti_nonexistent_lnvc.yaml");
        CHECK(!r2.ok(), "derive: missing file fail-closed");
    }

    std::printf("\n=== Summary ===\n  passed: %d\n  failed: %d\n", g_pass, g_fail);
    if (g_fail > 0) { std::printf("RESULT: FAIL\n"); return 1; }
    std::printf("RESULT: PASS\n");
    return 0;
}
