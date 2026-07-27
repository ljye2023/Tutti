// nvme_backend_test.cpp -- NvmeBackend Layer 3 tests (unit + real-HW)
//
// Unit tier (always runs, no hardware or daemon):
//   NvmeBackendUnit.*
//   Drives NvmeBackend through IDeviceManager + DaemonNvmeDeviceDriver in
//   mock_mode. Covers initialize/shutdown lifecycle, roster accessors, metadata,
//   and factory registration. Mock mode returns d_qps=nullptr and zero
//   blk_size/max_data_size; NvmeBackend guards against zero in initialize().
//
// Real-HW tier (requires TUTTI_NVME_REAL_HW=1 + live daemon):
//   NvmeBackendRealHw.*
//   Instantiates NvmeBackend against a live nvmeservice daemon and asserts that
//   initialize() populates the vdevice roster with live NvmeVirtualDevice
//   objects (d_qps non-null), acquire_target_handle() succeeds, and
//   prepare_descriptors() builds valid PRP descriptors.
//
// Env knobs (real-HW tier):
//   TUTTI_NVME_REAL_HW=1        required to run any real-HW case
//   TUTTI_NVME_ENDPOINT         daemon gRPC endpoint  (default 127.0.0.1:50051)

#include "backends/nvme/include/nvme_backend.h"
#include "backends/include/backend_factory.h"
#include "backends/include/storage_target.h"

#include "common/device_manager_impl.h"
#include "daemon_nvme_device_driver.h"
#include "nvme_virtual_device.h"
#include "mock_lease_manager.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstdio>
#include <cuda_runtime.h>
#include <nvm_dma.h>   // nvm_dma_map_data_device, nvm_dma_unmap
#include <memory>
#include <string>
#include <vector>

using namespace tutti;
using namespace tutti::backends;
using namespace tutti::backends::nvme;

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

bool env_flag(const char* key) {
    const char* v = std::getenv(key);
    return v && std::string(v) == "1";
}

// Build an IDeviceManager with one DaemonNvmeDeviceDriver.
std::unique_ptr<IDeviceManager> make_manager(
    ILeaseManager* lease,
    const std::string& addr,
    bool mock_mode)
{
    std::vector<std::unique_ptr<IDeviceDriver>> drivers;
    drivers.push_back(std::make_unique<DaemonNvmeDeviceDriver>(
        /*accel=*/nullptr, lease, addr, mock_mode));
    return create_device_manager(std::move(drivers));
}

BackendConfig cfg(int32_t phys_id, uint32_t vdev_count, uint32_t quota) {
    BackendConfig c;
    c.phys_id = phys_id;
    c.vdevice_count = vdev_count;
    c.quota_per_vdevice = quota;
    return c;
}

}  // namespace

// ===========================================================================
// Unit tier -- mock mode, no hardware / daemon
// ===========================================================================

TEST(NvmeBackendUnit, InitializeAndShutdownLifecycle) {
    MockLeaseManager lease;
    auto mgr = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), /*count=*/2, /*quota=*/4)));
    EXPECT_EQ(be.vdevice_count(), 2u);
    EXPECT_EQ(be.backend_type(), BackendType::LOCAL_NVME);

    be.shutdown();
    EXPECT_EQ(be.vdevice_count(), 0u);
}

TEST(NvmeBackendUnit, InitializeRejectsNullDm) {
    NvmeBackend be;
    EXPECT_FALSE(be.initialize(nullptr, cfg(0, 1, 1)));
    EXPECT_EQ(be.vdevice_count(), 0u);
}

TEST(NvmeBackendUnit, InitializeRejectsZeroCount) {
    MockLeaseManager lease;
    auto mgr = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    EXPECT_FALSE(be.initialize(mgr.get(), cfg(phys->id(), /*count=*/0, /*quota=*/1)));
}

TEST(NvmeBackendUnit, ShutdownIsIdempotent) {
    MockLeaseManager lease;
    auto mgr = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));
    be.shutdown();
    be.shutdown();  // second call must be no-op
    EXPECT_EQ(be.vdevice_count(), 0u);
}

