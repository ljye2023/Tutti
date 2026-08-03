// tutti/data_paths/striped_local_nvme/striped_data_path.cpp
//
// StripedDataPath implementation — single-kernel fused submission across
// N NVMe devices.

#include "tutti/data_paths/striped_local_nvme/striped_data_path.h"

#include "tutti/data_paths/local_nvme/io/device_target.h"
#include "tutti/data_paths/local_nvme/io/nvme_submit_primitives.cuh"
#include "tutti/data_paths/local_nvme/io/prp_builder.h"
#include "tutti/data_paths/local_nvme/io/nvme_queue_group.h"
#include "tutti/data_paths/striped_local_nvme/fused_submit_kernel.cuh"
#include "tutti/bindings/ext4_local_nvme/binding.h"

#include <nvm_ctrl.h>
#include <nvm_dma.h>

#include <cuda_runtime.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace tutti::data_paths::striped_local_nvme {

using namespace tutti::binding::ext4_local_nvme;
using namespace tutti::binding::striped_local_nvme;
using tutti::data_paths::local_nvme::DeviceTargetHandle;
using tutti::data_paths::local_nvme::DeviceLbaExtent;
using tutti::data_paths::local_nvme::EntryCompletionStatus;
using tutti::data_paths::local_nvme::NvmeQueueGroup;
using tutti::data_paths::local_nvme::PrpKind;
using tutti::data_paths::local_nvme::classify_prp;
using tutti::data_paths::local_nvme::fill_prp_list_page;
using tutti::data_paths::local_nvme::kDeviceTargetInlineExtents;
using tutti::data_paths::local_nvme::build_device_target;
using tutti::data_paths::local_nvme::free_device_target;

// =========================================================================
// Constructor / Destructor
// =========================================================================

StripedDataPath::StripedDataPath(std::vector<DeviceDescriptor> devices,
                                 std::uint32_t cuda_device,
                                 std::uint64_t mdts_override,
                                 std::uint32_t cq_poll_budget,
                                 std::uint32_t max_batch_entries,
                                 std::uint32_t max_in_flight_operations)
    : device_descs_(std::move(devices)),
      cuda_device_(cuda_device),
      mdts_override_(mdts_override),
      cq_poll_budget_(cq_poll_budget == 0 ? 2000000 : cq_poll_budget),
      max_batch_entries_(max_batch_entries == 0 ? 256 : max_batch_entries),
      max_in_flight_operations_(max_in_flight_operations == 0
                                 ? 16 : max_in_flight_operations) {

    caps_.name = "striped-local-nvme";
    caps_.source_api_version = 1;
    caps_.supports_host_execution = false;
    caps_.supports_device_execution = true;
    caps_.supports_host_memory = false;
    caps_.supports_device_memory = true;
    caps_.supports_direct = true;
    caps_.supports_staged = false;
    caps_.supports_read = true;
    caps_.supports_write = true;
    caps_.target_alignment_bytes = 4096;
    caps_.memory_alignment_bytes = 65536;  // 64 KiB (snvme pinning)
    caps_.length_alignment_bytes = 4096;
    // Preliminary caps; initialize() updates with real hardware values.
    caps_.max_single_io_bytes = static_cast<std::uint64_t>(max_batch_entries_) * 131072;
    caps_.max_batch_requests = max_batch_entries_;
    caps_.max_batch_bytes = caps_.max_single_io_bytes;
    caps_.max_in_flight_operations = max_in_flight_operations_;
    caps_.supports_scatter_gather = false;
    caps_.max_scatter_gather_entries = 0;
    caps_.registration_scope = RegistrationScope::PER_TARGET;
    caps_.progress_model = ProgressModel::HOST_POLL;
    caps_.device_completion_fence_on_caller_stream = true;
    caps_.device_execution_autonomous = true;
    caps_.supports_multi_stream = true;
    caps_.max_concurrent_streams = 2;
    caps_.max_concurrent_operations = max_in_flight_operations_;
    caps_.supports_multi_gpu = false;
    caps_.supports_cross_device = false;
    caps_.optional_target_features = {};
}

StripedDataPath::~StripedDataPath() {
    if (initialized_) {
        shutdown(0);
    }
}

// =========================================================================
// capabilities
// =========================================================================

const DataPathCapabilities& StripedDataPath::capabilities() const {
    return caps_;
}

// =========================================================================
// initialize — attach N controllers, create N queue groups, arena init
// =========================================================================

