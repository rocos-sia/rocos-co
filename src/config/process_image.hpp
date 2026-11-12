#pragma once
#include "config/master_config.hpp"
#include <cstring>

namespace rocos_co {

/** Copies a complete feedback PDO to the private window image; rejects partial/stale-size frames. */
inline bool receivePdo(const Pdo &p, const uint8_t *payload, size_t dlc, uint8_t *image,
                       size_t bytes) noexcept {
    if (!payload || !image || p.entries.empty() || dlc != p.dlc)
        return false;
    auto start = p.entries.front().image_offset;
    if (start > bytes || dlc > bytes - start)
        return false;
    std::memcpy(image + start, payload, dlc);
    return true;
}

/** Samples the contiguous command bytes for one compiled RPDO into a classic CAN payload. */
inline bool samplePdo(const Pdo &p, const uint8_t *image, size_t bytes, uint8_t (&payload)[8]) noexcept {
    if (!image || p.entries.empty() || p.dlc > sizeof(payload))
        return false;
    auto start = p.entries.front().image_offset;
    if (start > bytes || p.dlc > bytes - start)
        return false;
    std::memcpy(payload, image + start, p.dlc);
    return true;
}
} // namespace rocos_co