TEST(NvmeBackendUnit, RosterAccessors) {
    MockLeaseManager lease;
    auto mgr = make_manager(&lease, "127.0.0.1:59999", /*mock_mode=*/true);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 3, 2)));

    // In-range accessors
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_NE(be.vdevice_at(i), nullptr);
        VDeviceHandle h = be.vdevice_handle_at(i);
        EXPECT_TRUE(h.is_valid());
        EXPECT_EQ(h.index, i);
    }

    // Out-of-range accessors
    EXPECT_EQ(be.vdevice_at(3), nullptr);
    EXPECT_FALSE(be.vdevice_handle_at(3).is_valid());
}

TEST(NvmeBackendUnit, MetadataIdentity) {
    NvmeBackend be;
    EXPECT_EQ(be.backend_type(), BackendType::LOCAL_NVME);
    EXPECT_STREQ(be.backend_name(), "local_nvme");

    BackendMetadata m = be.metadata();
    EXPECT_STREQ(m.name, "local_nvme");
    EXPECT_EQ(m.type, BackendType::LOCAL_NVME);
    EXPECT_EQ(m.capabilities, SUPPORTS_GPUDIRECT);
}

TEST(NvmeBackendUnit, FactoryRegistration) {
    EXPECT_TRUE(BackendFactory::is_registered(BackendType::LOCAL_NVME));

    auto types = BackendFactory::available_backends();
    EXPECT_NE(std::find(types.begin(), types.end(), BackendType::LOCAL_NVME),
              types.end());
}

TEST(NvmeBackendUnit, FactoryCreateReturnsNvmeBackend) {
    auto be = BackendFactory::create_backend(BackendType::LOCAL_NVME);
    ASSERT_NE(be, nullptr);
    EXPECT_EQ(be->backend_type(), BackendType::LOCAL_NVME);
}

// ===========================================================================
// Real-HW tier -- live daemon (TUTTI_NVME_REAL_HW=1)
// ===========================================================================

class NvmeBackendRealHw : public ::testing::Test {
protected:
    void SetUp() override {
        if (!env_flag("TUTTI_NVME_REAL_HW")) {
            GTEST_SKIP() << "TUTTI_NVME_REAL_HW != 1";
        }
#ifndef TUTTI_NVMESERVICE_ENABLED
        GTEST_SKIP() << "built without TUTTI_NVMESERVICE_ENABLED "
                        "(gRPC not found at configure time)";
#endif
        endpoint_ = env_or("TUTTI_NVME_ENDPOINT", "127.0.0.1:50051");
    }

    std::string      endpoint_{"127.0.0.1:50051"};
    MockLeaseManager lease_;
};

