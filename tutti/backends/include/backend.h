#ifndef TUTTI_BACKENDS_BACKEND_H_
#define TUTTI_BACKENDS_BACKEND_H_

#include "backend_types.h"
#include <cstdint>

namespace tutti {

// Layer 2 facade + vdevice base (device_manager/include/common/).
// Forward-declared to keep this header free of libnvm / CUDA includes.
class IDeviceManager;
class IVirtualDevice;

} // namespace tutti

namespace tutti {
namespace backends {

// Device-agnostic backend interface (Layer 3).
//
// A backend adapts a transport family (NVMe, RDMA, GDS) to the upper layers.
// Unlike the v0.1 design (one backend bound to a single VDevice, batch-only),
// a backend now owns a *roster of multiple vdevices* that it acquires itself
// from the Device Manager at initialize().
//
// ── Model ────────────────────────────────────────────────────────────────
//
//   initialize(dm, cfg):
//       backend calls dm->open_vdevice(cfg.phys_id, cfg.quota_per_vdevice)
//       cfg.vdevice_count times, storing each IVirtualDevice* in a dense
//       roster. NVMe backends downcast to NvmeVirtualDevice* internally.
//
//   per-vdevice operations (defined by concrete backends, NOT here):
//       register(...)    — register an IO context for a vdevice: PRP/SGL
//                          addresses, pinned host memory, etc.
//       submit_one(...)  — submit a single IO to a chosen vdevice. For NVMe
//                          this is a device-side (__device__) function invoked
//                          from a submit kernel, so it cannot be a host virtual
//                          method and is intentionally absent from this SPI.
//
//   shutdown():
//       backend returns every vdevice to the Device Manager via
//       dm->close_vdevice() and releases its own resources.
//
// ── Why register / submit_one are not virtuals here ──────────────────────
//
// Their signatures and residency (host vs. device) are transport-specific and
// deferred to each concrete backend. This interface fixes only the parts that
// are genuinely device-agnostic: lifecycle, the vdevice roster, the selection
// handle, and metadata. Concrete backends expose register / submit_one on their
// own types (e.g. NvmeBackend + its device-side helpers).
//
// ── Threading ─────────────────────────────────────────────────────────────
//
// initialize() / shutdown() are bring-up/teardown only and need not be
// thread-safe. Roster accessors are read-only after initialize() and may be
// called concurrently.
class IBackend {
public:
    virtual ~IBackend() = default;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    // Acquire this backend's vdevice roster from the Device Manager.
    //
    // The backend opens cfg.vdevice_count vdevices from cfg.phys_id, each with
    // cfg.quota_per_vdevice resource units. The Device Manager retains ownership
    // of the returned IVirtualDevice* pointers; the backend holds references and
    // returns them in shutdown().
    //
    // dm must be non-null and already Open()'d. Returns false on failure
    // (unknown phys_id, pool exhausted, partial allocation rolled back); the
    // backend is unusable if false is returned.
    virtual bool initialize(IDeviceManager* dm, const BackendConfig& cfg) = 0;

    // Return every vdevice to the Device Manager and release backend resources.
    //
    // After shutdown() the backend is unusable except for destruction. Safe to
    // call once; a second call is a no-op.
    virtual void shutdown() = 0;

    // ── VDevice roster ───────────────────────────────────────────────────────

    // Number of vdevices this backend acquired at initialize().
    virtual uint32_t vdevice_count() const = 0;

    // The vdevice at dense index i, or nullptr if i >= vdevice_count().
    // Callers downcast to the transport subtype after checking type().
    virtual IVirtualDevice* vdevice_at(uint32_t i) const = 0;

    // The selection handle for the vdevice at dense index i.
    // Returns an invalid handle if i >= vdevice_count().
    // Pass this to the concrete backend's register / submit_one to target a
    // specific vdevice.
    virtual VDeviceHandle vdevice_handle_at(uint32_t i) const = 0;

    // ── Metadata ─────────────────────────────────────────────────────────────

    virtual BackendType     backend_type() const = 0;   // LOCAL_NVME, RDMA, GDS
    virtual const char*     backend_name() const = 0;   // "local_nvme", ...
    virtual BackendMetadata metadata()     const = 0;   // full capability record
};

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_BACKEND_H_
