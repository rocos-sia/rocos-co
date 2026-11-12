#pragma once
#include "301/CO_SDOclient.h"
#include <array>

namespace rocos_co {
/** Bounded, single-owner SDO transfer. step never sleeps. Timeout after dispatch of a write
 * leaves the device outcome unknown. Remote abort codes are retained separately from local failures.
 */
class SdoTransfer {
  public:
    enum class Result { pending, done, deadline, too_large, remote_abort, failed };
    explicit SdoTransfer(CO_SDOclient_t *client) : client_(client) {}
    ~SdoTransfer();
    SdoTransfer(const SdoTransfer &) = delete;
    SdoTransfer &operator=(const SdoTransfer &) = delete;
    /// Start a bounded download or, for null write_data, an upload with length bytes of capacity.
    /// Both times are absolute CLOCK_MONOTONIC nanoseconds. Concurrent requests are rejected.
    void start(uint8_t node, uint16_t index, uint8_t sub, const uint8_t *write_data, size_t length,
               uint64_t deadline_ns, uint64_t now_ns);
    /// Advance once. Read bytes(), size() or abortCode() after a terminal result.
    Result step(uint64_t now_ns) noexcept;
    void cancel() noexcept { finish(Result::failed); }
    const std::array<uint8_t, 64> &bytes() const noexcept { return bytes_; }
    size_t size() const noexcept { return size_; }
    uint32_t abortCode() const noexcept { return abort_; }
    bool writeDispatched() const noexcept { return dispatched_; }

  private:
    CO_SDOclient_t *client_;
    std::array<uint8_t, 64> bytes_{};
    size_t capacity_{}, size_{};
    uint64_t deadline_{}, previous_{};
    uint32_t abort_{};
    bool writing_{}, dispatched_{};
    Result result_{Result::done};
    Result finish(Result status) noexcept;
};
} // namespace rocos_co
