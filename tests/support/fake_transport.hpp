#pragma once
#include "can/transport.hpp"
#include <deque>
namespace rocos_co::test {
/** Controllable CAN endpoint for deterministic replay, without kernel sockets. */
class FakeTransport final : public Transport {
  public:
    bool running{};
    Io tx_result{Io::ok}, rx_result{Io::ok};
    std::deque<Frame> rx, tx;
    std::vector<CanFilter> filters;
    LinkInfo open(const std::string &, uint32_t rate) override {
        running = true;
        return {true, rate};
    }
    void setFilters(const std::vector<CanFilter> &value) override { filters = value; }
    Io receive(Frame &f) noexcept override {
        if (!running)
            return Io::disconnected;
        if (rx_result != Io::ok)
            return rx_result;
        if (rx.empty())
            return Io::again;
        auto next = rx.front();
        rx.pop_front();
        for (auto k : filters)
            if (!next.rtr && ((next.id ^ k.id) & k.mask) == 0) {
                if (next.size > 8)
                    return Io::invalid;
                f = next;
                return Io::ok;
            }
        return Io::ignored;
    }
    Io send(const Frame &f) noexcept override {
        if (!running)
            return Io::disconnected;
        if (f.id > 0x7ff || f.size > 8)
            return Io::invalid;
        if (tx_result != Io::ok)
            return tx_result;
        tx.push_back(f);
        return Io::ok;
    }
    void close() noexcept override {
        running = false;
        rx.clear();
        tx.clear();
    }
};
} // namespace rocos_co::test
