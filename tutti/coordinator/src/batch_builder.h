#pragma once

#include "io_engine/include/io_types.h"
#include <vector>
#include <cstdint>

namespace tutti {
namespace coordinator {

class BatchBuilder {
public:
    BatchBuilder() = default;
    ~BatchBuilder() = default;

    std::vector<std::vector<tutti::IoRequest>> pack_requests(
        IoRequest* requests,
        uint32_t count,
        uint32_t max_batch_size);

private:
    // Future: Add request reordering, priority scheduling
};

} // namespace coordinator
} // namespace tutti
