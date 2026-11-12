#pragma once
#include "canopen/sdo.hpp"
#include "canopen/stack.hpp"
#include <array>
#include <functional>

namespace rocos_co {
/** Device identity and latest external NMT status. A boot-up (state 0) never proves PREOP. */
struct NodeObservation {
    Identity identity{};
    std::string firmware;
    uint8_t nmt{};
    uint64_t last_heartbeat{};
    bool verified{};
};

/** PREOP commissioner. Calls pump between nonblocking SDO steps to advance the stack, receive
 * heartbeats, and observe shutdown. All nodes and transactions share one absolute stage deadline.
 */
class Commissioner {
  public:
    Commissioner(const MasterConfig &, Stack &, std::vector<NodeObservation> &, std::function<void()> pump);
    /// Complete the per-node verification sequence or throw with the failing startup step.
    void run();

  private:
    const MasterConfig &config_;
    Stack &stack_;
    std::vector<NodeObservation> &nodes_;
    std::function<void()> pump_;
    SdoTransfer sdo_;
    uint64_t deadline_{};
    uint8_t node_{};
    bool transfer(uint16_t index, uint8_t sub, const uint8_t *write, size_t limit, bool optional = false);
    uint32_t read(uint16_t index, uint8_t sub, unsigned width);
    void expect(uint16_t index, uint8_t sub, unsigned width, uint32_t value);
    void write(uint16_t index, uint8_t sub, unsigned width, uint32_t value);
    void map(const Pdo &p, bool configure);
    void prepareNode(size_t slot);
    void waitStep();
};
} // namespace rocos_co
