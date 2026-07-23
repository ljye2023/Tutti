#pragma once
#include <cstdint>
#include <cstddef>

namespace tutti {

// Opaque handle to an accelerator stream (maps to cudaStream_t, hipStream_t, etc.)
// Created by IAccelerator::create_stream(), destroyed by destroy_stream().
struct AccelStream {
    void* handle;

    AccelStream() : handle(nullptr) {}
    explicit AccelStream(void* h) : handle(h) {}

    bool is_valid() const { return handle != nullptr; }

    bool operator==(const AccelStream& other) const { return handle == other.handle; }
    bool operator!=(const AccelStream& other) const { return handle != other.handle; }
};

// Opaque handle to an accelerator event (maps to cudaEvent_t, hipEvent_t, etc.)
// Created by IAccelerator::create_event(), destroyed by destroy_event().
struct AccelEvent {
    void* handle;

    AccelEvent() : handle(nullptr) {}
    explicit AccelEvent(void* h) : handle(h) {}

    bool is_valid() const { return handle != nullptr; }

    bool operator==(const AccelEvent& other) const { return handle == other.handle; }
    bool operator!=(const AccelEvent& other) const { return handle != other.handle; }
};

// Opaque IPC handle for cross-process memory sharing.
// Maps to cudaIpcMemHandle_t, hipIpcMemHandle_t, etc.
struct IpcHandle {
    static constexpr size_t MAX_HANDLE_SIZE = 64;
    uint8_t data[MAX_HANDLE_SIZE];

    IpcHandle() {
        for (size_t i = 0; i < MAX_HANDLE_SIZE; ++i) {
            data[i] = 0;
        }
    }
};

// Dim3 structure for kernel launch grid/block dimensions.
// Kept vendor-neutral to avoid including cuda_runtime.h.
// Named Dim3 (not dim3) to avoid conflict with CUDA's dim3 type.
struct Dim3 {
    uint32_t x, y, z;

    Dim3() : x(1), y(1), z(1) {}
    explicit Dim3(uint32_t x_) : x(x_), y(1), z(1) {}
    Dim3(uint32_t x_, uint32_t y_) : x(x_), y(y_), z(1) {}
    Dim3(uint32_t x_, uint32_t y_, uint32_t z_) : x(x_), y(y_), z(z_) {}
};

} // namespace tutti