Status StripedDataPath::initialize(const DataPathConfig& config,
                                   ResourceProvider& /*resources*/) {
    if (initialized_) {
        return Status(StatusCode::BUSY, "already initialized");
    }
    if (device_descs_.empty()) {
        return Status(StatusCode::INVALID_ARGUMENT, "no devices configured");
    }

    if (!config.name.empty()) {
        caps_.name = config.name;
    }

    // All shard block sizes must be uniform (documented assumption).
    block_size_ = device_descs_[0].block_size;
    for (const auto& d : device_descs_) {
        if (d.block_size != block_size_) {
            return Status(StatusCode::INVALID_ARGUMENT,
                         "all devices must share the same block_size");
        }
        if (d.block_size == 0 || d.bar0_size == 0 || d.queue_depth == 0) {
            return Status(StatusCode::INVALID_ARGUMENT,
                         "device descriptor missing required fields");
        }
    }

    devices_.reserve(device_descs_.size());

    auto rollback_devices = [&]() {
        for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
            it->queue_group.reset();
            if (it->ctrl) { nvm_ctrl_free_client(it->ctrl); it->ctrl = nullptr; }
        }
        devices_.clear();
    };

    for (std::uint32_t i = 0; i < device_descs_.size(); ++i) {
        const auto& desc = device_descs_[i];
        DeviceSlot slot;
        slot.desc = desc;

        int rc = nvm_ctrl_attach_client(&slot.ctrl, desc.snvme_dev_path.c_str(),
                                        desc.bar0_size);
        if (rc != 0 || slot.ctrl == nullptr) {
            rollback_devices();
            return Status(StatusCode::NOT_READY,
                         "nvm_ctrl_attach_client(" + desc.snvme_dev_path +
                         ") failed: rc " + std::to_string(rc));
        }

        struct disk dev_info;
        std::memset(&dev_info, 0, sizeof(dev_info));
        rc = ioctl_get_dev_info(slot.ctrl, &dev_info);
        if (rc != 0) {
            nvm_ctrl_free_client(slot.ctrl);
            slot.ctrl = nullptr;
            rollback_devices();
            return Status(StatusCode::NOT_READY,
                         "ioctl_get_dev_info failed for device " +
                         std::to_string(i) + ": rc " + std::to_string(rc));
        }

        slot.hardware_mdts = dev_info.max_data_size;
        slot.page_size = static_cast<std::uint64_t>(slot.ctrl->page_size);

        struct disk disk_info = dev_info;
        disk_info.ns_id = desc.namespace_id;
        disk_info.block_size = desc.block_size;
        std::string dname = desc.snvme_dev_path;
        if (dname.rfind("/dev/", 0) == 0) dname = dname.substr(5);
        dname += "n" + std::to_string(desc.namespace_id);
        std::strncpy(disk_info.disk_name, dname.c_str(),
                    sizeof(disk_info.disk_name) - 1);

        try {
            slot.queue_group = std::make_unique<NvmeQueueGroup>(
                slot.ctrl, disk_info, desc.namespace_id, desc.cuda_device,
                desc.num_user_queues, desc.queue_depth);
        } catch (const std::runtime_error& e) {
            nvm_ctrl_free_client(slot.ctrl);
            slot.ctrl = nullptr;
            rollback_devices();
            return Status(StatusCode::NOT_READY,
                         "queue group creation failed for device " +
                         std::to_string(i) + ": " + e.what());
        }

        devices_.push_back(std::move(slot));
    }

    // effective_mdts = min(all devices' hardware MDTS), further clamped by
    // an optional override.
    std::uint64_t min_mdts = UINT64_MAX;
    for (const auto& d : devices_) {
        if (d.hardware_mdts > 0 && d.hardware_mdts < min_mdts) {
            min_mdts = d.hardware_mdts;
        }
    }
    if (min_mdts == UINT64_MAX || min_mdts == 0) {
        rollback_devices();
        return Status(StatusCode::NOT_READY, "no device reported a usable MDTS");
    }
    effective_mdts_bytes_ = (mdts_override_ > 0)
        ? std::min(mdts_override_, min_mdts)
        : min_mdts;
    if (effective_mdts_bytes_ % block_size_ != 0) {
        rollback_devices();
        return Status(StatusCode::INVALID_ARGUMENT,
                      "effective MDTS not a block-size multiple");
    }

    // PRP-list page capacity check (same formula as LocalNvmeDataPath).
    const std::uint64_t page_size = devices_[0].page_size;
    for (const auto& d : devices_) {
        if (d.page_size != page_size) {
            rollback_devices();
            return Status(StatusCode::INVALID_ARGUMENT,
                         "all devices must share the same controller page_size");
        }
    }
    std::uint64_t prp_list_page_capacity = page_size / sizeof(std::uint64_t) + 1;
    std::uint64_t mdts_pages = effective_mdts_bytes_ / page_size;
    if (mdts_pages > prp_list_page_capacity) {
        rollback_devices();
        return Status(StatusCode::INVALID_ARGUMENT,
                      "effective MDTS exceeds PRP-list page capacity");
    }

    max_request_bytes_ = static_cast<std::uint64_t>(max_batch_entries_) *
                         effective_mdts_bytes_;
    caps_.max_single_io_bytes = max_request_bytes_;
    caps_.max_batch_bytes = max_request_bytes_;
    caps_.max_batch_requests = max_batch_entries_;
    caps_.max_in_flight_operations = max_in_flight_operations_;
    caps_.max_concurrent_operations = max_in_flight_operations_;
    caps_.target_alignment_bytes = block_size_;
    caps_.memory_alignment_bytes = block_size_;
    caps_.length_alignment_bytes = block_size_;

    // Arena init: dev_table_capacity_per_slot = N (one submit's device
    // table spans exactly the striped target's N shards).
    std::vector<nvm_ctrl_t*> ctrls;
    ctrls.reserve(devices_.size());
    for (auto& d : devices_) ctrls.push_back(d.ctrl);

    StripedArena::Config arena_cfg;
    arena_cfg.num_slots = max_in_flight_operations_ * 2;
    arena_cfg.max_entries_per_slot = max_batch_entries_;
    arena_cfg.page_size = static_cast<std::uint32_t>(page_size);
    arena_cfg.cuda_device = cuda_device_;
    arena_cfg.dev_table_capacity_per_slot =
        static_cast<std::uint32_t>(devices_.size());
    if (!arena_.init(arena_cfg, ctrls)) {
        rollback_devices();
        return Status(StatusCode::NOT_READY, "StripedArena init failed");
    }

    initialized_ = true;
    return Status::Ok();
}

// =========================================================================
// shutdown — release arena, all N devices
// =========================================================================

