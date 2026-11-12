#include "master/commission.hpp"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <time.h>

namespace rocos_co {
namespace {
std::string address(uint8_t node, uint16_t idx, uint8_t sub) {
    std::ostringstream s;
    s << "node " << unsigned(node) << " OD 0x" << std::hex << idx << ':' << unsigned(sub);
    return s.str();
}
uint32_t decode(const uint8_t *data, unsigned width) {
    uint32_t n = 0;
    for (unsigned i = 0; i < width; ++i)
        n |= uint32_t(data[i]) << (8 * i);
    return n;
}
} // namespace
Commissioner::Commissioner(const MasterConfig &c, Stack &stack, std::vector<NodeObservation> &nodes,
                           std::function<void()> pump)
    : config_(c), stack_(stack), nodes_(nodes), pump_(std::move(pump)), sdo_(stack.client()) {}

void Commissioner::waitStep() {
    pump_();
    if (monotonicNs() >= deadline_)
        throw std::runtime_error("PREOP deadline at node " + std::to_string(node_));
    timespec t{0, 80000};
    nanosleep(&t, nullptr);
}
bool Commissioner::transfer(uint16_t idx, uint8_t sub, const uint8_t *data, size_t limit, bool optional) {
    auto now = monotonicNs();
    auto end = std::min<uint64_t>(deadline_, now + uint64_t(config_.command_timeout_ms) * 1000000);
    sdo_.start(node_, idx, sub, data, limit, end, now);
    while (true) {
        auto r = sdo_.step(monotonicNs());
        if (r == SdoTransfer::Result::done)
            return true;
        if (r == SdoTransfer::Result::remote_abort && optional &&
            (sdo_.abortCode() == CO_SDO_AB_NOT_EXIST || sdo_.abortCode() == CO_SDO_AB_SUB_UNKNOWN))
            return false;
        if (r != SdoTransfer::Result::pending) {
            std::ostringstream s;
            s << address(node_, idx, sub) << (data ? " download" : " upload");
            if (r == SdoTransfer::Result::remote_abort)
                s << " SDO abort 0x" << std::hex << sdo_.abortCode();
            else if (r == SdoTransfer::Result::too_large)
                s << " response exceeds capacity";
            else if (r == SdoTransfer::Result::deadline)
                s << " timeout";
            else
                s << " transfer failure";
            if (data && sdo_.writeDispatched() && r != SdoTransfer::Result::remote_abort)
                s << " (write outcome unknown)";
            throw std::runtime_error(s.str());
        }
        waitStep();
    }
}
uint32_t Commissioner::read(uint16_t i, uint8_t s, unsigned width) {
    transfer(i, s, nullptr, width);
    if (sdo_.size() != width)
        throw std::runtime_error(address(node_, i, s) + " unexpected size");
    return decode(sdo_.bytes().data(), width);
}
void Commissioner::expect(uint16_t i, uint8_t s, unsigned width, uint32_t expected) {
    auto actual = read(i, s, width);
    if (actual != expected) {
        std::ostringstream msg;
        msg << address(node_, i, s) << " expected 0x" << std::hex << expected << ", got 0x" << actual;
        throw std::runtime_error(msg.str());
    }
}
void Commissioner::write(uint16_t i, uint8_t s, unsigned width, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned j = 0; j < width; ++j)
        bytes[j] = static_cast<uint8_t>(value >> (8 * j));
    transfer(i, s, bytes, width);
}
void Commissioner::map(const Pdo &p, bool configure) {
    const uint16_t comm = p.index - 0x200;
    const uint32_t current = read(comm, 1, 4);
    // Preserve the supported RTR policy bit. The bus exclusively uses standard data frames.
    const uint32_t cob = (current & 0x40000000u) | p.cob_id;
    if (configure) {
        write(comm, 1, 4, current | 0x80000000);
        write(p.index, 0, 1, 0);
    }
    for (size_t j = 0; j < p.entries.size() && configure; ++j) {
        auto &e = p.entries[j];
        write(p.index, j + 1, 4, (uint32_t(e.index) << 16) | (uint32_t(e.sub_index) << 8) | (8 * e.bytes));
    }
    if (configure) {
        write(p.index, 0, 1, p.entries.size());
        write(comm, 2, 1, 1);
        write(comm, 1, 4, cob);
    }
    expect(comm, 1, 4, cob);
    expect(comm, 2, 1, 1);
    expect(p.index, 0, 1, p.entries.size());
    for (size_t j = 0; j < p.entries.size(); ++j) {
        const auto &e = p.entries[j];
        expect(p.index, j + 1, 4, (uint32_t(e.index) << 16) | (uint32_t(e.sub_index) << 8) | (8 * e.bytes));
    }
}
void Commissioner::prepareNode(size_t slot) {
    const auto &s = config_.slaves[slot];
    auto &obs = nodes_[slot];
    node_ = s.node_id;
    const uint64_t request = monotonicNs();
    if (stack_.nmt(node_, CO_NMT_ENTER_PRE_OPERATIONAL) != CO_ERROR_NO)
        throw std::runtime_error("PREOP NMT send failed");
    const unsigned highest = read(0x1018, 0, 1);
    expect(0x1018, 1, 4, s.identity.vendor_id);
    obs.identity.vendor_id = s.identity.vendor_id;
    expect(0x1018, 2, 4, s.identity.product_code);
    obs.identity.product_code = s.identity.product_code;
    for (unsigned sub : {3, 4}) {
        auto expected = sub == 3 ? s.identity.revision : s.identity.serial;
        auto &value = sub == 3 ? obs.identity.revision : obs.identity.serial;
        if (sub <= highest || expected)
            value = read(0x1018, sub, 4);
        if (expected && value != expected)
            throw std::runtime_error(address(node_, 0x1018, sub) + " identity mismatch");
    }
    if (transfer(0x100a, 0, nullptr, 63, true)) {
        obs.firmware.assign(reinterpret_cast<const char *>(sdo_.bytes().data()), sdo_.size());
        if (!obs.firmware.empty() && obs.firmware.back() == '\0')
            obs.firmware.pop_back();
        if (obs.firmware.find('\0') != std::string::npos)
            throw std::runtime_error(address(node_, 0x100a, 0) + " malformed firmware string");
    }
    if (read(0x1017, 0, 2) != s.heartbeat_ms)
        write(0x1017, 0, 2, s.heartbeat_ms);
    expect(0x1017, 0, 2, s.heartbeat_ms);
    while (obs.last_heartbeat < request || obs.nmt != 127)
        waitStep();
    uint32_t consumer = (uint32_t(config_.node_id) << 16) | s.master_heartbeat_timeout_ms;
    if (read(0x1016, 1, 4) != consumer)
        write(0x1016, 1, 4, consumer);
    expect(0x1016, 1, 4, consumer);
    expect(0x1005, 0, 4, 0x80);
    for (auto &cmd : s.init_commands) {
        transfer(cmd.index, cmd.sub_index, cmd.data.data(), cmd.data.size());
        expect(cmd.index, cmd.sub_index, cmd.data.size(), decode(cmd.data.data(), cmd.data.size()));
    }
    for (bool feedback : {true, false}) {
        const auto &pdos = feedback ? s.tx_pdos : s.rx_pdos;
        for (const auto &p : pdos)
            map(p, s.configure_pdos);
        if (s.identity.vendor_id == 0x5a65726f && s.identity.product_code == 0x26483052) {
            for (unsigned k = 0; k < 4; ++k) {
                const uint16_t index = (feedback ? 0x1a00 : 0x1600) + k;
                if (std::any_of(pdos.begin(), pdos.end(), [&](const Pdo &p) { return p.index == index; }))
                    continue;
                auto cob = read(index - 0x200, 1, 4);
                if (!(cob & 0x80000000)) {
                    if (!s.configure_pdos)
                        throw std::runtime_error(address(node_, index - 0x200, 1) + " unplanned enabled PDO");
                    write(index - 0x200, 1, 4, cob | 0x80000000);
                    expect(index - 0x200, 1, 4, cob | 0x80000000);
                }
            }
        }
    }
    obs.verified = true;
}
void Commissioner::run() {
    deadline_ = monotonicNs() + uint64_t(config_.preop_timeout_ms) * 1000000;
    for (size_t i = 0; i < config_.slaves.size(); ++i)
        prepareNode(i);
}
} // namespace rocos_co
