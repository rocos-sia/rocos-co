#include "canopen/stack.hpp"
#include "OD.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

namespace rocos_co {
namespace {
std::atomic_flag active = ATOMIC_FLAG_INIT;
void odCheck(ODR_t result, uint16_t index) {
    if (result != ODR_OK)
        throw std::runtime_error("master OD initialization failed at index " + std::to_string(index));
}
void stackCheck(CO_ReturnError_t r, uint32_t detail) {
    if (r != CO_ERROR_NO)
        throw std::runtime_error("CANopenNode init error " + std::to_string(r) + " OD detail " +
                                 std::to_string(detail));
}
} // namespace
CO_ReturnError_t Stack::transmit(void *opaque, const CO_CANtx_t *tx) noexcept {
    auto &self = *static_cast<Stack *>(opaque);
    Frame f{};
    f.id = tx->ident & 0x7ff;
    f.rtr = tx->ident & 0x800;
    f.size = tx->DLC;
    std::memcpy(f.data, tx->data, tx->DLC);
    const auto r = self.bus_.send(f);
    if (r != Io::ok)
        self.last_tx_ = r;
    if (r == Io::again)
        return CO_ERROR_TX_BUSY;
    return r == Io::ok ? CO_ERROR_NO : CO_ERROR_SYSCALL;
}
Stack::Stack(Transport &bus, const MasterConfig &config)
    : bus_(bus), port_{this, transmit}, co_(nullptr, CO_delete) {
    if (active.test_and_set())
        throw std::logic_error("one local CANopen dictionary per process");
    try {
        // Restore pristine dictionary storage when the process restarts its service.
        static const auto initial_ram = OD_RAM;
        static const auto initial_comm = OD_PERSIST_COMM;
        OD_RAM = initial_ram;
        OD_PERSIST_COMM = initial_comm;
        for (unsigned i = 0; i < OD->size; ++i)
            OD->list[i].extension = nullptr;
        co_.reset(CO_new(nullptr, nullptr));
        if (!co_)
            throw std::bad_alloc();
        stackCheck(CO_CANinit(co_.get(), &port_, config.bitrate / 1000), 0);
        OD_PERSIST_COMM.x1005_COB_ID_SYNCMessage = 0x40000080;
        OD_PERSIST_COMM.x1006_communicationCyclePeriod = config.period_us;
        OD_PERSIST_COMM.x1017_producerHeartbeatTime = config.heartbeat_ms;
        for (size_t i = 0; i < config.slaves.size(); ++i) {
            auto &s = config.slaves[i];
            OD_PERSIST_COMM.x1016_consumerHeartbeatTime[i] =
                (uint32_t(s.node_id) << 16) | s.heartbeat_timeout_ms;
        }
        bindMirrors(config);
        uint32_t detail = 0;
        auto r = CO_CANopenInit(co_.get(), nullptr, nullptr, OD, nullptr, 0, 1, 1000,
                                std::min<uint32_t>(config.command_timeout_ms, 65535), false, config.node_id,
                                &detail);
        stackCheck(r, detail);
        r = CO_CANopenInitPDO(co_.get(), co_->em, OD, config.node_id, &detail);
        stackCheck(r, detail);
        // The single SDO client changes partner at runtime. Include all expected SDO responses.
        std::vector<CanFilter> filters;
        for (unsigned i = 0; i < co_->CANmodule->rxSize; ++i) {
            auto &b = co_->CANmodule->rxArray[i];
            if (b.CANrx_callback && b.ident)
                filters.push_back(
                    {static_cast<uint16_t>(b.ident & 0x7ff), static_cast<uint16_t>(b.mask & 0x7ff)});
        }
        filters.push_back({0, 0x7ff});
        filters.push_back({0x80, 0x780}); // SYNC, EMCY
        for (const auto &s : config.slaves)
            filters.push_back({static_cast<uint16_t>(0x580 + s.node_id), 0x7ff});
        bus_.setFilters(filters);
        CO_CANsetNormalMode(co_->CANmodule);
    } catch (...) {
        co_.reset();
        active.clear();
        throw;
    }
}
Stack::~Stack() {
    co_.reset();
    active.clear();
}

void Stack::bindMirrors(const MasterConfig &c) {
    for (const auto &s : c.slaves)
        for (bool input : {true, false}) {
            auto &mirrors = input ? input_ : output_;
            for (const auto &p : input ? s.tx_pdos : s.rx_pdos) {
                auto n = mirrors.size();
                uint16_t comm = (input ? 0x1400 : 0x1800) + n, map = (input ? 0x1600 : 0x1a00) + n,
                         obj = (input ? 0x2000 : 0x2100) + n;
                auto *ce = OD_find(OD, comm);
                auto *me = OD_find(OD, map);
                odCheck(OD_set_u32(ce, 1, p.cob_id, true), comm);
                // Drives use type 1. The master consumes feedback immediately and explicitly phases its
                // targets.
                odCheck(OD_set_u8(ce, 2, 254, true), comm);
                odCheck(OD_set_u8(me, 0, p.dlc, true), map);
                for (unsigned k = 1; k <= p.dlc; ++k)
                    odCheck(OD_set_u32(me, k, (uint32_t(obj) << 16) | (k << 8) | 8, true), map);
                ODR_t err;
                auto *data = static_cast<uint8_t *>(OD_getPtr(OD_find(OD, obj), 1, 1, &err));
                odCheck(err, obj);
                mirrors.push_back({&p, data});
            }
        }
}
bool Stack::dispatch(const Frame &f) noexcept {
    if (f.id > 0x7ff || f.size > 8 || f.rtr)
        return false;
    RocosCanRxMessage msg{};
    msg.ident = f.id;
    msg.dlc = f.size;
    std::memcpy(msg.data, f.data, f.size);
    return rocos_can_dispatch(co_->CANmodule, &msg);
}
bool Stack::service(uint32_t elapsed_us) noexcept {
    auto reset = CO_process(co_.get(), false, elapsed_us, nullptr);
    return reset == CO_RESET_NOT &&
           (co_->CANmodule->CANerrorStatus & (CO_CAN_ERRTX_BUS_OFF | CO_CAN_ERRTX_OVERFLOW)) == 0;
}
CO_ReturnError_t Stack::nmt(uint8_t node, CO_NMT_command_t command) noexcept {
    if (!node || node > 127)
        return CO_ERROR_ILLEGAL_ARGUMENT;
    return CO_NMT_sendCommand(co_->NMT, command, node);
}
void Stack::operational() noexcept {
    CO_NMT_sendInternalCommand(co_->NMT, CO_NMT_ENTER_OPERATIONAL);
    CO_process(co_.get(), false, 0, nullptr);
}
bool Stack::sync() noexcept {
    last_tx_ = Io::ok;
    return CO_SYNCsend(co_->SYNC) == CO_ERROR_NO && !co_->CANmodule->CANtxCount && last_tx_ == Io::ok;
}
void Stack::readFeedback(uint8_t *image) noexcept {
    CO_process_RPDO(co_.get(), false, 0, nullptr);
    for (const auto &m : input_)
        std::memcpy(image + m.pdo->entries.front().image_offset, m.data, m.pdo->dlc);
}
bool Stack::sendTargets(const uint8_t *image) noexcept {
    last_tx_ = Io::ok;
    for (size_t i = 0; i < output_.size(); ++i) {
        const auto &m = output_[i];
        std::memcpy(m.data, image + m.pdo->entries.front().image_offset, m.pdo->dlc);
        CO_TPDOsendRequest(&co_->TPDO[i]);
    }
    CO_process_TPDO(co_.get(), false, 0, nullptr);
    return !co_->CANmodule->CANtxCount && last_tx_ == Io::ok;
}
void Stack::discardPending() noexcept { rocos_can_discard(co_->CANmodule); }
} // namespace rocos_co
