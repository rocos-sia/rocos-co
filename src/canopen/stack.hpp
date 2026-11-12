#pragma once
#include "can/transport.hpp"
#include "canopen/port/driver.h"
#include "config/master_config.hpp"
#include "CANopen.h"
#include <memory>

namespace rocos_co {

/** Single-thread CANopenNode owner. Each master process owns one local object dictionary.
 * The caller opens the transport and keeps it alive until this instance is destroyed.
 */
class Stack {
    struct Mirror {
        const Pdo *pdo;
        uint8_t *data;
    };
    Transport &bus_;
    RocosCanPort port_;
    std::unique_ptr<CO_t, decltype(&CO_delete)> co_;
    std::vector<Mirror> input_, output_;
    Io last_tx_{Io::ok};
    static CO_ReturnError_t transmit(void *, const CO_CANtx_t *) noexcept;
    void bindMirrors(const MasterConfig &config);

  public:
    Stack(Transport &bus, const MasterConfig &config);
    Stack(const Stack &) = delete;
    Stack &operator=(const Stack &) = delete;
    ~Stack();

    /** Routes one valid remote frame through the CANopenNode RX registration table. */
    bool dispatch(const Frame &frame) noexcept;
    /** Progress asynchronous stack services once. elapsed_us uses monotonic time. */
    bool service(uint32_t elapsed_us) noexcept;
    CO_ReturnError_t nmt(uint8_t node, CO_NMT_command_t cmd) noexcept;
    void operational() noexcept;
    bool sync() noexcept;
    /** Process event-driven local RPDOs in the feedback window of the current SYNC. */
    void readFeedback(uint8_t *image) noexcept;
    /** Copy an entire private image before requesting the local TPDO batch. */
    bool sendTargets(const uint8_t *image) noexcept;
    void discardPending() noexcept;
    CO_SDOclient_t *client() noexcept { return co_->SDOclient; }
    Io lastTx() const noexcept { return last_tx_; }
};
} // namespace rocos_co
