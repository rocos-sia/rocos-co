#include "support/fake_transport.hpp"
#include "canopen/stack.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

#define CHECK(expr)                                                                                            \
    do {                                                                                                       \
        if (!(expr))                                                                                           \
            throw std::runtime_error(#expr);                                                                   \
    } while (false)
using namespace rocos_co;
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        auto config = loadConfig(argv[1]);
        test::FakeTransport can;
        can.open("fake", 1000000);
        Stack stack(can, config);
        CHECK(stack.service(0));
        stack.operational();
        can.tx.clear();
        CHECK(stack.sync());
        CHECK(can.tx.size() == 1 && can.tx.front().id == 0x80 && can.tx.front().size == 0);
        can.tx.clear();
        std::array<uint8_t, 42> output{};
        for (size_t i = 0; i < output.size(); ++i)
            output[i] = i + 1;
        CHECK(stack.sendTargets(output.data()));
        CHECK(can.tx.size() == 6);
        for (unsigned i = 0; i < 6; ++i) {
            auto &f = can.tx.at(i);
            CHECK(f.id == 0x201 + i && f.size == 7 && !std::memcmp(f.data, output.data() + 7 * i, 7));
            for (const auto &p : config.slaves[i].tx_pdos) {
                Frame fb{};
                fb.id = p.cob_id;
                fb.size = p.dlc;
                for (unsigned j = 0; j < fb.size; ++j)
                    fb.data[j] = p.entries.front().image_offset + j + 1;
                CHECK(stack.dispatch(fb));
            }
        }
        std::array<uint8_t, 66> input{};
        stack.readFeedback(input.data());
        for (size_t i = 0; i < input.size(); ++i)
            CHECK(input[i] == i + 1);
        can.tx_result = Io::again;
        CHECK(!stack.sendTargets(output.data()));
        stack.discardPending();
        can.tx_result = Io::ok;
        can.tx.clear();
        CHECK(stack.service(10));
        for (const auto &f : can.tx)
            CHECK(f.id < 0x201 || f.id > 0x206);
        Frame bad{};
        bad.id = 0x181;
        bad.size = 9;
        CHECK(!stack.dispatch(bad));
        bad.size = 6;
        bad.rtr = true;
        CHECK(!stack.dispatch(bad));
        can.rx_result = Io::overflow;
        CHECK(can.receive(bad) == Io::overflow);
        can.rx_result = Io::bus_off;
        CHECK(can.receive(bad) == Io::bus_off);
        can.rx_result = Io::ok;
        can.rx.push_back(bad);
        CHECK(can.receive(bad) == Io::ignored);
        can.rx.push_back(Frame{0x181, 6});
        CHECK(can.receive(bad) == Io::ok && bad.id == 0x181);
        CHECK(can.receive(bad) == Io::again);
        can.close();
        can.close();
        CHECK(!stack.sync());
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
