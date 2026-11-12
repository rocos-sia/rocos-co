#include "can/transport.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace rocos_co;
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        auto bus = socketCan();
        auto info = bus->open(argv[1], 1000000);
        if (!info.virtual_link)
            throw std::runtime_error("test requires vcan");
        bus->setFilters({{0x181, 0x7ff}, {0x201, 0x7ff}});
        Frame cmd{};
        cmd.id = 0x201;
        cmd.size = 7;
        cmd.data[6] = 8;
        uint64_t began = monotonicNs();
        if (bus->send(cmd) != Io::ok)
            throw std::runtime_error("send");
        bool got = false;
        while (monotonicNs() - began < 500000000) {
            Frame fb{};
            auto r = bus->receive(fb);
            if (r == Io::again) {
                usleep(100);
                continue;
            }
            if (r == Io::ignored)
                continue;
            if (r != Io::ok || fb.id != 0x181 || fb.size != 6 || fb.data[0] != 0x27 || fb.stamp_ns < began)
                throw std::runtime_error("unexpected data or local echo");
            got = true;
            break;
        }
        bus->close();
        bus->close();
        if (!got || bus->send(cmd) != Io::disconnected)
            throw std::runtime_error("timeout or close");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
