#pragma once
#include "can/transport.hpp"
#include "config/master_config.hpp"
#include <array>

namespace rocos_co {

/** Receiver-side membership of a single SYNC window, keyed by configured COB-ID.
 * Frames have no remote cycle counter. Host-received frames can only be attributed by time and ID.
 */
class FeedbackWindow {
    struct Expected {
        uint8_t dlc{};
        bool seen{};
    };
    std::array<Expected, 2048> expected_{};
    unsigned total_{}, count_{};
    uint64_t begin_{}, end_{};

  public:
    enum class Event { unrelated, valid, complete, stale, duplicate, wrong_dlc };
    explicit FeedbackWindow(const MasterConfig &);
    void begin(uint64_t sync_ns, uint64_t next_sync_ns) noexcept;
    Event accept(const Frame &) noexcept;
    bool complete() const noexcept { return count_ == total_; }
    uint16_t missing() const noexcept;
};

/** Advance an absolute timer to the first deadline strictly after now; count skipped beats. */
uint64_t advanceDeadline(uint64_t now, uint64_t period, uint64_t &due) noexcept;
} // namespace rocos_co
