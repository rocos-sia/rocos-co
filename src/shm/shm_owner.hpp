#pragma once
#include "master/runtime.hpp"
#include "rocos_shm/shared_memory_config.hpp"
#include <atomic>
#include <thread>

namespace rocos_co {
/** Owner of the v2 IPC objects. Construct before opening CAN; destroy after the CAN event loop stops.
 * A non-RT thread owns the shared mailbox. The event loop only touches its private transaction copy.
 */
class ShmOwner final : public ProcessExchange {
    enum class Work { idle, requested, active, finished };
    const MasterConfig &config_;
    rocos::SharedMemoryConfig shm_;
    std::thread worker_;
    std::atomic<bool> closing_{false}, published_{false};
    std::atomic<Work> work_{Work::idle};
    rocos::AsyncCommandBox request_{}, response_{};
    int32_t result_{};
    uint64_t previous_sync_{}, sample_count_{};
    double mean_{}, min_{}, max_{};
    bool notification_failed_{};
    void mailboxLoop();
    int32_t preflight(const rocos::AsyncCommandBox &) const noexcept;
    void complete(int32_t result) noexcept;
    void resetStatistics() noexcept;
    void updateTimestamp() noexcept;

  public:
    explicit ShmOwner(const MasterConfig &);
    ~ShmOwner() override;
    /// True once the v2 owner has published the verified instance for client attachment.
    bool isReady() const noexcept { return published_.load(std::memory_order_acquire); }
    void ready(const std::vector<NodeObservation> &, const uint8_t *input) override;
    void publish(const uint8_t *input, const CycleStats &) noexcept override;
    void sample(uint8_t *output, size_t bytes) noexcept override;
    void tick(uint64_t now, const std::vector<NodeObservation> &) noexcept override;
    bool healthy() const noexcept override { return !notification_failed_; }
    void manage(SdoTransfer &, uint64_t now, bool boundary, bool permitted) noexcept override;
};
} // namespace rocos_co
