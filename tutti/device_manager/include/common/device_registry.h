#pragma once
#include <cstdint>
#include <vector>

namespace tutti {

// Forward declarations
struct Device;

// Device registry interface
// Responsibility: Physical controller bring-up and enumeration
class IDeviceRegistry {
public:
    virtual ~IDeviceRegistry() = default;

    virtual bool Open() = 0;
    virtual void Close() = 0;

    virtual int device_count() const = 0;
    virtual const Device* device_at(int index) const = 0;
    virtual const Device* find_by_id(uint32_t device_id) const = 0;
    virtual std::vector<const Device*> list() const = 0;
};

} // namespace tutti