TEST_F(NvmeBackendRealHw, InitializeAcquiresLiveQueues) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open()) << "Open() failed -- is daemon running at "
                             << endpoint_ << "?";

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    ASSERT_GT(mgr->available_resources(phys->id()), 0u);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));
    EXPECT_EQ(be.vdevice_count(), 1u);

    // Downcast to NvmeVirtualDevice and check live GPU queues
    IVirtualDevice* v = be.vdevice_at(0);
    ASSERT_NE(v, nullptr);
    ASSERT_EQ(v->type(), DeviceType::LOCAL_NVME);

    auto* nvme = static_cast<NvmeVirtualDevice*>(v);
    EXPECT_NE(nvme->d_qps, nullptr) << "d_qps is null -- queue allocation failed";
    EXPECT_EQ(nvme->queue_quota, 1u);

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, RosterHoldsNvmeVirtualDevice) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 2, 2)));

    for (uint32_t i = 0; i < 2; ++i) {
        IVirtualDevice* v = be.vdevice_at(i);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(v->type(), DeviceType::LOCAL_NVME);
        EXPECT_EQ(v->resource_count(), 2u);

        auto* nvme = static_cast<NvmeVirtualDevice*>(v);
        EXPECT_NE(nvme->d_qps, nullptr);
        EXPECT_EQ(nvme->queue_quota, 2u);
    }

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, MetadataPopulatedFromDevice) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));

    auto* nvme = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    ASSERT_NE(nvme, nullptr);

    // Namespace metadata populated from daemon session
    EXPECT_GT(nvme->namespace_id, 0u);
    EXPECT_GT(nvme->blk_size, 0u);
    EXPECT_GT(nvme->blk_size_log, 0u);

    BackendMetadata m = be.metadata();
    EXPECT_GT(m.max_io_size, 0u);
    EXPECT_GT(m.alignment_bytes, 0u);

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, MultipleVdevicesDistinctQueueSlices) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);
    ASSERT_GE(mgr->available_resources(phys->id()), 2u);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 2, 1)));

    auto* nvme0 = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    auto* nvme1 = static_cast<NvmeVirtualDevice*>(be.vdevice_at(1));
    ASSERT_NE(nvme0, nullptr);
    ASSERT_NE(nvme1, nullptr);

    // Two vdevices must have distinct queue slices
    EXPECT_NE(nvme0->d_qps, nvme1->d_qps);
    EXPECT_NE(nvme0->vdev_id(), nvme1->vdev_id());

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, AcquireReleaseTargetHandle) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));

    auto* nvme = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    ASSERT_NE(nvme, nullptr);

    // Build a synthetic StorageTarget
    StorageTarget target;
    target.kind = StorageTargetKind::NVME_FILE;
    target.target_id = 12345;
    target.logical_size_bytes = 1024 * 1024;
    target.namespace_id = nvme->namespace_id;
    target.nvme_block_size = nvme->blk_size;
    target.nvme_block_size_log = nvme->blk_size_log;

    // Single extent: LBA 0, 256 blocks
    LbaExtent extent;
    extent.start_lba = 0;
    extent.length_blocks = 256;
    extent.logical_offset = 0;
    target.num_extents = 1;
    target.extents = &extent;

    // Acquire handle
    VDeviceHandle hdl = be.vdevice_handle_at(0);
    void* handle = be.acquire_target_handle(target, hdl);
    ASSERT_NE(handle, nullptr) << "acquire_target_handle failed";

    // Release handle
    be.release_target_handle(handle);

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, PrepareAndReleaseDescriptors) {
    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open());

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 4)));

    auto* nvme = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    ASSERT_NE(nvme, nullptr);

    // Build fake ioaddrs (4K-aligned addresses for 3 pages)
    constexpr uint32_t PAGE_SIZE = 4096;
    uint64_t fake_ioaddrs[3] = {
        0x1000ULL,  // page 0
        0x2000ULL,  // page 1
        0x3000ULL   // page 2
    };

    // Single sub-slice: 8KB transfer (2 pages)
    SubSliceInfo slice;
    slice.offset_bytes = 0;
    slice.length_bytes = 2 * PAGE_SIZE;
    slice.slice_index = 0;

    BufferDescriptor desc;

    // Prepare descriptors
    bool ok = be.prepare_descriptors(fake_ioaddrs, &slice, 1, &desc);
    ASSERT_TRUE(ok) << "prepare_descriptors failed";

    // Verify descriptor structure (DUAL PRP: 2 pages)
    EXPECT_EQ(desc.storage_offset, 0u);
    EXPECT_EQ(desc.data_length, 2 * PAGE_SIZE);
    EXPECT_EQ(desc.prp1, 0x1000ULL);
    EXPECT_EQ(desc.prp2, 0x2000ULL);

    // Release descriptors
    be.release_descriptors(&desc, 1);

    be.shutdown();
}