Status StripedDataPath::shutdown(std::uint64_t timeout_ns) {
    if (!initialized_) return Status::Ok();

    auto has_inflight = [&]() -> bool {
        for (const auto& [tok, op] : ops_) {
            if (op.state == OpState::IN_FLIGHT) return true;
        }
        return false;
    };

    if (has_inflight()) {
        if (timeout_ns == 0) {
            return Status(StatusCode::TIMEOUT,
                          "shutdown: in-flight operations remain");
        }
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(timeout_ns);
        while (std::chrono::steady_clock::now() < deadline) {
            auto remaining_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining_ns <= 0) break;
            ProgressBudget pb{max_in_flight_operations_,
                              static_cast<std::uint64_t>(remaining_ns)};
            auto pr = progress(pb);
            if (!pr.ok()) break;
            if (!has_inflight()) break;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        if (has_inflight()) {
            return Status(StatusCode::TIMEOUT,
                          "shutdown: drain timeout, in-flight ops remain");
        }
    }

    bool any_timeout = false;
    for (const auto& [tok, op] : ops_) {
        if (op.has_timeout) { any_timeout = true; break; }
    }
    arena_.shutdown(any_timeout);
    ops_.clear();

    for (auto& [tok, tgt] : targets_) {
        for (std::uint32_t s = 0; s < tgt.num_shards; ++s) {
            if (tgt.dev_handles[s]) {
                free_device_target(tgt.dev_handles[s], tgt.overflow_allocs[s],
                                   cuda_device_);
            }
        }
    }
    targets_.clear();

    for (auto& [tok, mem] : memory_regs_) {
        for (auto* dma : mem.dmas) {
            if (dma) nvm_dma_unmap(dma);
        }
    }
    memory_regs_.clear();

    for (auto it = devices_.rbegin(); it != devices_.rend(); ++it) {
        it->queue_group.reset();
        if (it->ctrl) {
            nvm_ctrl_free_client(it->ctrl);
            it->ctrl = nullptr;
        }
    }
    devices_.clear();

    initialized_ = false;
    return Status::Ok();
}

// =========================================================================
// open — extract StripedLocalNvmePayload, build N DeviceTargetHandles
// =========================================================================

Result<DataPathTarget> StripedDataPath::open(const ResolvedTarget& target) {
    if (!initialized_) {
        return Result<DataPathTarget>::Failure(
            Status(StatusCode::NOT_READY, "not initialized"));
    }

    auto payload_result = tutti::binding::striped_local_nvme::view_payload(target);
    if (!payload_result.ok()) {
        return Result<DataPathTarget>::Failure(
            Status(payload_result.status().code(),
                   "open: payload view failed: " +
                   payload_result.status().message()));
    }
    const StripedLocalNvmePayload* p = payload_result.value();

    if (p->num_shards() != devices_.size()) {
        return Result<DataPathTarget>::Failure(
            Status(StatusCode::INVALID_ARGUMENT,
                   "payload num_shards=" + std::to_string(p->num_shards()) +
                   " != devices=" + std::to_string(devices_.size())));
    }

    StripedTarget tgt;
    tgt.num_shards = p->num_shards();
    tgt.stripe_unit = p->stripe_unit();
    tgt.logical_size = p->logical_size();
    tgt.dev_handles.assign(tgt.num_shards, nullptr);
    tgt.overflow_allocs.assign(tgt.num_shards, nullptr);
    tgt.shard_extents.resize(tgt.num_shards);

    for (std::uint32_t s = 0; s < tgt.num_shards; ++s) {
        if (!build_shard_handle_(s, p->shards()[s], tgt)) {
            for (std::uint32_t j = 0; j < s; ++j) {
                if (tgt.dev_handles[j]) {
                    free_device_target(tgt.dev_handles[j], tgt.overflow_allocs[j],
                                       cuda_device_);
                }
            }
            return Result<DataPathTarget>::Failure(
                Status(StatusCode::DEVICE_ERROR,
                       "open: failed to build handle for shard " +
                       std::to_string(s)));
        }
    }

    std::uint64_t tok = next_target_++;
    tgt.generation = 1;
    tgt.domain_key = "striped-local-nvme:" + std::to_string(tok);
    targets_[tok] = std::move(tgt);

    return Result<DataPathTarget>::Success(
        detail::SpiIdentityMint::mint<detail::DataPathTargetTag>(tok, 1));
}

bool StripedDataPath::build_shard_handle_(
    std::uint32_t dev_idx,
    const ResolvedTarget& shard_target,
    StripedTarget& out) {

    auto ep = tutti::binding::ext4_local_nvme::view_payload(shard_target);
    if (!ep.ok()) return false;
    const Ext4LocalNvmePayload* ext = ep.value();

    const auto& ns = ext->namespace_identity();
    const auto& src_extents = ext->extents();
    if (ns.block_size == 0) return false;

    DeviceSlot& slot = devices_[dev_idx];
    if (!slot.queue_group || slot.queue_group->d_qps() == nullptr) return false;

    std::uint32_t bs = ns.block_size;
    std::uint32_t bs_log = 0;
    while ((1u << bs_log) < bs) ++bs_log;
    if ((1u << bs_log) != bs) return false;

    DeviceTargetHandle tmpl;
    std::memset(&tmpl, 0, sizeof(tmpl));
    tmpl.file_id = dev_idx;
    tmpl.logical_size_bytes = shard_target.logical_size();
    tmpl.header_bytes = 0;
    tmpl.nvme_block_size = bs;
    tmpl.nvme_block_size_log = bs_log;
    tmpl.namespace_id = ns.namespace_id;
    tmpl.num_extents = static_cast<std::uint32_t>(src_extents.size());
    tmpl.d_qps = slot.queue_group->d_qps();
    tmpl.num_d_qps = slot.queue_group->n_qps();
    tmpl.extents_overflow = nullptr;

    auto convert_extent = [&](const Extent& e) -> DeviceLbaExtent {
        DeviceLbaExtent d;
        d.start_lba = e.device_offset / bs;
        d.length_blocks = e.length / bs;
        return d;
    };

    std::uint32_t n_inline = std::min(tmpl.num_extents, kDeviceTargetInlineExtents);
    for (std::uint32_t i = 0; i < n_inline; ++i) {
        tmpl.extents[i] = convert_extent(src_extents[i]);
    }

    std::vector<DeviceLbaExtent> overflow_buf;
    const DeviceLbaExtent* overflow_ptr = nullptr;
    std::uint32_t n_overflow = 0;
    if (tmpl.num_extents > kDeviceTargetInlineExtents) {
        n_overflow = tmpl.num_extents - kDeviceTargetInlineExtents;
        overflow_buf.resize(n_overflow);
        for (std::uint32_t i = 0; i < n_overflow; ++i) {
            overflow_buf[i] = convert_extent(src_extents[kDeviceTargetInlineExtents + i]);
        }
        overflow_ptr = overflow_buf.data();
    }

    DeviceTargetHandle* dev_h = nullptr;
    void* dev_ov = nullptr;
    if (!build_device_target(tmpl, overflow_ptr, n_overflow,
                             cuda_device_, &dev_h, &dev_ov)) {
        return false;
    }

    out.dev_handles[dev_idx] = dev_h;
    out.overflow_allocs[dev_idx] = dev_ov;

    // Host-side byte extents for stripe-split boundary clamping.
    out.shard_extents[dev_idx].reserve(src_extents.size());
    for (const auto& e : src_extents) {
        out.shard_extents[dev_idx].push_back({e.logical_offset, e.length});
    }

    return true;
}

