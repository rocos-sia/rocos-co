#include "master/runtime.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <time.h>

namespace rocos_co {
namespace {
void sleepUntil(uint64_t ns) noexcept {
    timespec t{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr);
}
} // namespace
MasterRuntime::MasterRuntime(const MasterConfig &c, Transport &b, ProcessExchange &a,
                             const volatile sig_atomic_t &quit)
    : cfg_(c), bus_(b), stack_(b, c), mailbox_sdo_(stack_.client()), app_(a), nodes_(c.slaves.size()),
      window_(c), input_(c.input_bytes), output_(c.output_bytes), stop_(c.output_bytes), quit_(quit) {
    slot_.fill(-1);
    for (size_t i = 0; i < c.slaves.size(); ++i)
        slot_[c.slaves[i].node_id] = i;
    unsigned bits = 0;
    for (const auto &s : c.slaves)
        for (const auto &p : s.rx_pdos)
            bits += 47 + 8 * p.dlc + (33 + 8 * p.dlc) / 4;
    target_wire_ns_ = (uint64_t(bits + c.reserve_bits) * 1000000000 + c.bitrate - 1) / c.bitrate;
    last_service_ = monotonicNs();
}
void MasterRuntime::fail(const char *reason, uint16_t id) noexcept {
    if (!stats_.fault) {
        stats_.fault = reason;
        stats_.fault_id = id;
        stats_.fault_ns = monotonicNs();
        if (id >= 0x701 && id <= 0x77f)
            ++stats_.heartbeat_losses;
    }
}
void MasterRuntime::accept(const Frame &f) noexcept {
    if (stopping_ && stop_sync_ns_ && f.stamp_ns >= stop_sync_ns_)
        observeStop(f);
    if (f.id == 0 && f.size == 2 && (!f.data[1] || f.data[1] == cfg_.node_id)) {
        fail("external master NMT command", f.id);
        return;
    }
    if (f.id == 0x80) {
        external_sync_seen_ = true;
        if (stopping_ && !stop_sync_ns_ && first_stop_batch_ns_ && f.stamp_ns >= first_stop_batch_ns_)
            stop_sync_ns_ = f.stamp_ns;
        fail("external SYNC producer", f.id);
        return;
    }
    if (f.id >= 0x701 && f.id <= 0x77f) {
        const unsigned node = f.id - 0x700;
        if (node == cfg_.node_id) {
            fail("master node ID conflict", f.id);
            return;
        }
        int i = slot_[node];
        if (i >= 0) {
            if (f.size != 1) {
                fail("invalid heartbeat DLC", f.id);
                return;
            }
            auto &obs = nodes_[i];
            if ((armed_ && f.data[0] != 5) || (obs.verified && !op_request_ && f.data[0] != 127))
                fail("node left expected NMT state", f.id);
            obs.nmt = f.data[0];
            obs.last_heartbeat = f.stamp_ns;
        }
    }
    if (f.id > 0x80 && f.id <= 0xff && slot_[f.id - 0x80] >= 0 && (f.size != 8 || f.data[0] || f.data[1])) {
        fail("node EMCY", f.id);
        return;
    }
    if (in_window_) {
        auto event = window_.accept(f);
        if (event == FeedbackWindow::Event::wrong_dlc || event == FeedbackWindow::Event::duplicate ||
            event == FeedbackWindow::Event::stale) {
            if (armed_ && !stats_.fault) {
                ++stats_.missed;
                fail("invalid feedback window", f.id);
            }
            return;
        }
        if (event == FeedbackWindow::Event::valid || event == FeedbackWindow::Event::complete) {
            if (!stack_.dispatch(f)) {
                fail("unregistered feedback PDO", f.id);
                return;
            }
            if (event == FeedbackWindow::Event::complete) {
                stats_.feedback_ns = f.stamp_ns;
                stats_.max_feedback_lag_ns =
                    std::max(stats_.max_feedback_lag_ns, f.stamp_ns - stats_.last_sync);
                stack_.readFeedback(input_.data());
                stats_.od_ns = monotonicNs();
                ++stats_.complete;
                if (armed_ && ready_ && !stats_.fault) {
                    stats_.last_published = stats_.syncs;
                    app_.publish(input_.data(), stats_);
                }
            }
            return;
        }
    }
    stack_.dispatch(f);
}
void MasterRuntime::pump() {
    for (unsigned i = 0; i < 128; ++i) {
        Frame f;
        auto r = bus_.receive(f);
        if (r == Io::again)
            break;
        if (r == Io::ignored)
            continue;
        if (r != Io::ok) {
            if (r == Io::overflow)
                ++stats_.rx_overflows;
            if (r == Io::bus_off || r == Io::disconnected)
                link_ok_ = false;
            fail(r == Io::bus_off ? "bus-off" : r == Io::overflow ? "RX overflow" : "CAN receive failure");
            break;
        }
        accept(f);
    }
    auto now = monotonicNs();
    uint32_t elapsed = (now - last_service_) / 1000;
    last_service_ += uint64_t(elapsed) * 1000;
    if (!stack_.service(elapsed))
        fail("CANopen stack error or reset");
    if (!stopping_)
        for (size_t i = 0; i < nodes_.size(); ++i)
            if (nodes_[i].verified &&
                now - nodes_[i].last_heartbeat > uint64_t(cfg_.slaves[i].heartbeat_timeout_ms) * 1000000)
                fail("heartbeat timeout", 0x700 + cfg_.slaves[i].node_id);
    app_.tick(now, nodes_);
    if (!app_.healthy())
        fail("SHM cycle notification failed");
    app_.manage(mailbox_sdo_, now, false, !stats_.fault && !stopping_);
}
bool MasterRuntime::allOperational(uint64_t now) const noexcept {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const auto &n = nodes_[i];
        if (!n.verified || n.nmt != 5 || n.last_heartbeat < op_request_ ||
            now - n.last_heartbeat > uint64_t(cfg_.slaves[i].heartbeat_timeout_ms) * 1000000)
            return false;
    }
    return true;
}
void MasterRuntime::runCycles() {
    const uint64_t period = uint64_t(cfg_.period_us) * 1000;
    due_ = monotonicNs();
    stage_deadline_ = due_ + uint64_t(cfg_.op_timeout_ms) * 1000000;
    while (!quit_ && !stats_.fault) {
        pump();
        auto now = monotonicNs();
        if (stats_.fault)
            break;
        if (!ready_ && now >= stage_deadline_) {
            fail("OP observation deadline", window_.missing());
            break;
        }
        if (ready_ && cfg_.duration_ms && now - started_ >= uint64_t(cfg_.duration_ms) * 1000000)
            break;
        if (now >= due_) {
            uint64_t skipped = advanceDeadline(now, period, due_);
            stats_.missed += skipped;
            if (armed_ && (skipped || (in_window_ && !window_.complete()) || (ready_ && !sampled_))) {
                if (!skipped)
                    ++stats_.missed;
                fail("cycle deadline or missing TPDO", window_.missing());
                break;
            }
            if (allOperational(now)) {
                if (armed_ && in_window_ && window_.complete() && ++good_ >= 3 && !ready_) {
                    ready_ = true;
                    stats_.ready_sync = stats_.syncs;
                    started_ = now;
                    app_.ready(nodes_, input_.data());
                }
                armed_ = true;
            }
            app_.manage(mailbox_sdo_, now, true, !stats_.fault && !stopping_);
            window_.begin(now, due_);
            in_window_ = true;
            sampled_ = false;
            if (!stack_.sync()) {
                fail("SYNC transmit backlog");
                break;
            }
            if (stats_.syncs) {
                const uint64_t delta = now - stats_.last_sync;
                stats_.max_sync_jitter_ns =
                    std::max(stats_.max_sync_jitter_ns, delta > period ? delta - period : period - delta);
            }
            ++stats_.syncs;
            stats_.last_sync = now;
            if (!cfg_.input_bytes && ready_) {
                ++stats_.complete;
                stats_.last_published = stats_.syncs;
                app_.publish(nullptr, stats_);
            }
            sample_at_ = due_ - period / 2;
        }
        if (ready_ && !sampled_ && now >= sample_at_) {
            if (!window_.complete()) {
                ++stats_.missed;
                fail("TPDO missing at output phase", window_.missing());
                break;
            }
            if (now + target_wire_ns_ >= due_) {
                ++stats_.missed;
                fail("insufficient time for target batch");
                break;
            }
            app_.sample(output_.data(), output_.size());
            if (!stack_.sendTargets(output_.data())) {
                fail("target transmit backlog");
                break;
            }
            stats_.last_target_ns = monotonicNs();
            const uint64_t margin = due_ > stats_.last_target_ns ? due_ - stats_.last_target_ns : 0;
            stats_.min_target_margin_ns =
                stats_.commands ? std::min(stats_.min_target_margin_ns, margin) : margin;
            ++stats_.commands;
            sampled_ = true;
        }
        sleepUntil(std::min(due_, now + 150000));
    }
}
void MasterRuntime::makeStopImage() noexcept {
    std::fill(stop_.begin(), stop_.end(), 0);
    for (const auto &s : cfg_.slaves) {
        const uint8_t *position = nullptr;
        for (const auto &p : s.tx_pdos)
            for (const auto &e : p.entries)
                if (e.index == 0x6064 && !e.sub_index && e.bytes == 4)
                    position = input_.data() + e.image_offset;
        for (const auto &p : s.rx_pdos)
            for (const auto &e : p.entries) {
                auto *dst = stop_.data() + e.image_offset;
                if (e.index == 0x6040 && !e.sub_index && e.bytes == 2)
                    dst[0] = 2; // CiA 402 quick stop
                if (e.index == 0x607a && !e.sub_index && e.bytes == 4 && position)
                    std::memcpy(dst, position, 4);
                if (e.index == 0x6060 && !e.sub_index && e.bytes == 1)
                    dst[0] = 8;
            }
    }
}
void MasterRuntime::observeStop(const Frame &f) noexcept {
    for (size_t i = 0; i < cfg_.slaves.size(); ++i)
        for (const auto &p : cfg_.slaves[i].tx_pdos) {
            if (f.id != p.cob_id || f.size != p.dlc)
                continue;
            const uint64_t bit = uint64_t{1} << i;
            for (const auto &e : p.entries) {
                if (e.sub_index)
                    continue;
                if (e.index == 0x6041 && e.bytes == 2) {
                    uint16_t sw;
                    std::memcpy(&sw, f.data + e.frame_offset, 2);
                    stats_.stop_status[i] = sw;
                    stats_.stop_status_seen |= bit;
                    // CiA 402 quick-stop active, switch-on disabled, or ready to switch on.
                    if ((sw & 0x6f) == 0x07 || (sw & 0x4f) == 0x40 || (sw & 0x6f) == 0x21) {
                        if (!stats_.first_stop_status_ns[i])
                            stats_.first_stop_status_ns[i] = f.stamp_ns;
                        stats_.stop_state_confirmed |= bit;
                    } else
                        stats_.stop_state_confirmed &= ~bit;
                }
                if (e.index == 0x606c && e.bytes == 4) {
                    std::memcpy(&stats_.stop_velocity[i], f.data + e.frame_offset, 4);
                    stats_.stop_velocity_seen |= bit;
                }
            }
        }
}
void MasterRuntime::shutdownGroup() noexcept {
    stack_.discardPending();
    armed_ = false;
    ready_ = false;
    in_window_ = false;
    if (!link_ok_ || !session_running_)
        return;
    // Keep generating SYNC for quick-stop RPDOs. Feedback loss cannot confirm a mechanical stop.
    stopping_ = true;
    makeStopImage();
    const uint64_t period = uint64_t(cfg_.period_us) * 1000;
    const uint64_t end = monotonicNs() + uint64_t(cfg_.command_timeout_ms) * 1000000;
    uint64_t next = monotonicNs() + period;
    uint64_t status_required = 0, velocity_expected = 0;
    for (size_t i = 0; i < cfg_.slaves.size(); ++i) {
        const auto &axis = cfg_.slaves[i];
        const uint64_t bit = uint64_t{1} << i;
        for (const auto &pdo : axis.rx_pdos)
            for (const auto &entry : pdo.entries)
                if (entry.index == 0x6040 && !entry.sub_index && entry.bytes == 2)
                    status_required |= bit;
        for (const auto &pdo : axis.tx_pdos)
            for (const auto &entry : pdo.entries)
                if ((status_required & bit) && entry.index == 0x606c && !entry.sub_index && entry.bytes == 4)
                    velocity_expected |= bit;
    }
    auto confirmed = [&] {
        return (stop_.empty() || stop_sync_ns_) &&
               (stats_.stop_state_confirmed & status_required) == status_required &&
               (stats_.stop_velocity_seen & velocity_expected) == velocity_expected;
    };
    while (link_ok_ && monotonicNs() < end) {
        if (!stack_.sendTargets(stop_.data())) {
            fail("stop target transmit failed");
            stack_.discardPending();
            break;
        }
        if (!first_stop_batch_ns_)
            first_stop_batch_ns_ = monotonicNs();
        while (monotonicNs() < next && monotonicNs() < end && link_ok_) {
            pump();
            sleepUntil(std::min<uint64_t>(next, monotonicNs() + 150000));
            if (confirmed())
                break;
        }
        if (confirmed())
            break;
        if (!link_ok_ || monotonicNs() >= end)
            break;
        auto sent = monotonicNs();
        if (!external_sync_seen_) {
            if (!stack_.sync()) {
                fail("stop SYNC transmit failed", 0x80);
                break;
            }
            if (!stop_sync_ns_)
                stop_sync_ns_ = sent;
            ++stats_.stop_syncs;
        }
        next = sent + period;
    }
    if (!confirmed())
        fail("group stop feedback unconfirmed");
    if (link_ok_)
        for (const auto &s : cfg_.slaves)
            if (stack_.nmt(s.node_id, CO_NMT_ENTER_PRE_OPERATIONAL) != CO_ERROR_NO)
                fail("shutdown NMT transmit failed");
}
void MasterRuntime::run() {
    Commissioner commissioner(cfg_, stack_, nodes_, [&] {
        pump();
        if (stats_.fault || quit_)
            throw std::runtime_error(stats_.fault ? stats_.fault : "interrupted during PREOP");
    });
    commissioner.run();
    if (cfg_.state == "preop") {
        app_.ready(nodes_, nullptr);
        started_ = monotonicNs();
        uint64_t boundary = started_;
        while (!quit_ && !stats_.fault) {
            pump();
            auto now = monotonicNs();
            if (now >= boundary) {
                advanceDeadline(now, uint64_t(cfg_.period_us) * 1000, boundary);
                app_.manage(mailbox_sdo_, now, true, !stats_.fault);
            }
            if (cfg_.duration_ms && now - started_ >= uint64_t(cfg_.duration_ms) * 1000000)
                break;
            sleepUntil(now + 150000);
        }
    } else {
        stack_.operational();
        op_request_ = monotonicNs();
        session_running_ = true;
        try {
            for (const auto &s : cfg_.slaves)
                if (stack_.nmt(s.node_id, CO_NMT_ENTER_OPERATIONAL) != CO_ERROR_NO)
                    throw std::runtime_error("NMT start failed");
            runCycles();
        } catch (...) {
            shutdownGroup();
            throw;
        }
        shutdownGroup();
    }
    if (stats_.fault)
        throw std::runtime_error(std::string(stats_.fault) + " CAN-ID " + std::to_string(stats_.fault_id));
}
} // namespace rocos_co
