#include "config/master_config.hpp"
#include "version.hpp"
#include "shm/shm_owner.hpp"
#include "report/terminal.hpp"
#include <sched.h>
#include <csignal>
#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
volatile sig_atomic_t quit_requested = 0;
void handleSignal(int) { quit_requested = 1; }

void usage(std::ostream &out) {
    out << "ROCOS-CO " << ROCOS_CO_VERSION
        << "\nrocos_co_master --config FILE [--check-config] "
           "[--master-id N] [--period-us N] [--cpu N]\nrocos_co_master --help\n";
}

int64_t number(const std::string &s, const std::string &key) {
    int64_t value{};
    auto end = s.data() + s.size();
    auto parsed = std::from_chars(s.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::runtime_error(key + ": expected decimal integer");
    return value;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        std::string path;
        rocos_co::Overrides cli;
        std::set<std::string> seen;
        for (int i = 1; i < argc; ++i) {
            std::string key = argv[i];
            if (!seen.insert(key).second)
                throw std::runtime_error("duplicate option " + key);
            if (key == "--check-config")
                continue;
            if (key != "--master-id" && key != "--config" && key != "--cpu" && key != "--period-us")
                throw std::runtime_error("unknown option " + key);
            if (++i == argc)
                throw std::runtime_error(key + " requires a value");
            std::string val = argv[i];
            if (key == "--config")
                path = val;
            else if (key == "--master-id")
                cli.master_id = number(val, key);
            else if (key == "--period-us")
                cli.period_us = number(val, key);
            else
                cli.cpu = number(val, key);
        }
        if (path.empty())
            throw std::runtime_error("--config FILE required");
        auto config = rocos_co::loadConfig(path, cli);
        rocos_co::printStartup(std::cout, config, cli);
        if (seen.count("--check-config"))
            return 0;
        std::signal(SIGINT, handleSignal);
        std::signal(SIGTERM, handleSignal);
        rocos_co::ShmOwner owner(config);
        if (config.cpu) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(*config.cpu, &set);
            if (sched_setaffinity(0, sizeof(set), &set) != 0)
                throw std::runtime_error("cannot apply cycle CPU affinity");
        }
        auto transport = rocos_co::socketCan();
        auto link = transport->open(config.interface, config.bitrate);
        std::cout << "[CAN] " << config.interface << " | "
                  << (link.virtual_link ? "vcan (budget bitrate)" : "physical (verified bitrate)")
                  << " | timestamp: kernel RX / host monotonic ns\n";
        rocos_co::MasterRuntime runtime(config, *transport, owner, quit_requested);
        try {
            runtime.run();
        } catch (...) {
            rocos_co::printSummary(std::cerr, config, runtime.stats());
            throw;
        }
        rocos_co::printSummary(std::cout, config, runtime.stats());
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
