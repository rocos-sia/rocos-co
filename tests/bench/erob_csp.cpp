// Explicit three-axis hardware exercise. Never registered as an automatic CTest.
#include <rocos_shm/shared_memory_config.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <csignal>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
volatile sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}

// The client is the sole output writer and cycle-slot 8 consumer. PDO values are
// raw output encoder counts, not motor counts. A stale owner never permits motion.
class Bench {
    rocos::SharedMemoryConfig shm_;
    std::ofstream trace_;
    long previous_{};
    uint64_t instance_{};
    bool connected_{};
  public:
    std::array<int32_t, 3> position{}, velocity{}, target{};
    std::array<uint16_t, 3> status{};
    std::array<int8_t, 3> mode{};
    explicit Bench(int id, const char *trace) : shm_(id), trace_(trace) {
        require(bool(trace_), "cannot open trace");
        require(shm_.connect(), shm_.lastError());
        require(shm_.header->abi_version == 2 && shm_.getSlaveNum() == 3 &&
                shm_.getDt() == 10000 && shm_.ecatBus->backend_type == 3 &&
                shm_.header->input_bytes == 33 && shm_.header->output_bytes == 21,
                "expected three-axis CANopen SHM v2, 10 ms, input/output 33/21 B");
        instance_ = shm_.header->instance_id;
        for (int i = 0; i < 3; ++i) {
            auto s = shm_.getSlave(i);
            require(s.vendor_id == 0x5a65726f && s.product_code == 0x26483052,
                    "unexpected drive identity");
            const unsigned in_indices[]{0x6041, 0x6064, 0x606c, 0x6061};
            const int in_sizes[]{2, 4, 4, 1}, in_offsets[]{0, 2, 6, 10};
            const unsigned out_indices[]{0x6040, 0x607a, 0x6060};
            const int out_sizes[]{2, 4, 1}, out_offsets[]{0, 2, 6};
            require(s.input_var_num == 4 && s.output_var_num == 3, "PDO variable count mismatch");
            for (int j = 0; j < 4; ++j)
                require(s.input_vars[j].index == in_indices[j] && s.input_vars[j].sub_index == 0 &&
                        s.input_vars[j].size == in_sizes[j] && s.input_vars[j].offset == 11*i+in_offsets[j],
                        "input PDO descriptor mismatch");
            for (int j = 0; j < 3; ++j)
                require(s.output_vars[j].index == out_indices[j] && s.output_vars[j].sub_index == 0 &&
                        s.output_vars[j].size == out_sizes[j] && s.output_vars[j].offset == 7*i+out_offsets[j],
                        "output PDO descriptor mismatch");
            std::cout << "slot=" << i << " serial=" << s.serial_number
                      << " firmware=" << s.firmware_version << '\n';
        }
        trace_ << "timestamp_us,phase,slot,status,position,velocity,mode,target,control\n";
        connected_ = true;
        tick("attach");
        for (int i = 0; i < 3; ++i)
            target[i] = shm_.getSlaveOutputVarValue<int32_t>(i, 1);
    }
    void command(uint16_t cw) {
        for (int i = 0; i < 3; ++i) {
            shm_.setSlaveOutputVarValue<int32_t>(i, 1, target[i]);
            shm_.setSlaveOutputVarValue<int8_t>(i, 2, 8);
            shm_.setSlaveOutputVarValue<uint16_t>(i, 0, cw);
        }
    }
    void tick(const char *phase, bool check_fault = true) {
        const auto deadline = rocos::monotonicNs() + 200000000;
        do {
            require(!interrupted, "interrupted");
            require(rocos::monotonicNs() < deadline, "no fresh SHM cycle in 200 ms");
            if (sem_trywait(shm_.sem_mutex[8]) != 0) { usleep(200); continue; }
            auto stamp = shm_.getTimestamp();
            if (stamp <= previous_) continue;
            require(rocos::atomicLoad(shm_.header->ready) == 1 &&
                    shm_.header->instance_id == instance_ &&
                    rocos::monotonicNs() - rocos::atomicLoad(shm_.ecatBus->heartbeat_mono_ns) < 200000000 &&
                    rocos::atomicLoad(shm_.ecatBus->connected_slave_num) == 3, "SHM owner/node unhealthy");
            previous_ = stamp;
            for (int i = 0; i < 3; ++i) {
                status[i] = shm_.getSlaveInputVarValue<uint16_t>(i, 0);
                position[i] = shm_.getSlaveInputVarValue<int32_t>(i, 1);
                velocity[i] = shm_.getSlaveInputVarValue<int32_t>(i, 2);
                mode[i] = shm_.getSlaveInputVarValue<int8_t>(i, 3);
                trace_ << stamp << ',' << phase << ',' << i << ',' << status[i] << ',' << position[i]
                       << ',' << velocity[i] << ',' << int(mode[i]) << ','
                       << shm_.getSlaveOutputVarValue<int32_t>(i, 1) << ','
                       << shm_.getSlaveOutputVarValue<uint16_t>(i, 0) << '\n';
                if (check_fault) require(!(status[i] & 8), "drive fault at slot " + std::to_string(i));
            }
            return;
        } while (true);
    }
    void hold(int cycles, const char *phase) { for (int n = 0; n < cycles; ++n) tick(phase); }
    void state(uint16_t cw, uint16_t expected, const char *phase, uint16_t alternate = 0xffff) {
        command(cw);
        for (int n = 0; n < 100; ++n) {
            tick(phase);
            bool reached = true;
            for (int i = 0; i < 3; ++i)
                reached &= ((status[i] & 0x6f) == expected || (status[i] & 0x6f) == alternate) && mode[i] == 8;
            if (reached) return;
        }
        throw std::runtime_error(std::string("state transition timeout: ") + phase);
    }
    void inspect() {
        hold(100, "inspect");
        for (int i = 0; i < 3; ++i)
            std::cout << "slot=" << i << " status=0x" << std::hex << status[i] << std::dec
                      << " position=" << position[i] << " velocity=" << velocity[i]
                      << " mode=" << int(mode[i]) << " target=" << target[i] << '\n';
        std::cout << "SHM period us min/avg/max=" << shm_.getBusMinCycleTime() << '/'
                  << shm_.getBusAvgCycleTime() << '/' << shm_.getBusMaxCycleTime() << '\n';
    }
    void move(bool visible = false) {
        for (int i = 0; i < 3; ++i) require(!(status[i] & 4), "drive already enabled");
        target = position;
        const auto start = position;
        const std::array<int32_t, 3> amplitude = visible ? std::array<int32_t, 3>{65536, 65536, 65536}
                                                       : std::array<int32_t, 3>{4096, 6144, 8192};
        const double repetitions = visible ? 3.0 : 1.0;
        for (int i = 0; i < 3; ++i)
            require(int64_t(start[i]) + amplitude[i] <= std::numeric_limits<int32_t>::max(), "target would overflow");
        state(6, 0x21, "shutdown");
        state(7, 0x23, "switch_on");
        // Stagger brake release to avoid a simultaneous three-drive supply transient.
        for (int axis = 0; axis < 3; ++axis) {
            shm_.setSlaveOutputVarValue<uint16_t>(axis, 0, 15);
            bool enabled = false;
            for (int n = 0; n < 100; ++n) {
                tick("enable");
                if ((status[axis] & 0x6f) == 0x27 && mode[axis] == 8) { enabled = true; break; }
            }
            require(enabled, "enable timeout at slot " + std::to_string(axis));
            hold(70, "brake_release"); // >500 ms after each enable per eRob manual.
        }
        std::array<int32_t, 3> peak = start, max_error{};
        const auto begin = rocos::monotonicNs();
        while (true) {
            tick("motion");
            const double t = std::min(repetitions, (rocos::monotonicNs()-begin)/6000000000.0);
            for (int i = 0; i < 3; ++i) {
                require((status[i] & 0x6f) == 0x27 && mode[i] == 8, "lost CSP enable");
                max_error[i] = std::max(max_error[i], int32_t(std::abs(int64_t(position[i])-target[i])));
                require(max_error[i] < 1024, "following error exceeds 1024 counts");
                peak[i] = std::max(peak[i], position[i]);
                target[i] = start[i] + std::lround(amplitude[i]*0.5*(1-std::cos(2*3.141592653589793*t)));
            }
            command(15);
            if (t >= repetitions) break;
        }
        hold(100, "settle");
        for (int i = 0; i < 3; ++i) {
            require(peak[i]-start[i] > amplitude[i]*0.8 && std::abs(int64_t(position[i])-start[i]) < 200,
                    "motion excursion or return position mismatch");
            std::cout << "slot=" << i << " excursion_counts=" << peak[i]-start[i]
                      << " max_tracking_error_counts=" << max_error[i]
                      << " return_error_counts=" << position[i]-start[i] << '\n';
        }
        // Firmware 150403171 completes its brake delay by entering switch-on-disabled.
        state(7, 0x23, "disable_operation", 0x40);
        hold(60, "brake_engage");
        state(0, 0x40, "disable_voltage");
        hold(40, "disabled");
        std::cout << "PASS CSP motion and disabled final state\n";
    }
    void abort() noexcept {
        if (!connected_) return;
        try {
            command(2);
            usleep(300000);
            command(0);
        } catch (...) {}
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc != 4 || (std::string(argv[2]) != "inspect" && std::string(argv[2]) != "move" &&
                      std::string(argv[2]) != "visible")) {
        std::cerr << "usage: erob_csp_bench SHM_ID inspect|move|visible TRACE.csv\n";
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        Bench bench(std::stoi(argv[1]), argv[3]);
        try {
            if (std::string(argv[2]) == "inspect") bench.inspect();
            else bench.move(std::string(argv[2]) == "visible");
        } catch (...) {
            if (std::string(argv[2]) != "inspect") bench.abort();
            throw;
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