TEST_F(NvmeBackendRealHw, GpuSubmitSingleBlock) {
    // Full GPU IO round-trip: READ one block from LBA 0 via the GPU kernel.
    //
    // Flow:
    //   1. Initialize NvmeBackend → get live NvmeVirtualDevice (d_qps != null)
    //   2. cudaMalloc read buffer (1 block)
    //   3. nvm_dma_map_data_device → get DMA-capable ioaddr for the buffer
    //   4. acquire_target_handle for a 1-block raw target at LBA 0
    //   5. prepare_descriptors with the real ioaddr
    //   6. launch_batch_gpu_stream (READ)
    //   7. cudaDeviceSynchronize + verify no CUDA errors

    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open()) << "daemon not running at " << endpoint_;

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));

    auto* nvme = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    ASSERT_NE(nvme, nullptr);
    ASSERT_NE(nvme->d_qps, nullptr) << "live queues required";
    ASSERT_NE(nvme->ctrl,  nullptr) << "ctrl required for DMA mapping";
    ASSERT_GT(nvme->blk_size, 0u);

    const uint32_t blk = nvme->blk_size;

    // 2. Allocate GPU read buffer
    void* gpu_buf = nullptr;
    ASSERT_EQ(cudaMalloc(&gpu_buf, blk), cudaSuccess);

    // 3. Map GPU buffer for NVMe DMA (B3 data-mapping API)
    nvm_dma_t* dma = nullptr;
    int rc = nvm_dma_map_data_device(&dma, nvme->ctrl, gpu_buf, blk);
    if (rc != 0) {
        cudaFree(gpu_buf);
        GTEST_SKIP() << "nvm_dma_map_data_device failed (errno=" << rc
                     << "); GPU buffer may not be DMA-accessible on this kernel";
    }
    ASSERT_NE(dma, nullptr);
    ASSERT_GE(dma->n_ioaddrs, 1u);

    uint64_t ioaddr = dma->ioaddrs[0];

    // 4. Build a raw 1-block StorageTarget at LBA 0
    StorageTarget target;
    target.kind           = StorageTargetKind::NVME_RAW;
    target.target_id      = 42;
    target.namespace_id   = nvme->namespace_id;
    target.nvme_block_size     = blk;
    target.nvme_block_size_log = nvme->blk_size_log;
    target.logical_size_bytes  = blk;
    target.start_lba      = 0;
    target.length_blocks  = 1;

    VDeviceHandle hdl = be.vdevice_handle_at(0);
    void* handle = be.acquire_target_handle(target, hdl);
    ASSERT_NE(handle, nullptr) << "acquire_target_handle failed";

    // 5. Build PRP descriptor in unified memory so host can write it and
    //    GPU kernel can read it (a stack-allocated BufferDescriptor is not
    //    GPU-accessible and would fault on access).
    SubSliceInfo slice;
    slice.offset_bytes = 0;
    slice.length_bytes = blk;
    slice.slice_index  = 0;

    BufferDescriptor* d_desc = nullptr;
    ASSERT_EQ(cudaMallocManaged(&d_desc, sizeof(BufferDescriptor)), cudaSuccess);

    ASSERT_TRUE(be.prepare_descriptors(&ioaddr, &slice, 1, d_desc))
        << "prepare_descriptors failed";

    // 6. Launch GPU READ kernel and wait for completion
    cudaStream_t stream = nullptr;
    ASSERT_EQ(cudaStreamCreate(&stream), cudaSuccess);

    be.launch_batch_gpu_stream(stream, handle, d_desc, 1, /*is_read=*/true);

    ASSERT_EQ(cudaGetLastError(), cudaSuccess) << "kernel launch error";
    cudaError_t sync_err = cudaStreamSynchronize(stream);
    EXPECT_EQ(sync_err, cudaSuccess)
        << "GPU kernel did not complete cleanly: "
        << cudaGetErrorString(sync_err);

    // 7. Cleanup
    cudaStreamDestroy(stream);
    be.release_descriptors(d_desc, 1);
    cudaFree(d_desc);
    be.release_target_handle(handle);
    nvm_dma_unmap(dma);
    cudaFree(gpu_buf);

    be.shutdown();
}

// ---------------------------------------------------------------------------
// Write/Read/Verify round-trip via GPU kernel
//
// Writes a deterministic pattern to LBA 0 via GPU, reads it back via GPU,
// and verifies the data matches byte-for-byte on the host.
//
// Guarded behind TUTTI_NVME_DESTRUCTIVE=1 because it overwrites LBA 0.
// Only run against the dedicated scratch device (sys_config.b1.yaml).
// ---------------------------------------------------------------------------

