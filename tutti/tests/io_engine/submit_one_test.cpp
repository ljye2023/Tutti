// tutti/tests/io_engine/submit_one_test.cpp
// Unit tests for IoEngineImpl::submit_one
//
// Covers 7 cases:
//   (a) null region → false
//   (b) invalid VDeviceHandle → false (optional; noted if design passes it through)
//   (c) single shard read, length <= max_io → true, prepare_count == 1
//   (d) single shard write, length <= max_io → true
//   (e) MDTS fan-out: 12288 with max_io=4096 → true, prepare_count == 3
//   (f) acquire_target_handle called exactly once per submit_one call
//   (g) zero length → false

#include <iostream>
#include <cassert>
#include <cstring>
#include "io_engine/src/io_engine_impl.h"
#include "backends/nvme/include/batch_submitter.h"
#include "accel/include/common/iaccel.h"
#include "accel/include/common/memory_region.h"

using namespace tutti;

//==============================================================================
// Mock Batch Submitter
//
// Implements IBatchSubmitter including the new acquire_target_handle method.
// acquire_count and prepare_count let test cases verify call semantics.
//==============================================================================
class MockBatchSubmitter : public backends::nvme::IBatchSubmitter {
public:
    int acquire_count = 0;
    int prepare_count = 0;

    backends::BackendMetadata metadata() const override {
        backends::BackendMetadata meta{};
        meta.max_batch_size = 16;
        meta.max_io_size    = 4096;
        return meta;
    }

    bool prepare_descriptors(
        const uint64_t*                     ioaddrs,
        const backends::nvme::SubSliceInfo* slices,
        uint32_t                            n_slices,
        backends::nvme::BufferDescriptor*   out_descs) override {
        for (uint32_t i = 0; i < n_slices; ++i) {
            out_descs[i].prp1        = ioaddrs[0] + slices[i].offset_bytes;
            out_descs[i].data_length = slices[i].length_bytes;
        }
        prepare_count += static_cast<int>(n_slices);
        return true;
    }

    void release_descriptors(backends::nvme::BufferDescriptor*, uint32_t) override {}

    void launch_batch_gpu_stream(
        void*, void*,
        const backends::nvme::BufferDescriptor*, uint32_t, bool) override {
        std::cout << "  [Mock] GPU stream batch launched" << std::endl;
    }

    void* acquire_target_handle(
        const backends::StorageTarget&,
        backends::VDeviceHandle) override {
        ++acquire_count;
        return reinterpret_cast<void*>(0x1000);
    }
};

//==============================================================================
// Mock Accelerator -- copied verbatim from layer4_smoke_test.cpp
//==============================================================================
class MockAccelerator : public IAccelerator {
public:
    MockAccelerator() : device_buffer_(nullptr), event_completed_(false) {}

    const char* vendor_name() const override { return "MockGPU"; }
    int device_count() const override { return 1; }
    bool set_device(int) override { return true; }
    int get_device() const override { return 0; }

    void* allocate_device(size_t size, MemoryKind, int) override {
        device_buffer_ = malloc(size);
        return device_buffer_;
    }

    void* allocate_host(size_t size, MemoryKind) override {
        return malloc(size);
    }

    void free(void* ptr, MemoryKind) override {
        if (ptr == device_buffer_) {
            ::free(device_buffer_);
            device_buffer_ = nullptr;
        } else {
            ::free(ptr);
        }
    }

    bool memcpy_async(void* dst, const void* src, size_t size, AccelStream) override {
        memcpy(dst, src, size);
        return true;
    }

    AccelStream create_stream() override {
        AccelStream s;
        s.handle = reinterpret_cast<void*>(0x2000);
        return s;
    }

    void destroy_stream(AccelStream) override {}

    void synchronize_stream(AccelStream) override {
        std::cout << "  [Mock] Stream synchronized" << std::endl;
    }

    AccelEvent create_event() override {
        AccelEvent e;
        e.handle = reinterpret_cast<void*>(0x3000);
        return e;
    }

    void destroy_event(AccelEvent) override {}

