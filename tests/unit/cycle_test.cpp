#include "master/cycle.hpp"
#include <iostream>
#include <stdexcept>
#define CHECK(e)                                                                                               \
    do {                                                                                                       \
        if (!(e))                                                                                              \
            throw std::runtime_error(#e);                                                                      \
    } while (false)
using namespace rocos_co;
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        auto c = loadConfig(argv[1]);
        FeedbackWindow w(c);
        using E = FeedbackWindow::Event;
        w.begin(100, 108);
        Frame f{};
        f.id = 0x181;
        f.size = 6;
        f.stamp_ns = 99;
        CHECK(w.accept(f) == E::stale);
        f.stamp_ns = 108;
        CHECK(w.accept(f) == E::stale);
        f.stamp_ns = 105;
        f.size = 5;
        CHECK(w.accept(f) == E::wrong_dlc);
        f.size = 6;
        CHECK(w.accept(f) == E::valid && w.accept(f) == E::duplicate);
        CHECK(!w.complete() && w.missing() == 0x182);
        w.begin(108, 116);
        f.stamp_ns = 109;
        for (const auto &s : c.slaves)
            for (const auto &p : s.tx_pdos) {
                f.id = p.cob_id;
                f.size = p.dlc;
                auto expected = f.id == 0x286 ? E::complete : E::valid;
                CHECK(w.accept(f) == expected);
            }
        CHECK(w.complete());
        w.begin(116, 124);
        CHECK(!w.complete() && w.missing() == 0x181);
        uint64_t due = 8;
        CHECK(!advanceDeadline(7, 8, due) && due == 8);
        CHECK(!advanceDeadline(8, 8, due) && due == 16);
        CHECK(advanceDeadline(35, 8, due) == 2 && due == 40);
        CHECK(!advanceDeadline(35, 8, due) && due == 40);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