// =========================================================================
// close / registration_domain
// =========================================================================

Status StripedDataPath::close(DataPathTarget target) {
    if (!target.valid()) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "close: target identity is invalid");
    }
    auto it = targets_.find(target.token());
    if (it == targets_.end()) {
        return Status(StatusCode::NOT_FOUND, "close: target not found");
    }
    if (it->second.generation != target.generation()) {
        return Status(StatusCode::NOT_FOUND, "close: generation mismatch");
    }
    if (target_has_inflight_ops_(target.token())) {
        return Status(StatusCode::BUSY, "close: target has in-flight operations");
    }

    for (std::uint32_t s = 0; s < it->second.num_shards; ++s) {
        if (it->second.dev_handles[s]) {
            free_device_target(it->second.dev_handles[s],
                               it->second.overflow_allocs[s], cuda_device_);
        }
    }
    targets_.erase(it);
    return Status::Ok();
}

Result<RegistrationDomainKey> StripedDataPath::registration_domain(
    DataPathTarget target) const {
    const auto* tgt = find_target_(target);
    if (!tgt) {
        return Result<RegistrationDomainKey>::Failure(
            Status(StatusCode::NOT_FOUND, "registration_domain: target not found"));
    }
    return Result<RegistrationDomainKey>::Success(
        RegistrationDomainKey{tgt->domain_key});
}

// =========================================================================
// register_memory / unregister_memory — nvm_dma_map_data_device x N
// =========================================================================

Result<DataPathMemory> StripedDataPath::register_memory(
    const DataPathMemoryView& view,
    const RegistrationDomainKey& /*domain*/) {

    if (!initialized_) {
        return Result<DataPathMemory>::Failure(
            Status(StatusCode::NOT_READY, "register_memory: not initialized"));
    }
    if (view.base == nullptr || view.size_bytes == 0) {
        return Result<DataPathMemory>::Failure(
            Status(StatusCode::INVALID_ARGUMENT, "null/zero memory view"));
    }
    if (view.kind != DataPathMemoryKind::DEVICE) {
        return Result<DataPathMemory>::Failure(
            Status(StatusCode::UNSUPPORTED, "only DEVICE memory supported"));
    }
    if ((reinterpret_cast<std::uintptr_t>(view.base) % 65536) != 0) {
        return Result<DataPathMemory>::Failure(
            Status(StatusCode::INVALID_ARGUMENT,
                   "DEVICE view.base must be 64 KiB-aligned"));
    }

    StripedMemory mem;
    mem.base = view.base;
    mem.size = view.size_bytes;
    mem.dmas.assign(devices_.size(), nullptr);

    for (std::size_t i = 0; i < devices_.size(); ++i) {
        nvm_dma_t* dma = nullptr;
        int rc = nvm_dma_map_data_device(&dma, devices_[i].ctrl,
                                         view.base,
                                         static_cast<size_t>(view.size_bytes));
        if (rc != 0 || dma == nullptr) {
            for (std::size_t j = 0; j < i; ++j) {
                if (mem.dmas[j]) { nvm_dma_unmap(mem.dmas[j]); mem.dmas[j] = nullptr; }
            }
            return Result<DataPathMemory>::Failure(
                Status(StatusCode::DEVICE_ERROR,
                       "nvm_dma_map_data_device failed for device " +
                       std::to_string(i) + ": rc " + std::to_string(rc)));
        }
        mem.dmas[i] = dma;
    }

    std::uint64_t tok = next_memory_++;
    mem.generation = 1;
    memory_regs_[tok] = std::move(mem);

    return Result<DataPathMemory>::Success(
        detail::SpiIdentityMint::mint<detail::DataPathMemoryTag>(tok, 1));
}

Status StripedDataPath::unregister_memory(DataPathMemory memory) {
    if (!memory.valid()) {
        return Status(StatusCode::INVALID_ARGUMENT,
                      "unregister_memory: memory identity is invalid");
    }
    auto it = memory_regs_.find(memory.token());
    if (it == memory_regs_.end()) {
        return Status(StatusCode::NOT_FOUND, "unregister_memory: not found");
    }
    if (it->second.generation != memory.generation()) {
        return Status(StatusCode::NOT_FOUND,
                      "unregister_memory: generation mismatch");
    }
    if (memory_has_inflight_ops_(memory.token())) {
        return Status(StatusCode::BUSY,
                      "unregister_memory: memory has in-flight operations");
    }
    for (auto* dma : it->second.dmas) {
        if (dma) nvm_dma_unmap(dma);
    }
    memory_regs_.erase(it);
    return Status::Ok();
}

// =========================================================================
// submit — stripe split -> 1 H2D (entries + dev_table) -> 1 launch -> 1 event
// =========================================================================

