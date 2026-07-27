#ifndef TUTTI_BACKENDS_BACKEND_FACTORY_H_
#define TUTTI_BACKENDS_BACKEND_FACTORY_H_

#include "backend.h"
#include "backend_types.h"
#include <functional>
#include <memory>
#include <vector>

namespace tutti {
namespace backends {

// Factory for runtime backend selection and instantiation.
//
// Backends self-register at static initialization time via REGISTER_BACKEND macro.
// IO Engine and tests use create_backend() to instantiate backends by type.
//
// Thread-safe: registration happens at static init (single-threaded), creation
// can be called from multiple threads (read-only access to registry).
//
// Example usage:
//   // In backend implementation file:
//   REGISTER_BACKEND(BackendType::LOCAL_NVME, []() {
//       return new LocalNvmeBackend();
//   });
//
//   // In IO Engine:
//   auto backend = BackendFactory::create_backend(BackendType::LOCAL_NVME);
//   if (backend && backend->initialize(device_manager, cfg)) {
//       // Use backend...
//   }
class BackendFactory {
public:
    // Backend constructor function type
    using BackendConstructor = std::function<IBackend*()>;

    // Register backend constructor for a given type.
    //
    // Called at static initialization time via REGISTER_BACKEND macro.
    // Duplicate registrations for same type replace previous registration (last wins).
    //
    // type: Backend type identifier
    // constructor: Function that returns new backend instance (heap-allocated)
    static void register_backend(BackendType type, BackendConstructor constructor);

    // Create backend instance by type.
    //
    // Returns heap-allocated backend instance wrapped in unique_ptr, or nullptr if
    // type is not registered.
    //
    // Caller must call initialize() before using backend.
    //
    // type: Backend type to instantiate
    //
    // Returns unique_ptr to backend, or nullptr if type not registered.
    static std::unique_ptr<IBackend> create_backend(BackendType type);

    // List all registered backend types.
    //
    // Useful for capability discovery and testing.
    //
    // Returns vector of registered BackendType values.
    static std::vector<BackendType> available_backends();

    // Check if backend type is registered.
    //
    // type: Backend type to check
    //
    // Returns true if backend constructor is registered for this type.
    static bool is_registered(BackendType type);

private:
    // Private constructor - factory is singleton with static methods only
    BackendFactory() = default;
};

// Backend registration helper class for static initialization.
//
// Usage in backend implementation files:
//   static BackendRegistrar register_local_nvme(
//       BackendType::LOCAL_NVME,
//       []() { return new LocalNvmeBackend(); }
//   );
class BackendRegistrar {
public:
    BackendRegistrar(BackendType type, BackendFactory::BackendConstructor constructor) {
        BackendFactory::register_backend(type, constructor);
    }
};

// Convenience macro for backend registration.
//
// Usage in backend implementation file (typically at bottom of local_nvme_backend.cpp):
//   REGISTER_BACKEND(BackendType::LOCAL_NVME, []() {
//       return new LocalNvmeBackend();
//   });
#define REGISTER_BACKEND(TYPE, CONSTRUCTOR) \
    static ::tutti::backends::BackendRegistrar \
        registrar_##TYPE(TYPE, CONSTRUCTOR)

} // namespace backends
} // namespace tutti

#endif // TUTTI_BACKENDS_BACKEND_FACTORY_H_
