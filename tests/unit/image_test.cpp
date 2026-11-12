#include "config/process_image.hpp"
#include "301/CO_ODinterface.h"
#include "OD.h"
#include <array>
#include <iostream>
#include <stdexcept>

#define CHECK(expr)                                                                                            \
    do {                                                                                                       \
        if (!(expr))                                                                                           \
            throw std::runtime_error(#expr);                                                                   \
    } while (false)

using namespace rocos_co;

void checkEntry(const Mapping &e, const char *name, unsigned index, unsigned bytes, unsigned frame,
                unsigned absolute) {
    CHECK(e.name == name && e.index == index && e.sub_index == 0);
    CHECK(e.bytes == bytes && e.frame_offset == frame && e.image_offset == absolute);
}

void checkDictionary() {
    CHECK(OD_CNT_RPDO == 12 && OD_CNT_TPDO == 6 && OD_CNT_ARR_1016 == 50);
    for (auto base : {0x2000, 0x2100}) {
        bool input = base == 0x2000;
        for (int p = 0; p < (input ? 12 : 6); ++p) {
            CHECK(OD_find(OD, (input ? 0x1400 : 0x1800) + p));
            CHECK(OD_find(OD, (input ? 0x1600 : 0x1a00) + p));
            for (int j = 1; j <= 8; ++j) {
                OD_IO_t io{};
                CHECK(OD_getSub(OD_find(OD, base + p), j, &io, true) == ODR_OK);
                CHECK(io.stream.dataLength == 1 && (io.stream.attribute & (input ? ODA_RPDO : ODA_TPDO)));
            }
        }
    }
    CHECK(OD_find(OD, 0x1280) && OD_find(OD, 0x1006));
}

int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    try {
        checkDictionary();
        auto c = loadConfig(argv[1]);
        CHECK(c.id == 2 && c.node_id == 127 && c.period_us == 8000 && c.bitrate == 1000000);
        CHECK(c.input_bytes == 66 && c.output_bytes == 42 && c.frames == 19 && c.cyclic_bits == 2125);
        CHECK(c.slaves.size() == 6);
        std::array<uint8_t, 66> input{};
        std::array<uint8_t, 42> output{};
        for (unsigned i = 0; i < 6; ++i) {
            const auto &s = c.slaves.at(i);
            CHECK(s.node_id == i + 1 && s.name == "eRob-J" + std::to_string(i + 1));
            CHECK(s.rx_pdos.size() == 1 && s.tx_pdos.size() == 2);
            auto &rx = s.rx_pdos[0];
            auto &tx = s.tx_pdos[0];
            auto &vx = s.tx_pdos[1];
            CHECK(rx.cob_id == 0x201 + i && rx.dlc == 7 && rx.entries.size() == 3);
            CHECK(tx.cob_id == 0x181 + i && tx.dlc == 6 && tx.entries.size() == 2);
            CHECK(vx.cob_id == 0x281 + i && vx.dlc == 5 && vx.entries.size() == 2);
            checkEntry(rx.entries[0], "Control word", 0x6040, 2, 0, 7 * i);
            checkEntry(rx.entries[1], "Target Position", 0x607a, 4, 2, 7 * i + 2);
            checkEntry(rx.entries[2], "Mode of operation", 0x6060, 1, 6, 7 * i + 6);
            checkEntry(tx.entries[0], "Status word", 0x6041, 2, 0, 11 * i);
            checkEntry(tx.entries[1], "Position actual value", 0x6064, 4, 2, 11 * i + 2);
            checkEntry(vx.entries[0], "Velocity actual value", 0x606c, 4, 0, 11 * i + 6);
            checkEntry(vx.entries[1], "Mode display", 0x6061, 1, 4, 11 * i + 10);
            const uint8_t position[]{0x27, 0, 0xfc, 0xff, 0xff, 0xff};
            const uint8_t velocity[]{0x80, 0xff, 0xff, 0xff, 8};
            CHECK(!receivePdo(tx, position, 5, input.data(), input.size()));
            CHECK(receivePdo(tx, position, 6, input.data(), input.size()));
            CHECK(receivePdo(vx, velocity, 5, input.data(), input.size()));
            int32_t value{};
            std::memcpy(&value, input.data() + i * 11 + 2, 4);
            CHECK(value == -4);
            std::memcpy(&value, input.data() + i * 11 + 6, 4);
            CHECK(value == -128);
            const uint8_t command[]{0xf, 0, 0x80, 0xff, 0xff, 0xff, 8};
            std::memcpy(output.data() + 7 * i, command, 7);
            uint8_t frame[8]{};
            CHECK(samplePdo(rx, output.data(), output.size(), frame));
            CHECK(!std::memcmp(frame, command, 7) && frame[7] == 0);
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
