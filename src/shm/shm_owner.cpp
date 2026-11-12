#include "shm/shm_owner.hpp"
#include "version.hpp"
#include "report/terminal.hpp"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <time.h>

namespace rocos_co {
namespace {
using namespace rocos;
void setVars(PdVar *dest, const std::vector<Pdo> &pdos, int &count, size_t bytes) {
    for (const auto &p : pdos)
        for (const auto &v : p.entries) {
            auto &d = dest[count++];
            std::memcpy(d.name, v.name.c_str(), v.name.size() + 1);
            d.index = v.index;
            d.sub_index = v.sub_index;
            if (!setPdoBits(d, 8 * v.image_offset, 8 * v.bytes, bytes))
                throw std::runtime_error("invalid SHM descriptor");
        }
}

bool readable(const SlaveConfig &s, const AsyncCommandBox &r) noexcept {
    const auto i = r.sdo_index;
    if (i == 0x1018)
        return r.sdo_subindex <= 4;
    if (!r.sdo_subindex && (i == 0x1000 || i == 0x1001 || i == 0x1008 || i == 0x1009 || i == 0x100a ||
                            i == 0x1017 || i == 0x607c || i == 0x6083 || i == 0x6084 || i == 0x6085))
        return true;
    for (const auto &p : s.tx_pdos)
        for (const auto &e : p.entries)
            if (e.index == i && e.sub_index == r.sdo_subindex)
                return true;
    return false;
}
} // namespace

ShmOwner::ShmOwner(const MasterConfig &c) : config_(c), shm_(c.id) {
    if (!shm_.createSharedMemory() || !shm_.createPdDataMemoryProvider(c.input_bytes, c.output_bytes))
        throw std::runtime_error(shm_.lastError());
    auto &b = *shm_.ecatBus;
    b.backend_type = 3; // rocos-shm v2 backend registry, no ABI change
    b.dt = c.period_us;
    b.current_state = 0;
    b.request_state = 0;
    b.is_authorized = true;
    b.supported_cmds =
        (1u << rocos::CMD_RESET_STATS) | (1u << rocos::CMD_SDO_READ) | (1u << rocos::CMD_SDO_WRITE);
    b.status_valid = 1; // online count; interface UP alone does not prove physical link
    static_assert(sizeof(ROCOS_CO_VERSION) <= sizeof(b.ecm_version));
    std::memcpy(b.ecm_version, ROCOS_CO_VERSION, sizeof(ROCOS_CO_VERSION));
    b.slave_num = c.slaves.size();
    for (size_t i = 0; i < c.slaves.size(); ++i) {
        auto &d = b.slaves[i];
        const auto &s = c.slaves[i];
        d.id = i;
        std::memcpy(d.name, s.name.c_str(), s.name.size() + 1);
        setVars(d.input_vars, s.tx_pdos, d.input_var_num, c.input_bytes);
        setVars(d.output_vars, s.rx_pdos, d.output_var_num, c.output_bytes);
    }
    worker_ = std::thread(&ShmOwner::mailboxLoop, this);
}
ShmOwner::~ShmOwner() {
    closing_.store(true);
    if (worker_.joinable())
        worker_.join();
    shm_.disconnect();
}
void ShmOwner::updateTimestamp() noexcept {
    timespec t{};
    clock_gettime(CLOCK_REALTIME, &t);
    rocos::atomicStore(shm_.ecatBus->timestamp, long(t.tv_sec) * 1000000 + t.tv_nsec / 1000);
}
void ShmOwner::ready(const std::vector<NodeObservation> &nodes, const uint8_t *input) {
    for (size_t i = 0; i < nodes.size(); ++i) {
        auto &s = shm_.ecatBus->slaves[i];
        auto &n = nodes[i];
        if (!n.verified || n.firmware.size() >= sizeof(s.firmware_version))
            throw std::runtime_error("incomplete online identity");
        s.vendor_id = n.identity.vendor_id;
        s.product_code = n.identity.product_code;
        s.info_valid = 3;
        s.identity_source = 1;
        if (n.identity.revision) {
            s.revision_number = *n.identity.revision;
            s.info_valid |= 4;
        }
        if (n.identity.serial) {
            s.serial_number = *n.identity.serial;
            s.info_valid |= 8;
        }
        if (!n.firmware.empty()) {
            std::memcpy(s.firmware_version, n.firmware.c_str(), n.firmware.size() + 1);
            s.info_valid |= 16;
        }
        if (input && config_.output_bytes) {
            // Before clients attach: disabled drive, CSP selected, target aligned with online position.
            const uint8_t *position = nullptr;
            const auto &axis = config_.slaves[i];
            for (const auto &p : axis.tx_pdos)
                for (const auto &e : p.entries)
                    if (e.index == 0x6064 && !e.sub_index && e.bytes == 4)
                        position = input + e.image_offset;
            for (const auto &p : axis.rx_pdos)
                for (const auto &e : p.entries) {
                    auto *out = static_cast<uint8_t *>(shm_.pdOutputPtr) + e.image_offset;
                    if (e.index == 0x607a && !e.sub_index && e.bytes == 4 && position)
                        std::memcpy(out, position, 4);
                    if (e.index == 0x6060 && !e.sub_index && e.bytes == 1)
                        *out = 8;
                }
        }
    }
    if (input && config_.input_bytes)
        std::memcpy(shm_.pdInputPtr, input, config_.input_bytes);
    if (config_.state == "op")
        updateTimestamp();
    if (!shm_.publishReady())
        throw std::runtime_error(shm_.lastError());
    published_.store(true, std::memory_order_release);
    if (config_.state == "op" && !shm_.notifyClients())
        throw std::runtime_error("first SHM notification failed");
}
void ShmOwner::publish(const uint8_t *in, const CycleStats &s) noexcept {
    auto &b = *shm_.ecatBus;
    if (config_.input_bytes)
        std::memcpy(shm_.pdInputPtr, in, config_.input_bytes);
    if (previous_sync_) {
        double us = (s.last_sync - previous_sync_) / 1000.0;
        ++sample_count_;
        mean_ += (us - mean_) / sample_count_;
        min_ = sample_count_ == 1 ? us : std::min(min_, us);
        max_ = std::max(max_, us);
        rocos::atomicStore(b.current_cycle_time, us);
        rocos::atomicStore(b.avg_cycle_time, mean_);
        rocos::atomicStore(b.min_cycle_time, min_);
        rocos::atomicStore(b.max_cycle_time, max_);
    }
    previous_sync_ = s.last_sync;
    updateTimestamp();
    notification_failed_ = !shm_.notifyClients();
}
void ShmOwner::sample(uint8_t *out, size_t bytes) noexcept {
    if (bytes)
        std::memcpy(out, shm_.pdOutputPtr, bytes);
}
void ShmOwner::tick(uint64_t now, const std::vector<NodeObservation> &nodes) noexcept {
    auto &b = *shm_.ecatBus;
    unsigned count = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        bool online =
            nodes[i].last_heartbeat && now >= nodes[i].last_heartbeat &&
            now - nodes[i].last_heartbeat <= uint64_t(config_.slaves[i].heartbeat_timeout_ms) * 1000000;
        rocos::atomicStore(b.slaves[i].online, online ? 1u : 0u);
        rocos::atomicStore(b.slaves[i].runtime_valid, 1u);
        count += online;
    }
    rocos::atomicStore(b.connected_slave_num, count);
    rocos::atomicStore(b.heartbeat_mono_ns, now);
}
void ShmOwner::resetStatistics() noexcept {
    previous_sync_ = sample_count_ = 0;
    mean_ = min_ = max_ = 0;
    auto &b = *shm_.ecatBus;
    rocos::atomicStore(b.current_cycle_time, 0.0);
    rocos::atomicStore(b.avg_cycle_time, 0.0);
    rocos::atomicStore(b.min_cycle_time, 0.0);
    rocos::atomicStore(b.max_cycle_time, 0.0);
}
int32_t ShmOwner::preflight(const rocos::AsyncCommandBox &r) const noexcept {
    using namespace rocos;
    if (r.cmd_type == CMD_RESET_STATS)
        return RESULT_OK;
    if (config_.state != "preop")
        return STATE_MISMATCH;
    if (r.target_slave < 0 || size_t(r.target_slave) >= config_.slaves.size())
        return INVALID_ARGUMENT;
    if (!atomicLoad(shm_.ecatBus->slaves[r.target_slave].online))
        return SLAVE_OFFLINE;
    const auto &s = config_.slaves[r.target_slave];
    if (r.cmd_type == CMD_SDO_READ)
        return readable(s, r) ? RESULT_OK : UNSUPPORTED;
    if (r.cmd_type != CMD_SDO_WRITE || r.sdo_subindex || s.identity.vendor_id != 0x5a65726f ||
        s.identity.product_code != 0x26483052)
        return UNSUPPORTED;
    if (r.sdo_index != 0x607c && r.sdo_index != 0x6083 && r.sdo_index != 0x6084 && r.sdo_index != 0x6085)
        return UNSUPPORTED;
    if (r.data_size != 4)
        return INVALID_ARGUMENT;
    uint32_t raw;
    std::memcpy(&raw, r.data, 4);
    return r.sdo_index == 0x6085 && (!raw || raw > 65500) ? INVALID_ARGUMENT : RESULT_OK;
}
void ShmOwner::mailboxLoop() {
    using namespace rocos;
    auto pause = [] {
        timespec t{0, 250000};
        nanosleep(&t, nullptr);
    };
    bool reported = false;
    while (!closing_.load()) {
        if (!published_.load(std::memory_order_acquire)) {
            pause();
            continue;
        }
        if (!reported) {
            printOnline(std::cout, config_, *shm_.ecatBus);
            reported = true;
        }
        shm_.processCommand(
            [&](const AsyncCommandBox &req, AsyncCommandBox &res) -> int32_t {
                request_ = req;
                response_ = req;
                work_.store(Work::requested, std::memory_order_release);
                while (work_.load(std::memory_order_acquire) != Work::finished) {
                    // Destruction occurs after the event loop and its SDO owner have stopped.
                    if (closing_.load())
                        return req.cmd_type == CMD_SDO_WRITE ? OUTCOME_UNKNOWN : BACKEND_ERROR;
                    pause();
                }
                res = response_;
                work_.store(Work::idle, std::memory_order_release);
                return result_;
            },
            config_.command_timeout_ms, [&](const AsyncCommandBox &req) { return preflight(req); });
        pause();
    }
}
void ShmOwner::complete(int32_t code) noexcept {
    result_ = code;
    work_.store(Work::finished, std::memory_order_release);
}
void ShmOwner::manage(SdoTransfer &sdo, uint64_t now, bool boundary, bool permitted) noexcept {
    using namespace rocos;
    auto state = work_.load(std::memory_order_acquire);
    if (state != Work::active && state != Work::requested)
        return;
    auto &r = request_;
    if (state == Work::requested) {
        if (now >= r.deadline_mono_ns) {
            complete(DEADLINE_EXCEEDED);
            return;
        }
        if (!permitted) {
            complete(STATE_MISMATCH);
            return;
        }
        if (r.cmd_type == CMD_RESET_STATS) {
            if (boundary) {
                resetStatistics();
                response_.data_size = 0;
                complete(RESULT_OK);
            }
            return;
        }
        auto rc = preflight(r);
        if (rc != RESULT_OK) {
            complete(rc);
            return;
        }
        const auto node = config_.slaves[r.target_slave].node_id;
        sdo.start(node, r.sdo_index, r.sdo_subindex, r.cmd_type == CMD_SDO_WRITE ? r.data : nullptr,
                  r.data_size, r.deadline_mono_ns, now);
        work_.store(Work::active, std::memory_order_release);
    }
    // On shutdown the owner stops advancing the SDO and closes the private client before returning a result.
    if (!permitted) {
        bool unknown = sdo.writeDispatched();
        sdo.cancel();
        complete(unknown ? OUTCOME_UNKNOWN : BACKEND_ERROR);
        return;
    }
    switch (sdo.step(now)) {
    case SdoTransfer::Result::pending:
        return;
    case SdoTransfer::Result::done:
        response_.data_size = sdo.size();
        if (r.cmd_type == CMD_SDO_READ)
            std::memcpy(response_.data, sdo.bytes().data(), sdo.size());
        complete(RESULT_OK);
        return;
    case SdoTransfer::Result::too_large:
        complete(BUFFER_TOO_SMALL);
        return;
    case SdoTransfer::Result::remote_abort:
        response_.error_valid = 2;
        response_.sdo_abort_code = sdo.abortCode();
        complete(BACKEND_ERROR);
        return;
    case SdoTransfer::Result::deadline:
        complete(sdo.writeDispatched() ? OUTCOME_UNKNOWN : DEADLINE_EXCEEDED);
        return;
    default:
        complete(sdo.writeDispatched() ? OUTCOME_UNKNOWN : BACKEND_ERROR);
        return;
    }
}
} // namespace rocos_co