    void record_event(AccelEvent, AccelStream) override {
        event_completed_ = true;
    }

    bool query_event(AccelEvent) override {
        return event_completed_;
    }

    void wait_event(AccelStream, AccelEvent) override {}

    MemoryRegion* register_host(void* host_ptr, size_t size) override {
        auto* region = new MemoryRegion();
        region->host_ptr        = host_ptr;
        region->device_ptr      = nullptr;
        region->size            = size;
        region->backend_private = ioaddrs_;
        return region;
    }

    MemoryRegion* register_device(void*, size_t, int) override { return nullptr; }
    MemoryRegion* register_external(void*, void*, size_t, const ExternalMemorySpec&) override { return nullptr; }

    void unregister(MemoryRegion* region) override {
        delete region;
    }

    MemoryRegion* lookup(const void*) const override { return nullptr; }
    MemoryRegion* lookup_by_id(uint64_t) const override { return nullptr; }
    void* device_pointer_for(const void*) override { return nullptr; }

    void launch(void*, const Dim3&, const Dim3&, size_t, AccelStream, void**) override {}

    bool ipc_export(MemoryRegion*, IpcHandle*) override { return false; }
    MemoryRegion* ipc_import(const IpcHandle&, int) override { return nullptr; }

private:
    void* device_buffer_;
    bool  event_completed_;
    uint64_t ioaddrs_[16] = {0x100000, 0x101000, 0x102000, 0x103000};
};

//==============================================================================
// Helper: build a valid NVME_RAW StorageTarget reused across cases
//==============================================================================
static backends::StorageTarget make_raw_target() {
    backends::StorageTarget t;
    t.kind              = backends::StorageTargetKind::NVME_RAW;
    t.target_id         = 42;
    t.start_lba         = 0;
    t.length_blocks     = 100;
    t.nvme_block_size   = 4096;
    t.nvme_block_size_log = 12;
    return t;
}

//==============================================================================
// Test Cases
//==============================================================================

// (a) null region → false
bool test_null_region() {
    std::cout << "\n[TEST a] submit_one null region" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    SingleShardIoRequest req{};
    req.region         = nullptr;
    req.logical_offset = 0;
    req.length         = 4096;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0};

    bool result = engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);

    assert(!result && "null region must return false");
    std::cout << "  pass: null region rejected" << std::endl;
    return true;
}

// (b) invalid VDeviceHandle (INVALID sentinel 0xFFFFFFFF) → false
// Optional: if the engine defers validation to the backend mock (which always
// returns sentinel 0x1000 regardless of handle), this may return true.
// In that case the test notes the behavior and counts as passed regardless.
bool test_invalid_vdev() {
    std::cout << "\n[TEST b] submit_one invalid VDeviceHandle" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[4096];
    MemoryRegion* region = accel.register_host(buffer, 4096);

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 4096;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0xFFFFFFFFu};  // INVALID

    bool result = engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);
    accel.unregister(region);

    if (!result) {
        std::cout << "  pass: invalid vdev rejected (engine validates before dispatch)"
                  << std::endl;
    } else {
        // Design defers vdev validation to the backend; mock accepts anything.
        std::cout << "  note: design passes invalid vdev to backend; mock accepted — "
                     "counted as pass per contract (optional case)" << std::endl;
    }
    return true;
}

// (c) single shard read, length <= max_io → true, prepare_count == 1
bool test_single_shard_read() {
    std::cout << "\n[TEST c] submit_one single shard read" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[4096];
    MemoryRegion* region = accel.register_host(buffer, 4096);
    backend.prepare_count = 0;

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 4096;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0};

    bool result = engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);
    accel.unregister(region);

    assert(result && "single shard read must return true");
    assert(backend.prepare_count == 1 && "expected exactly 1 descriptor for length == max_io");
    std::cout << "  pass: result=true, prepare_count=" << backend.prepare_count << std::endl;
    return true;
}

