#include "backend_factory.h"
#include <unordered_map>
#include <mutex>

namespace tutti {
namespace backends {

namespace {
// Backend registry - maps BackendType to constructor function.
// Protected by mutex for thread-safe access (though registration happens at static init).
struct BackendRegistry {
    std::unordered_map<BackendType, BackendFactory::BackendConstructor> constructors;
    std::mutex mutex;

    static BackendRegistry& instance() {
        static BackendRegistry registry;
        return registry;
    }
};
} // anonymous namespace

void BackendFactory::register_backend(
    BackendType type,
    BackendConstructor constructor) {
    auto& registry = BackendRegistry::instance();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.constructors[type] = constructor;
}

std::unique_ptr<IBackend> BackendFactory::create_backend(BackendType type) {
    auto& registry = BackendRegistry::instance();
    std::lock_guard<std::mutex> lock(registry.mutex);

    auto it = registry.constructors.find(type);
    if (it == registry.constructors.end()) {
        return nullptr;
    }

    // Call constructor and wrap in unique_ptr
    IBackend* backend = it->second();
    return std::unique_ptr<IBackend>(backend);
}

std::vector<BackendType> BackendFactory::available_backends() {
    auto& registry = BackendRegistry::instance();
    std::lock_guard<std::mutex> lock(registry.mutex);

    std::vector<BackendType> types;
    types.reserve(registry.constructors.size());

    for (const auto& entry : registry.constructors) {
        types.push_back(entry.first);
    }

    return types;
}

bool BackendFactory::is_registered(BackendType type) {
    auto& registry = BackendRegistry::instance();
    std::lock_guard<std::mutex> lock(registry.mutex);
    return registry.constructors.find(type) != registry.constructors.end();
}

} // namespace backends
} // namespace tutti