SubmitOutcome StripedDataPath::submit(const DataPathRequest* requests,
                                      std::size_t count,
                                      const HostSubmitContext& ctx) {
    ++test_submit_call_count_;

    SubmitOutcome outcome;
    outcome.op = std::nullopt;
    outcome.initial_states.resize(count);

    if (count == 0) {
        outcome.status = Status::Ok();
        return outcome;
    }

    auto reject_all = [&](StatusCode code, const std::string& msg) {
        outcome.status = Status(code, msg);
        for (std::size_t i = 0; i < count; ++i) {
            outcome.initial_states[i].state = RequestState::REJECTED;
            outcome.initial_states[i].status = Status(code, msg);
        }
    };
    StatusCode first_rejected_code = StatusCode::OK;
    std::string first_rejected_msg;
    auto reject_one = [&](std::size_t i, StatusCode code, const std::string& msg) {
        outcome.initial_states[i].state = RequestState::REJECTED;
        outcome.initial_states[i].status = Status(code, msg);
        if (first_rejected_code == StatusCode::OK) {
            first_rejected_code = code;
            first_rejected_msg = msg;
        }
    };

    if (!initialized_) {
        reject_all(StatusCode::NOT_READY, "DataPath not initialized");
        return outcome;
    }
    if (requests == nullptr) {
        reject_all(StatusCode::INVALID_ARGUMENT, "null requests");
        return outcome;
    }
    if (ctx.execution_domain != ExecutionDomain::DEVICE_EXECUTION) {
        reject_all(StatusCode::UNSUPPORTED, "HOST_EXECUTION not supported");
        return outcome;
    }
    if (ctx.stream == nullptr) {
        reject_all(StatusCode::INVALID_ARGUMENT, "null stream");
        return outcome;
    }
    if (ctx.device_id != static_cast<std::int32_t>(cuda_device_)) {
        reject_all(StatusCode::INVALID_ARGUMENT,
                   "ctx.device_id does not match this DataPath's CUDA device");
        return outcome;
    }

    std::uint64_t in_flight_count = 0;
    for (const auto& [tok, op] : ops_) {
        if (op.state == OpState::IN_FLIGHT) ++in_flight_count;
    }
    if (in_flight_count >= max_in_flight_operations_) {
        reject_all(StatusCode::RESOURCE_EXHAUSTED,
                   "in-flight operation capacity exhausted");
        return outcome;
    }

    // Device-table capacity constraint: StripedArena sizes the per-op device
    // table at exactly N (one striped target's shard count) -- see
    // striped_data_path.h's class comment. A single striped target's shard
    // set already uses the full table, so a batch referencing more than one
    // target is rejected per-request (partial commit, not silent mis-route),
    // not merged into a larger table. This is a capacity constraint, not a
    // single-target assumption bug: the SPI's "requests MAY span multiple
    // targets" contract is honored by explicit RESOURCE_EXHAUSTED rejection
    // of the requests that would exceed the table, exactly as an
    // over-large batch is rejected elsewhere in this function.
    const StripedTarget* tgt = nullptr;
    std::uint64_t tgt_token = 0;
    std::vector<bool> rejected(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* t = find_target_(requests[i].target);
        if (!t) {
            reject_one(i, StatusCode::NOT_FOUND, "target not found or closed");
            rejected[i] = true;
            continue;
        }
        if (tgt == nullptr) {
            tgt = t;
            tgt_token = requests[i].target.token();
        } else if (requests[i].target.token() != tgt_token) {
            reject_one(i, StatusCode::RESOURCE_EXHAUSTED,
                      "batch references a second striped target; device-table "
                      "capacity (N shards) is exhausted by the first target");
            rejected[i] = true;
        }
    }
    if (tgt == nullptr) {
        outcome.status = Status(StatusCode::NOT_FOUND, "no valid target in batch");
        return outcome;
    }

    const std::uint32_t page_size =
        static_cast<std::uint32_t>(devices_[0].page_size);

    struct ListInfo {
        std::uint32_t entry_idx;
        std::uint32_t start_page;
        std::uint32_t pages_in_io;
        std::uint32_t dev_idx;
        nvm_dma_t* dma;  // the exact DMA table used to compute this entry's prp1
    };

    std::vector<StripedDeviceSubmitEntry> h_entries;
    h_entries.reserve(count * 2);
    std::vector<ListInfo> list_infos;
    std::uint64_t total_bytes = 0;
    bool has_rejection = false;
    std::vector<const StripedMemory*> req_mregs(count, nullptr);

    for (std::size_t i = 0; i < count; ++i) {
        if (rejected[i]) {
            has_rejection = true;
            continue;
        }
        const auto& req = requests[i];
        const auto& intent = req.intent;

        if (intent.length > max_request_bytes_) {
            reject_one(i, StatusCode::OUT_OF_RANGE, "request exceeds max_single_io_bytes");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }

        const auto* mreg = find_memory_(req.memory);
        if (!mreg) {
            reject_one(i, StatusCode::NOT_FOUND, "memory not found");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }

        if (intent.target_offset % block_size_ != 0 ||
            intent.length % block_size_ != 0 ||
            intent.memory_offset % block_size_ != 0) {
            reject_one(i, StatusCode::INVALID_ARGUMENT, "not block-aligned");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }
        if (intent.length == 0) {
            reject_one(i, StatusCode::INVALID_ARGUMENT, "zero length");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }
        if (intent.target_offset > tgt->logical_size ||
            intent.length > tgt->logical_size - intent.target_offset) {
            reject_one(i, StatusCode::OUT_OF_RANGE, "target bounds exceeded");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }
        if (intent.memory_offset > mreg->size ||
            intent.length > mreg->size - intent.memory_offset) {
            reject_one(i, StatusCode::OUT_OF_RANGE, "memory bounds exceeded");
            rejected[i] = true;
            has_rejection = true;
            continue;
        }

        req_mregs[i] = mreg;
        std::uint32_t direction = (intent.direction == IoDirection::READ) ? 0 : 1;

        // Stripe-split + MDTS fan-out.
        std::uint64_t remaining = intent.length;
        std::uint64_t cur_off = intent.target_offset;
        std::uint64_t cur_mem = intent.memory_offset;
        bool req_ok = true;

        while (remaining > 0) {
            std::uint32_t shard = static_cast<std::uint32_t>(
                (cur_off / tgt->stripe_unit) % tgt->num_shards);
            std::uint64_t shard_off =
                (cur_off / (tgt->stripe_unit * tgt->num_shards)) * tgt->stripe_unit +
                (cur_off % tgt->stripe_unit);

            std::uint64_t unit_remaining = tgt->stripe_unit - (cur_off % tgt->stripe_unit);
            std::uint64_t sub_io = std::min(remaining, unit_remaining);
            sub_io = std::min(sub_io, effective_mdts_bytes_);

            // Clamp to the shard's own extent boundary (mirrors
            // LocalNvmeDataPath's extent-boundary clamp).
            std::uint64_t ext_end = 0;
            for (const auto& ext : tgt->shard_extents[shard]) {
                std::uint64_t ext_start = ext.logical_offset_bytes;
                std::uint64_t ext_e = ext_start + ext.length_bytes;
                if (shard_off >= ext_start && shard_off < ext_e) {
                    ext_end = ext_e;
                    break;
                }
            }
            if (ext_end > 0) {
                sub_io = std::min(sub_io, ext_end - shard_off);
            }

            nvm_dma_t* dma = req_mregs[i]->dmas[shard];
            std::uint32_t start_page = static_cast<std::uint32_t>(cur_mem / page_size);
            std::uint32_t pages_in_io = static_cast<std::uint32_t>(
                (sub_io + page_size - 1) / page_size);
            PrpKind kind = classify_prp(pages_in_io);

            if (start_page + pages_in_io > dma->n_ioaddrs) {
                reject_one(i, StatusCode::OUT_OF_RANGE, "PRP page out of DMA range");
                rejected[i] = true;
                req_ok = false;
                break;
            }

            StripedDeviceSubmitEntry entry{};
            entry.dev_idx = shard;
            entry.direction = direction;
            entry.shard_offset = shard_off;
            entry.length = sub_io;
            entry.prp1 = dma->ioaddrs[start_page];

            if (kind == PrpKind::SINGLE) {
                entry.prp2 = 0;
            } else if (kind == PrpKind::DUAL) {
                entry.prp2 = dma->ioaddrs[start_page + 1];
            } else {  // LIST
                entry.prp2 = 0;  // filled after PRP-list alloc
                list_infos.push_back({
                    static_cast<std::uint32_t>(h_entries.size()),
                    start_page, pages_in_io, shard, dma});
            }

            h_entries.push_back(entry);
            total_bytes += sub_io;
            cur_off += sub_io;
            cur_mem += sub_io;
            remaining -= sub_io;
        }
        if (!req_ok) { has_rejection = true; continue; }

        outcome.initial_states[i].state = RequestState::ACCEPTED;
        outcome.initial_states[i].status = Status::Ok();
    }

    if (h_entries.empty()) {
        outcome.status = Status(first_rejected_code != StatusCode::OK
                                ? first_rejected_code : StatusCode::INVALID_ARGUMENT,
                                "all requests rejected");
        return outcome;
    }

    const std::uint32_t total_entries = static_cast<std::uint32_t>(h_entries.size());
    if (total_entries > max_batch_entries_) {
        reject_all(StatusCode::RESOURCE_EXHAUSTED, "too many sub-IOs (entries)");
        return outcome;
    }
    if (total_bytes > caps_.max_batch_bytes) {
        reject_all(StatusCode::RESOURCE_EXHAUSTED, "batch bytes exceed limit");
        return outcome;
    }

    // ---- Irreversible resource reservation ----
    StripedArena::Lease lease;
    if (!arena_.acquire(lease)) {
        reject_all(StatusCode::RESOURCE_EXHAUSTED,
                   "StripedArena exhausted (all slots in use or leaked)");
        return outcome;
    }
    if (tgt->num_shards > lease.dev_table_capacity) {
        arena_.release(lease.slot_index);
        reject_all(StatusCode::RESOURCE_EXHAUSTED,
                   "target shard count exceeds arena device-table capacity");
        return outcome;
    }

    cudaEvent_t event = static_cast<cudaEvent_t>(lease.event);
    StripedDeviceSubmitEntry* d_entries = lease.d_entries;
    EntryCompletionStatus* d_status = lease.d_status;
    void* prp_pages = lease.prp_pages_devptr;
    std::uint32_t prp_ioaddrs_base = lease.prp_ioaddrs_base;
    cudaError_t ce;

    // Fill PRP-list pages (arena pool, H2D on caller stream). Each list
    // entry's PRP-list page content depends on the exact nvm_dma_t* used
    // to compute that entry's prp1 (captured per-entry in ListInfo.dma);
    // the arena's PRP-list page IOVA for THIS shard's controller comes from
    // arena_.prp_dma(dev_idx) (the arena maps its PRP pool once per device).
    if (!list_infos.empty()) {
        std::vector<std::uint64_t> h_page(page_size / sizeof(std::uint64_t), 0);
        std::uint32_t list_idx = 0;
        for (const auto& li : list_infos) {
            fill_prp_list_page(h_page.data(), li.dma,
                               li.start_page, li.pages_in_io, page_size);
            ce = cudaMemcpyAsync(
                static_cast<char*>(prp_pages) + list_idx * page_size,
                h_page.data(), page_size, cudaMemcpyHostToDevice, ctx.stream);
            if (ce != cudaSuccess) {
                arena_.release(lease.slot_index);
                reject_all(StatusCode::DEVICE_ERROR, "H2D PRP page failed");
                return outcome;
            }
            h_entries[li.entry_idx].prp2 =
                arena_.prp_dma(li.dev_idx)->ioaddrs[prp_ioaddrs_base + list_idx];
            ++list_idx;
        }
    }

    ce = cudaMemcpyAsync(d_entries, h_entries.data(),
                         total_entries * sizeof(StripedDeviceSubmitEntry),
                         cudaMemcpyHostToDevice, ctx.stream);
    if (ce != cudaSuccess) {
        arena_.release(lease.slot_index);
        reject_all(StatusCode::DEVICE_ERROR, "H2D entries failed");
        return outcome;
    }

    ce = cudaMemcpyAsync(const_cast<void*>(static_cast<const void*>(lease.d_dev_table)),
                        tgt->dev_handles.data(),
                        tgt->num_shards * sizeof(DeviceTargetHandle*),
                        cudaMemcpyHostToDevice, ctx.stream);
    if (ce != cudaSuccess) {
        arena_.release(lease.slot_index);
        reject_all(StatusCode::DEVICE_ERROR, "H2D dev_table failed");
        return outcome;
    }

    ce = cudaMemsetAsync(d_status, 0, total_entries * sizeof(EntryCompletionStatus),
                        ctx.stream);
    if (ce != cudaSuccess) {
        arena_.release(lease.slot_index);
        reject_all(StatusCode::DEVICE_ERROR, "cudaMemset d_status failed");
        return outcome;
    }

    cudaError_t launch_err = launch_fused_submit(
        d_entries, d_status,
        reinterpret_cast<const DeviceTargetHandle* const*>(lease.d_dev_table),
        total_entries, tgt->num_shards, cq_poll_budget_, 0, ctx.stream);
    if (launch_err != cudaSuccess) {
        arena_.release(lease.slot_index);
        reject_all(StatusCode::DEVICE_ERROR,
                   std::string("fused kernel launch failed: ") +
                   cudaGetErrorString(launch_err));
        return outcome;
    }
    ++test_kernel_launch_count_;

    ce = cudaEventRecord(event, ctx.stream);
    std::uint64_t op_token = next_op_token_++;
    OpEntry op;
    op.total_bytes = total_bytes;
    op.d_entries = d_entries;
    op.d_status = d_status;
    op.entry_count = total_entries;
    op.event = event;
    op.stream = ctx.stream;
    op.arena_slot = lease.slot_index;
    op.prp_ioaddrs_base = prp_ioaddrs_base;
    op.prp_pages_devptr = prp_pages;
    op.prp_list_page_count = static_cast<std::uint32_t>(list_infos.size());
    op.op_token = op_token;
    op.op_generation = 1;
    op.target_token = tgt_token;

    if (ce != cudaSuccess) {
        // Same conservative fallback as LocalNvmeDataPath: sync the stream.
        cudaError_t sync_err = cudaStreamSynchronize(ctx.stream);
        if (sync_err != cudaSuccess) cudaGetLastError();
        op.state = (sync_err == cudaSuccess) ? OpState::COMPLETED : OpState::FAILED;
        op.status = (sync_err == cudaSuccess)
            ? Status::Ok()
            : Status(StatusCode::DEVICE_ERROR, "stream sync failed after event record failure");
        if (sync_err == cudaSuccess) {
            aggregate_completion_status_(op);
        } else {
            op.bytes_transferred = 0;
        }
    } else {
        op.state = OpState::IN_FLIGHT;
        op.status = Status::Ok();
    }
    ops_[op_token] = std::move(op);

    if (has_rejection) {
        outcome.status = Status(first_rejected_code != StatusCode::OK
                                ? first_rejected_code : StatusCode::INVALID_ARGUMENT,
                                "partial commit: " +
                                (first_rejected_msg.empty()
                                 ? std::string("some requests rejected")
                                 : first_rejected_msg));
    } else {
        outcome.status = Status::Ok();
    }
    outcome.op = detail::SpiIdentityMint::mint<detail::DataPathOpTag>(op_token, 1);
    return outcome;
}

