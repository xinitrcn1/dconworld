#pragma once

#include <cstdint>

namespace physics {
    // PhysicalObject.BodyChunkConnection.Type
    enum class connection_type : uint8_t {
        normal,
        pull,
        push,
    };
}
