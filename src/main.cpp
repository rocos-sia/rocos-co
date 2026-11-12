#include "config/master_config.hpp"
#include "version.hpp"
#include <charconv>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
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
        rocos_co::printConfig(std::cout, config, cli);
        if (seen.count("--check-config"))
            return 0;
        throw std::runtime_error("M1 build supports offline --check-config only");
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