// =========================================================================
// progress — poll events
// =========================================================================

Result<ProgressResult> StripedDataPath::progress(ProgressBudget budget) {
    ProgressResult result{};
    if (budget.max_work_units == 0 || budget.timeout_ns == 0) {
        for (const auto& [tok, op] : ops_) {
            if (op.state == OpState::IN_FLIGHT) { result.more_work_likely = true; break; }
        }
        return Result<ProgressResult>::Success(std::move(result));
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(budget.timeout_ns);
    std::uint64_t work_done = 0;

    for (auto& [tok, op] : ops_) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.more_work_likely = true;
            break;
        }
        if (op.state != OpState::IN_FLIGHT) continue;
        if (work_done >= budget.max_work_units) {
            result.more_work_likely = true;
            break;
        }

        cudaError_t ce = cudaEventQuery(static_cast<cudaEvent_t>(op.event));
        if (ce == cudaSuccess) {
            aggregate_completion_status_(op);
            cudaGetLastError();
            ++result.operations_terminal;
        } else if (ce == cudaErrorNotReady) {
            ++result.operations_advanced;
        } else {
            op.state = OpState::FAILED;
            op.status = Status(StatusCode::DEVICE_ERROR,
                               "cudaEventQuery error: " +
                               std::string(cudaGetErrorString(ce)));
            cudaGetLastError();
            ++result.operations_terminal;
        }
        ++work_done;
    }

    result.work_units_consumed = work_done;
    for (const auto& [tok, op] : ops_) {
        if (op.state == OpState::IN_FLIGHT) { result.more_work_likely = true; break; }
    }
    return Result<ProgressResult>::Success(std::move(result));
}