namespace {
// RAII GPU/host DMA buffer.
//
// Two modes:
//   alloc_device: cudaMalloc + nvm_dma_map_data_device
//     Use for WRITE buffers — cudaMemcpy(HostToDevice) uses DMA engine that
//     bypasses GPU L2 and lands directly in GDDR, so the NVMe controller can
//     read via PCIe without coherency issues.
//
//   alloc_host: cudaMallocHost + nvm_dma_map_data_host
//     Use for READ buffers — NVMe controller writes to host-pinned DRAM via
//     PCIe, which is CPU-coherent. Avoids the GPU L2 stale-cache problem
//     that occurs when NVMe DMA writes to GDDR and the GPU cache retains
//     stale data for a subsequent cudaMemcpy(DeviceToHost).
struct DmaBuf {
    void*      dev         = nullptr;
    nvm_dma_t* dma         = nullptr;
    uint32_t   size        = 0;
    bool       host_pinned = false;

    bool alloc_device(nvm_ctrl_t* ctrl, uint32_t bytes) {
        size = bytes; host_pinned = false;
        if (cudaMalloc(&dev, bytes) != cudaSuccess) return false;
        if (nvm_dma_map_data_device(&dma, ctrl, dev, bytes) != 0) {
            cudaFree(dev); dev = nullptr; return false;
        }
        return true;
    }

    bool alloc_host(nvm_ctrl_t* ctrl, uint32_t bytes) {
        size = bytes; host_pinned = true;
        // cudaHostAllocWriteCombined: CPU reads bypass the L1/L2/L3 cache and
        // go directly to DRAM.  This ensures that NVMe PCIe DMA writes (which
        // land in DRAM without invalidating CPU cache lines) are immediately
        // visible when the CPU reads back the buffer after the kernel completes.
        if (cudaHostAlloc(&dev, bytes, cudaHostAllocWriteCombined) != cudaSuccess)
            return false;
        if (nvm_dma_map_data_host(&dma, ctrl, dev, bytes) != 0) {
            cudaFreeHost(dev); dev = nullptr; return false;
        }
        return true;
    }

    ~DmaBuf() {
        if (dma) nvm_dma_unmap(dma);
        if (dev) {
            if (host_pinned) cudaFreeHost(dev);
            else             cudaFree(dev);
        }
    }
};
} // namespace