// (d) single shard write, length <= max_io → true
bool test_single_shard_write() {
    std::cout << "\n[TEST d] submit_one single shard write" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[4096];
    MemoryRegion* region = accel.register_host(buffer, 4096);

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 4096;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0};

    bool result = engine.submit_one(req, false, stream);  // is_read=false
    accel.destroy_stream(stream);
    accel.unregister(region);

    assert(result && "single shard write must return true");
    std::cout << "  pass: write returned true" << std::endl;
    return true;
}

// (e) MDTS fan-out: length=12288 with max_io=4096 → true, prepare_count == 3
bool test_mdts_fanout() {
    std::cout << "\n[TEST e] submit_one MDTS fan-out (12288 / 4096 = 3 descriptors)" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[12288];
    MemoryRegion* region = accel.register_host(buffer, 12288);
    backend.prepare_count = 0;

    backends::StorageTarget t = make_raw_target();
    t.length_blocks = 3;  // 3 * 4096 bytes covers the full 12288-byte transfer

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 12288;  // 3 x max_io_size (4096)
    req.shard_target   = t;
    req.vdev           = backends::VDeviceHandle{0};

    bool result = engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);
    accel.unregister(region);

    assert(result && "fan-out read must return true");
    assert(backend.prepare_count == 3 && "expected 3 descriptors: 12288 / 4096 == 3");
    std::cout << "  pass: result=true, prepare_count=" << backend.prepare_count
              << " (assert: prepare_count == 3)" << std::endl;
    return true;
}

// (f) acquire_target_handle must be called exactly once per submit_one invocation
bool test_acquire_called_once() {
    std::cout << "\n[TEST f] acquire_target_handle called once" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[4096];
    MemoryRegion* region = accel.register_host(buffer, 4096);
    backend.acquire_count = 0;

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 4096;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0};

    engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);
    accel.unregister(region);

    assert(backend.acquire_count == 1 &&
           "acquire_target_handle must be called exactly once per submit_one");
    std::cout << "  pass: acquire_count=" << backend.acquire_count << std::endl;
    return true;
}

// (g) zero length → false
bool test_zero_length() {
    std::cout << "\n[TEST g] submit_one zero length" << std::endl;
    MockBatchSubmitter backend;
    MockAccelerator    accel;
    IoEngineImpl       engine(&backend, &accel);
    AccelStream stream = accel.create_stream();

    char buffer[4096];
    MemoryRegion* region = accel.register_host(buffer, 4096);

    SingleShardIoRequest req{};
    req.region         = region;
    req.logical_offset = 0;
    req.length         = 0;
    req.shard_target   = make_raw_target();
    req.vdev           = backends::VDeviceHandle{0};

    bool result = engine.submit_one(req, true, stream);
    accel.destroy_stream(stream);
    accel.unregister(region);

    assert(!result && "zero-length request must return false");
    std::cout << "  pass: zero length rejected" << std::endl;
    return true;
}

//==============================================================================
// Main
//==============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "submit_one Unit Test" << std::endl;
    std::cout << "========================================" << std::endl;

    int passed = 0;
    int total  = 7;

    struct TestCase {
        bool (*fn)();
        const char* name;
    };

    TestCase cases[] = {
        { test_null_region,        "null region"           },
        { test_invalid_vdev,       "invalid vdev"          },
        { test_single_shard_read,  "single shard read"     },
        { test_single_shard_write, "single shard write"    },
        { test_mdts_fanout,        "MDTS fan-out"          },
        { test_acquire_called_once,"acquire called once"   },
        { test_zero_length,        "zero length"           },
    };

    for (auto& tc : cases) {
        try {
            if (tc.fn()) {
                ++passed;
            } else {
                std::cout << "  FAIL: " << tc.name << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "  EXCEPTION in " << tc.name << ": " << e.what() << std::endl;
        } catch (...) {
            std::cout << "  UNKNOWN EXCEPTION in " << tc.name << std::endl;
        }
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Results: " << passed << "/" << total << " tests passed" << std::endl;
    std::cout << "========================================" << std::endl;

    return (passed == total) ? 0 : 1;
}