// =========================================================================
// aggregate_completion_status_
// =========================================================================

void StripedDataPath::aggregate_completion_status_(OpEntry& op) {
    if (!op.d_status || op.entry_count == 0) {
        op.state = OpState::COMPLETED;
        op.status = Status::Ok();
        op.bytes_transferred = op.total_bytes;
        return;
    }

    std::vector<EntryCompletionStatus> h_status(op.entry_count);
    cudaError_t ce = cudaMemcpy(h_status.data(), op.d_status,
                               op.entry_count * sizeof(EntryCompletionStatus),
                               cudaMemcpyDeviceToHost);
    if (ce != cudaSuccess) {
        cudaGetLastError();
        op.state = OpState::FAILED;
        op.status = Status(StatusCode::DEVICE_ERROR,
                          "D2H completion status failed: " +
                          std::string(cudaGetErrorString(ce)));
        op.bytes_transferred = 0;
        return;
    }

    std::vector<StripedDeviceSubmitEntry> h_entries(op.entry_count);
    ce = cudaMemcpy(h_entries.data(), op.d_entries,
                    op.entry_count * sizeof(StripedDeviceSubmitEntry),
                    cudaMemcpyDeviceToHost);
    bool have_entries = (ce == cudaSuccess);
    if (!have_entries) cudaGetLastError();

    std::uint64_t confirmed_bytes = 0;
    bool any_failed = false;
    std::string first_error;

    for (std::uint32_t i = 0; i < op.entry_count; ++i) {
        const auto& s = h_status[i];
        if (s.result == 0) {
            if (have_entries) confirmed_bytes += h_entries[i].length;
        } else {
            any_failed = true;
            if (s.result == 2) op.has_timeout = true;
            if (first_error.empty()) {
                first_error = "entry " + std::to_string(i) + ": result " +
                             std::to_string(s.result);
            }
        }
    }

    if (any_failed) {
        op.state = OpState::FAILED;
        op.status = Status(StatusCode::DEVICE_ERROR, first_error);
        op.bytes_transferred = have_entries ? confirmed_bytes : 0;
    } else {
        op.state = OpState::COMPLETED;
        op.status = Status::Ok();
        op.bytes_transferred = op.total_bytes;
    }
}

