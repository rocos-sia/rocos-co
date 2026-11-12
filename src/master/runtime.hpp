#pragma once
#include "master/commission.hpp"
#include "master/cycle.hpp"
#include <csignal>

namespace rocos_co {
/** Host timestamps (monotonic ns). A successful socket send is a host submission time. */
struct CycleStats {
    uint64_t syncs{}, complete{}, commands{}, missed{}, last_sync{}, feedback_ns{}, od_ns{}, last_target_ns{};
    uint64_t max_sync_jitter_ns{}, max_feedback_lag_ns{}, min_target_margin_ns{}, rx_overflows{},
        heartbeat_losses{};
    uint64_t ready_sync{}, last_published{}, stop_syncs{};
    uint64_t fault_ns{}, stop_status_seen{}, stop_state_confirmed{}, stop_velocity_seen{};
    /// First host RX timestamp (CLOCK_MONOTONIC ns) for a qualifying post-stop status on each slot.
    std::array<uint64_t, 50> first_stop_status_ns{};
    std::array<uint16_t, 50> stop_status{};
    std::array<int32_t, 50> stop_velocity{};
    uint16_t fault_id{};
    const char *fault{};
};

/** Event-loop callbacks. Buffers are fixed-size process images, valid only during the call. */
class ProcessExchange {
  public:
    virtual bool healthy() const noexcept { return true; }
    /// Advance mailbox work. RESET_STATS requires a boundary and SDO requires a permitted session.
    virtual void manage(SdoTransfer &, uint64_t, bool, bool) noexcept {}
    virtual ~ProcessExchange() = default;
    /// Publish verified identity and initial Input before the first OP notification. Input is null in PREOP.
    virtual void ready(const std::vector<NodeObservation> &, const uint8_t *input) = 0;
    /// Publish one complete Input and notify the App on the event-loop thread.
    virtual void publish(const uint8_t *input, const CycleStats &stats) noexcept = 0;
    /// Snapshot the client-owned Output into the private batch used for the next SYNC.
    virtual void sample(uint8_t *output, size_t size) noexcept = 0;
    /// Update backend liveness and node observations even between process-image notifications.
    virtual void tick(uint64_t now, const std::vector<NodeObservation> &nodes) noexcept = 0;
};

/** Owns one boot/maintenance/run session on an already-open transport. Recovery requires reinitialization. */
class MasterRuntime {
    const MasterConfig &cfg_;
    Transport &bus_;
    Stack stack_;
    SdoTransfer mailbox_sdo_;
    ProcessExchange &app_;
    std::vector<NodeObservation> nodes_;
    FeedbackWindow window_;
    std::array<int, 128> slot_{};
    std::vector<uint8_t> input_, output_, stop_;
    CycleStats stats_;
    const volatile sig_atomic_t &quit_;
    uint64_t last_service_{}, stage_deadline_{}, due_{}, sample_at_{}, started_{}, op_request_{};
    uint64_t target_wire_ns_{}, first_stop_batch_ns_{}, stop_sync_ns_{};
    unsigned good_{};
    bool external_sync_seen_{};
    bool armed_{}, ready_{}, sampled_{}, in_window_{}, link_ok_{true}, session_running_{}, stopping_{};
    void pump();
    void accept(const Frame &) noexcept;
    bool allOperational(uint64_t now) const noexcept;
    void fail(const char *reason, uint16_t id = 0) noexcept;
    void shutdownGroup() noexcept;
    void observeStop(const Frame &) noexcept;
    void makeStopImage() noexcept;
    void runCycles();

  public:
    MasterRuntime(const MasterConfig &, Transport &, ProcessExchange &, const volatile sig_atomic_t &quit);
    /** Commission, then run to duration, signal or a latched group fault; faults cause nonzero exit at the CLI.
     */
    void run();
    const CycleStats &stats() const noexcept { return stats_; }
};
} // namespace rocos_co
