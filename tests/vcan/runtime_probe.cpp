#include "master/runtime.hpp"
#include <cstring>
#include <iostream>

class SimulatedApp final : public rocos_co::ProcessExchange {
    unsigned ticks_{};

  public:
    void ready(const std::vector<rocos_co::NodeObservation> &nodes, const uint8_t *input) override {
        if (nodes.size() != 6 || !nodes.back().verified)
            throw std::runtime_error("invalid identities");
        std::cout << "READY " << (input ? "op" : "preop") << '\n';
    }
    void publish(const uint8_t *, const rocos_co::CycleStats &) noexcept override { ++ticks_; }
    void sample(uint8_t *output, size_t size) noexcept override {
        if (size != 42)
            return;
        for (int i = 0; i < 6; ++i) {
            output[i * 7] = 0xf;
            int32_t p = -1000 * (i + 1) + static_cast<int32_t>(ticks_);
            std::memcpy(output + i * 7 + 2, &p, 4);
            output[i * 7 + 6] = 8;
        }
    }
    void tick(uint64_t, const std::vector<rocos_co::NodeObservation> &) noexcept override {}
};
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        auto c = rocos_co::loadConfig(argv[1]);
        auto bus = rocos_co::socketCan();
        if (!bus->open(c.interface, c.bitrate).virtual_link)
            throw std::runtime_error("test requires vcan");
        SimulatedApp app;
        volatile sig_atomic_t quit = 0;
        rocos_co::MasterRuntime master(c, *bus, app, quit);
        bool failed = false;
        try {
            master.run();
        } catch (const std::exception &e) {
            failed = true;
            std::cerr << e.what() << "\n";
        }
        const auto &s = master.stats();
        std::cout << "SYNC " << s.syncs << " ready " << s.ready_sync << " published " << s.last_published
                  << " targets " << s.commands << " feedback_ns " << s.feedback_ns << " od_ns " << s.od_ns
                  << " last_target_ns " << s.last_target_ns << " misses " << s.missed << " stop_syncs "
                  << s.stop_syncs << " status_mask " << s.stop_state_confirmed << " velocity_mask "
                  << s.stop_velocity_seen << "\n";
        return failed || s.fault || (!s.ready_sync && c.state == "op") ? 1 : 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