// =========================================================================
// query / release
// =========================================================================

Result<DataPathSnapshot> StripedDataPath::query(DataPathOp op) const {
    const auto* entry = find_op_(op);
    if (!entry) {
        return Result<DataPathSnapshot>::Failure(
            Status(StatusCode::NOT_FOUND, "query: op not found"));
    }
    DataPathSnapshot snap;
    snap.state = entry->state;
    snap.status = entry->status;
    snap.bytes_transferred = entry->bytes_transferred;
    return Result<DataPathSnapshot>::Success(std::move(snap));
}

Status StripedDataPath::release(DataPathOp op) {
    auto* entry = find_op_(op);
    if (!entry) {
        return Status(StatusCode::NOT_FOUND, "release: op not found");
    }
    if (entry->state == OpState::IN_FLIGHT) {
        return Status(StatusCode::BUSY, "release: op is still in flight");
    }
    if (entry->arena_slot != UINT32_MAX) {
        if (entry->has_timeout) {
            arena_.release_with_timeout_leak(entry->arena_slot);
        } else {
            arena_.release(entry->arena_slot);
        }
        entry->arena_slot = UINT32_MAX;
    }
    ops_.erase(op.token());
    return Status::Ok();
}

// =========================================================================
// lookups
// =========================================================================

const StripedDataPath::StripedTarget* StripedDataPath::find_target_(
    DataPathTarget target) const {
    if (!target.valid()) return nullptr;
    auto it = targets_.find(target.token());
    if (it == targets_.end()) return nullptr;
    if (it->second.generation != target.generation()) return nullptr;
    return &it->second;
}
StripedDataPath::StripedTarget* StripedDataPath::find_target_(
    DataPathTarget target) {
    if (!target.valid()) return nullptr;
    auto it = targets_.find(target.token());
    if (it == targets_.end()) return nullptr;
    if (it->second.generation != target.generation()) return nullptr;
    return &it->second;
}
const StripedDataPath::StripedMemory* StripedDataPath::find_memory_(
    DataPathMemory memory) const {
    if (!memory.valid()) return nullptr;
    auto it = memory_regs_.find(memory.token());
    if (it == memory_regs_.end()) return nullptr;
    if (it->second.generation != memory.generation()) return nullptr;
    return &it->second;
}
StripedDataPath::StripedMemory* StripedDataPath::find_memory_(
    DataPathMemory memory) {
    if (!memory.valid()) return nullptr;
    auto it = memory_regs_.find(memory.token());
    if (it == memory_regs_.end()) return nullptr;
    if (it->second.generation != memory.generation()) return nullptr;
    return &it->second;
}
const StripedDataPath::OpEntry* StripedDataPath::find_op_(DataPathOp op) const {
    if (!op.valid()) return nullptr;
    auto it = ops_.find(op.token());
    if (it == ops_.end()) return nullptr;
    if (it->second.op_generation != op.generation()) return nullptr;
    return &it->second;
}
StripedDataPath::OpEntry* StripedDataPath::find_op_(DataPathOp op) {
    if (!op.valid()) return nullptr;
    auto it = ops_.find(op.token());
    if (it == ops_.end()) return nullptr;
    if (it->second.op_generation != op.generation()) return nullptr;
    return &it->second;
}
bool StripedDataPath::target_has_inflight_ops_(std::uint64_t token) const {
    for (const auto& [tok, op] : ops_) {
        if (op.state == OpState::IN_FLIGHT && op.target_token == token) return true;
    }
    return false;
}
bool StripedDataPath::memory_has_inflight_ops_(std::uint64_t token) const {
    for (const auto& [tok, op] : ops_) {
        if (op.state == OpState::IN_FLIGHT && op.memory_token == token) return true;
    }
    return false;
}

// =========================================================================
// test-only accessors
// =========================================================================

bool StripedDataPath::test_op_has_timeout(DataPathOp op) const {
    const auto* entry = find_op_(op);
    return entry ? entry->has_timeout : false;
}
std::uint32_t StripedDataPath::test_entry_count(DataPathOp op) const {
    const auto* entry = find_op_(op);
    return entry ? entry->entry_count : 0;
}
bool StripedDataPath::test_copy_entry_dev_idx(
    DataPathOp op, std::vector<std::uint32_t>& out) const {
    const auto* entry = find_op_(op);
    if (!entry || entry->entry_count == 0 || !entry->d_entries) return false;
    std::vector<StripedDeviceSubmitEntry> h_entries(entry->entry_count);
    cudaError_t ce = cudaMemcpy(h_entries.data(), entry->d_entries,
                               entry->entry_count * sizeof(StripedDeviceSubmitEntry),
                               cudaMemcpyDeviceToHost);
    if (ce != cudaSuccess) { cudaGetLastError(); return false; }
    out.resize(entry->entry_count);
    for (std::uint32_t i = 0; i < entry->entry_count; ++i) {
        out[i] = h_entries[i].dev_idx;
    }
    return true;
}

} // namespace tutti::data_paths::striped_local_nvme
