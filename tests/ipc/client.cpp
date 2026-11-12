// Compile exclusively against the client revision of the public rocos-shm headers.
#include <rocos_shm/shared_memory_config.hpp>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
using namespace rocos;
#define CHECK(e)                                                                                               \
    do {                                                                                                       \
        if (!(e))                                                                                              \
            throw std::runtime_error(#e);                                                                      \
    } while (false)

void layout(SharedMemoryConfig &c) {
    auto &h = *c.header;
    auto &b = *c.ecatBus;
    CHECK(h.abi_version == 2 && h.header_size == 80 && h.bus_size == 220088 && h.ready == 1 && h.instance_id);
    CHECK(h.input_bytes == 66 && h.output_bytes == 42 && c.pd_input_size_ == 66 && c.pd_output_size_ == 42);
    CHECK(b.backend_type == 3 && b.slave_num == 6 && b.dt == 8000 && b.is_authorized);
    CHECK(!b.state_control_supported && !b.current_state && !(b.status_valid & 4) && !b.wc_state);
    CHECK(!b.dc_sync_status && !b.dc_metric_kind && (b.status_valid & 1) &&
          atomicLoad(b.connected_slave_num) == 6);
    for (int i = 0; i < 6; ++i) {
        auto s = c.getSlave(i);
        CHECK(s.id == i && s.input_var_num == 4 && s.output_var_num == 3 && s.identity_source == 1);
        CHECK(s.online && s.runtime_valid == 1 && s.info_valid == 23 && s.revision_number == 0x20111);
        CHECK(s.input_vars[3].offset == 11 * i + 10 && s.output_vars[2].offset == 7 * i + 6);
    }
}
void mailbox(SharedMemoryConfig &c, bool op) {
    AsyncCommandBox r{}, res{};
    auto read = [&](int idx, unsigned sub, unsigned n) {
        r = AsyncCommandBox{};
        r.cmd_type = CMD_SDO_READ;
        r.target_slave = 0;
        r.sdo_index = idx;
        r.sdo_subindex = sub;
        r.data_size = n;
        return c.executeCommand(r, res);
    };
    CHECK(read(0x1001, 0, 65) == INVALID_ARGUMENT);
    if (op)
        CHECK(read(0x1001, 0, 1) == STATE_MISMATCH);
    else {
        CHECK(read(0x1001, 0, 1) == RESULT_OK && res.data_size == 1);
        CHECK(read(0x1008, 0, 64) == RESULT_OK && res.data_size == 64);
        for (unsigned i = 0; i < 64; ++i)
            CHECK(res.data[i] == i);
        CHECK(read(0x1008, 0, 4) == BUFFER_TOO_SMALL);
        CHECK(read(0x1018, 4, 4) == BACKEND_ERROR && res.error_valid == 2 && res.sdo_abort_code != 0);
        r.cmd_type = CMD_SDO_WRITE;
        r.sdo_index = 0x1600;
        r.sdo_subindex = 0;
        r.data_size = 1;
        CHECK(c.executeCommand(r, res) == UNSUPPORTED);
        r.sdo_index = 0x6085;
        r.data_size = 4;
        r.data[0] = 44;
        CHECK(c.executeCommand(r, res) == RESULT_OK);
        CHECK(read(0x6085, 0, 4) == RESULT_OK && res.data[0] == 44);
        // Simulated server delays 0x1009 response past the App's local waiting budget.
        r.sdo_index = 0x1009;
        r.data_size = 4;
        r.deadline_mono_ns = monotonicNs() + 300000000;
        CHECK(c.executeCommand(r, res, 1) == OUTCOME_UNKNOWN);
        CHECK(c.executeCommand(r, res) == BUSY);
        int32_t rc = BUSY;
        uint64_t end = monotonicNs() + 500000000;
        while (rc == BUSY && monotonicNs() < end) {
            usleep(1000);
            rc = c.pollCommand(res);
        }
        CHECK(rc == RESULT_OK);
        // Device applied the write but loses its response: no replay, no false success.
        r.cmd_type = CMD_SDO_WRITE;
        r.sdo_index = 0x6084;
        r.data_size = 4;
        r.deadline_mono_ns = monotonicNs() + 80000000;
        CHECK(c.executeCommand(r, res) == OUTCOME_UNKNOWN && !res.error_valid &&
              res.status == BOX_TIMEOUT_UNKNOWN);
    }
    r = AsyncCommandBox{};
    r.cmd_type = CMD_RESET_STATS;
    r.target_slave = -1;
    CHECK(c.executeCommand(r, res) == RESULT_OK);
    r.cmd_type = CMD_RESET_FAULT;
    CHECK(c.executeCommand(r, res) == UNSUPPORTED);
    auto &box = c.ecatBus->cmd_box;
    r.instance_id = c.header->instance_id ^ 1;
    r.cmd_id = 999;
    r.deadline_mono_ns = monotonicNs() + 500000000;
    copyCommandPayload(box, r);
    atomicStore(box.status, BOX_REQUESTED);
    uint64_t end = monotonicNs() + 1000000000;
    while (!terminalStatus(atomicLoad(box.status)) && monotonicNs() < end)
        usleep(200);
    CHECK(atomicLoad(box.status) == BOX_FAILED && box.result_code == STALE_INSTANCE);
    atomicStore(box.status, BOX_IDLE);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 1;
    try {
        int id = std::stoi(argv[1]);
        std::string mode = argv[2];
        SharedMemoryConfig c(id);
        if (mode == "early") {
            CHECK(!c.connect());
            return 0;
        }
        if (mode == "duplicate") {
            CHECK(!c.createSharedMemory());
            return 0;
        }
        bool monitor = mode == "monitor" || mode == "fault" || mode == "snapshot" || mode == "audit-width";
        uint64_t until = monotonicNs() + 3000000000;
        while (!c.connect(monitor)) {
            if (monotonicNs() >= until)
                throw std::runtime_error(c.lastError());
            usleep(1000);
        }
        if(mode=="audit-width") {
            auto v=c.findSlaveInputVarByName(0,"Position actual value");
            std::cout<<"slot=0 input=Position actual value expected=4 B actual="<<v.size<<" B\n";
            CHECK(v.size==2 && v.index==0x6064);return 0;
        }
        layout(c);
        std::cout << "INSTANCE " << c.header->instance_id << '\n';
        if (mode == "snapshot") {
            for(int i=0;i<6;++i)
                std::cout<<i<<' '<<c.getSlaveInputVarValueByName<int32_t>(i,"Position actual value")
                         <<' '<<c.getSlaveOutputVarValueByName<int32_t>(i,"Target Position")
                         <<' '<<int(c.getSlaveInputVarValueByName<int8_t>(i,"Mode display"))
                         <<' '<<int(c.getSlaveOutputVarValueByName<int8_t>(i,"Mode of operation"))<<'\n';
            return 0;
        }
        if (mode == "fault") {
            long stamp = 0;
            const auto end = monotonicNs() + 2000000000;
            while (monotonicNs() < end) {
                stamp = c.getTimestamp();
                auto heartbeat = atomicLoad(c.ecatBus->heartbeat_mono_ns);
                usleep(40000);
                if (stamp && stamp == c.getTimestamp() && atomicLoad(c.ecatBus->heartbeat_mono_ns) > heartbeat)
                    break;
            }
            CHECK(monotonicNs() < end);
            // Sole cycle consumer in this case. A read-only observer cannot consume slot 8.
            SharedMemoryConfig slots(id);
            CHECK(slots.connect());
            int value;
            while (sem_trywait(slots.sem_mutex[8]) == 0) {
            }
            usleep(40000);
            CHECK(sem_getvalue(slots.sem_mutex[8], &value) == 0 && value == 0);
            CHECK(c.getTimestamp() == stamp && c.header->ready == 1);
            return 0;
        }
        if (monitor) {
            CHECK(!c.waitForSignal(8));
            return 0;
        }
        const bool op = mode == "op";
        if (op) {
            CHECK(c.waitForSignal(8) && c.getTimestamp() > 0);
            for (int i = 0; i < 6; ++i) {
                CHECK(c.getSlaveInputVarValueByName<int32_t>(i, "Position actual value") == -1000 * (i + 1));
                CHECK(c.getSlaveOutputVarValueByName<int32_t>(i, "Target Position") == -1000 * (i + 1));
                c.setSlaveOutputVarValueByName<uint16_t>(i, "Control word", 0xf);
                c.setSlaveOutputVarValueByName<int32_t>(i, "Target Position", 500 + i);
            }
            for (int i = 0; i < 4; ++i)
                CHECK(c.waitForSignal(8));
            for (int i = 0; i < 6; ++i)
                CHECK(c.getSlaveInputVarValueByName<int32_t>(i, "Position actual value") == 500 + i);
        }
        mailbox(c, op);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