TEST_F(NvmeBackendRealHw, GpuWriteReadVerify) {
    if (!env_flag("TUTTI_NVME_DESTRUCTIVE")) {
        GTEST_SKIP() << "TUTTI_NVME_DESTRUCTIVE != 1 (protects scratch NVMe LBA 0)";
    }

    auto mgr = make_manager(&lease_, endpoint_, /*mock_mode=*/false);
    ASSERT_TRUE(mgr->Open()) << "daemon not running at " << endpoint_;

    IPhysicalDevice* phys = mgr->find_by_type(DeviceType::LOCAL_NVME, 0);
    ASSERT_NE(phys, nullptr);

    NvmeBackend be;
    ASSERT_TRUE(be.initialize(mgr.get(), cfg(phys->id(), 1, 1)));

    auto* nvme = static_cast<NvmeVirtualDevice*>(be.vdevice_at(0));
    ASSERT_NE(nvme, nullptr);
    ASSERT_NE(nvme->d_qps, nullptr);
    ASSERT_NE(nvme->ctrl,  nullptr);
    const uint32_t blk = nvme->blk_size;
    ASSERT_GT(blk, 0u);

    // Write buffer: host-pinned WB memory (CPU fills the pattern, NVMe controller
    //   reads via regular PCIe DMA — no GPU↔NVMe P2P required).
    // Read buffer:  host-pinned WC (write-combining) memory — NVMe writes go to
    //   DRAM, WC reads bypass the CPU cache so we see the DMA-written data.
    DmaBuf wb, rb;
    if (!wb.alloc_host(nvme->ctrl, blk)) {
        GTEST_SKIP() << "nvm_dma_map_data_host (write buf) failed";
    }
    if (!rb.alloc_host(nvme->ctrl, blk)) {
        GTEST_SKIP() << "nvm_dma_map_data_host (read buf) failed";
    }

    // Deterministic write pattern: byte[i] = 0xA5 ^ (i % 251)
    // Non-trivial, avoids accidental match with zeroed or 0xFF flash content.
    std::vector<uint8_t> pattern(blk);
    for (uint32_t i = 0; i < blk; ++i)
        pattern[i] = static_cast<uint8_t>(0xA5u ^ static_cast<uint8_t>(i % 251u));

    // Fill write buffer with pattern; zero the read buffer (both host-pinned)
    memcpy(wb.dev, pattern.data(), blk);
    memset(rb.dev, 0x00, blk);

    // One target handle for LBA 0 (shared between write + read passes)
    StorageTarget target;
    target.kind                = StorageTargetKind::NVME_RAW;
    target.target_id           = 99;
    target.namespace_id        = nvme->namespace_id;
    target.nvme_block_size     = blk;
    target.nvme_block_size_log = nvme->blk_size_log;
    target.logical_size_bytes  = blk;
    target.start_lba           = 0;
    target.length_blocks       = 1;

    VDeviceHandle hdl = be.vdevice_handle_at(0);
    void* handle = be.acquire_target_handle(target, hdl);
    ASSERT_NE(handle, nullptr);

    // PRP descriptors in unified memory (host prepares, GPU kernel consumes)
    SubSliceInfo slice;
    slice.offset_bytes = 0;
    slice.length_bytes = blk;
    slice.slice_index  = 0;

    uint64_t w_ioaddr = wb.dma->ioaddrs[0];
    uint64_t r_ioaddr = rb.dma->ioaddrs[0];

    BufferDescriptor* d_wdesc = nullptr;
    BufferDescriptor* d_rdesc = nullptr;
    ASSERT_EQ(cudaMallocManaged(&d_wdesc, sizeof(BufferDescriptor)), cudaSuccess);
    ASSERT_EQ(cudaMallocManaged(&d_rdesc, sizeof(BufferDescriptor)), cudaSuccess);
    ASSERT_TRUE(be.prepare_descriptors(&w_ioaddr, &slice, 1, d_wdesc));
    ASSERT_TRUE(be.prepare_descriptors(&r_ioaddr, &slice, 1, d_rdesc));

    cudaStream_t stream = nullptr;
    ASSERT_EQ(cudaStreamCreate(&stream), cudaSuccess);

    // ── WRITE ─────────────────────────────────────────────────────────────
    be.launch_batch_gpu_stream(stream, handle, d_wdesc, 1, /*is_read=*/false);
    ASSERT_EQ(cudaGetLastError(), cudaSuccess) << "WRITE kernel launch failed";
    ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess)
        << "WRITE GPU kernel did not complete cleanly";

    // ── READ ──────────────────────────────────────────────────────────────
    be.launch_batch_gpu_stream(stream, handle, d_rdesc, 1, /*is_read=*/true);
    ASSERT_EQ(cudaGetLastError(), cudaSuccess) << "READ kernel launch failed";
    ASSERT_EQ(cudaStreamSynchronize(stream), cudaSuccess)
        << "READ GPU kernel did not complete cleanly";

    // ── VERIFY ────────────────────────────────────────────────────────────
    // rb.dev is host-pinned: NVMe DMA wrote directly to CPU-coherent DRAM.
    // Read in-place after stream sync — no cudaMemcpy needed.
    const uint8_t* readback = static_cast<const uint8_t*>(rb.dev);

    int first_mismatch = -1;
    for (uint32_t i = 0; i < blk; ++i) {
        if (readback[i] != pattern[i]) { first_mismatch = (int)i; break; }
    }
    EXPECT_EQ(first_mismatch, -1)
        << "data mismatch at byte " << first_mismatch
        << std::hex
        << ": wrote 0x" << (int)pattern[first_mismatch]
        << " read 0x"   << (int)readback[first_mismatch];

    // Cleanup (wb/rb freed by RAII dtor)
    cudaStreamDestroy(stream);
    be.release_descriptors(d_wdesc, 1);  cudaFree(d_wdesc);
    be.release_descriptors(d_rdesc, 1);  cudaFree(d_rdesc);
    be.release_target_handle(handle);

    be.shutdown();
}
