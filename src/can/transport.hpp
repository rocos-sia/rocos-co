#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rocos_co {

/** Standard 11-bit CAN data frame. stamp_ns is a host monotonic RX timestamp. */
struct Frame {
    uint16_t id{};
    uint8_t size{}, data[8]{};
    uint64_t stamp_ns{};
    bool rtr{};
};

enum class Io { ok, again, ignored, overflow, bus_off, disconnected, invalid };
struct CanFilter {
    uint16_t id, mask;
};
struct LinkInfo {
    bool virtual_link{};
    uint32_t bitrate{};
};

/** One nonblocking endpoint, exclusively owned by the CANopen event loop.
 * receive consumes at most one datagram, including ignored frames. close is idempotent.
 */
class Transport {
  public:
    virtual ~Transport() = default;
    virtual LinkInfo open(const std::string &interface, uint32_t expected_bitrate) = 0;
    virtual void setFilters(const std::vector<CanFilter> &filters) = 0;
    virtual Io receive(Frame &frame) noexcept = 0;
    virtual Io send(const Frame &frame) noexcept = 0;
    virtual void close() noexcept = 0;
};

/** Creates a Linux SocketCAN endpoint without opening a socket. */
std::unique_ptr<Transport> socketCan();
uint64_t monotonicNs() noexcept;

} // namespace rocos_co
