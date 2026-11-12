#include "config/master_config.hpp"

#include "301/CO_ODinterface.h"
#include "OD.h"
#include "rocos_shm/abi.hpp"
#include "version.hpp"
#include <yaml-cpp/yaml.h>
#include <net/if.h>
#include <sched.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace rocos_co {
namespace {

/** Retains the YAML key path and nearest source mark, including for missing keys. */
struct Field {
    YAML::Node node;
    std::string path;
    YAML::Mark mark;

    [[noreturn]] void fail(const std::string &why) const {
        throw std::runtime_error(path + " (line " + std::to_string(mark.line + 1) + "): " + why);
    }
    Field key(const std::string &name) const {
        const auto child = node[name];
        return {child, path + "." + name, child ? child.Mark() : mark};
    }
    Field item(size_t i) const {
        const auto child = node[i];
        return {child, path + "[" + std::to_string(i) + "]", child.Mark()};
    }
    size_t sequence() const {
        if (!node || !node.IsSequence())
            fail("expected sequence");
        return node.size();
    }
    void keys(std::initializer_list<std::string_view> allowed) const {
        if (!node || !node.IsMap())
            fail("expected mapping");
        std::set<std::string> seen;
        for (const auto &kv : node) {
            if (!kv.first.IsScalar())
                fail("expected scalar key");
            const auto name = kv.first.Scalar();
            Field where{kv.first, path + "." + name, kv.first.Mark()};
            if (!seen.insert(name).second)
                where.fail("duplicate key");
            if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
                where.fail("unknown key");
        }
    }
    std::string str(std::string fallback = {}) const {
        if (!node && !fallback.empty())
            return fallback;
        if (!node || !node.IsScalar() || node.Scalar().empty() || node.Scalar().find('\0') != std::string::npos)
            fail("expected nonempty string");
        return node.Scalar();
    }
    int64_t integer(int64_t low, int64_t high, std::optional<int64_t> fallback = {},
                    std::optional<int64_t> cli = {}) const {
        int64_t n;
        if (cli)
            n = *cli;
        else if (!node && fallback)
            n = *fallback;
        else {
            if (!node || !node.IsScalar() || !YAML::convert<int64_t>::decode(node, n))
                fail("expected integer");
        }
        if (n < low || n > high)
            fail(std::string(cli ? "CLI override " : "") + "value outside [" + std::to_string(low) + ", " +
                 std::to_string(high) + "]");
        return n;
    }
    std::string choice(std::initializer_list<std::string_view> options, const std::string &fallback) const {
        auto value = str(fallback);
        if (std::find(options.begin(), options.end(), value) == options.end())
            fail("unsupported value " + value);
        return value;
    }
};

unsigned frameBits(unsigned dlc) { return 47 + 8 * dlc + (33 + 8 * dlc) / 4; }

std::optional<uint32_t> optionalU32(const Field &f) {
    if (!f.node)
        return {};
    return static_cast<uint32_t>(f.integer(0, UINT32_MAX));
}

void parseMaster(const Field &m, const Overrides &cli, MasterConfig &c) {
    m.keys({"id", "cycle", "cpuidx", "duration", "verbose", "state", "transport", "interface", "bitrate",
            "node_id", "heartbeat_ms", "preop_timeout_ms", "op_timeout_ms", "command_timeout_ms"});
    c.id = m.key("id").integer(0, INT32_MAX, 0, cli.master_id);
    c.period_us = m.key("cycle").integer(1000, 1000000, 8000, cli.period_us);
    const auto cpu = m.key("cpuidx");
    if (cli.cpu || (cpu.node && !cpu.node.IsNull())) {
        c.cpu = cpu.integer(0, CPU_SETSIZE - 1, {}, cli.cpu);
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof(set), &set) || !CPU_ISSET(*c.cpu, &set))
            cpu.fail("CPU unavailable to this process");
    }
    c.duration_ms = m.key("duration").integer(0, UINT32_MAX, 0);
    c.state = m.key("state").choice({"preop", "op"}, "op");
    c.verbose = m.key("verbose").choice(
        {"silent", "errors", "warnings", "diagnostic", "info", "verbose", "cyclic"}, "diagnostic");
    m.key("transport").choice({"socketcan"}, "socketcan");
    c.interface = m.key("interface").str();
    if (c.interface.size() >= IFNAMSIZ || c.interface.find_first_of("/ \t\r\n") != std::string::npos)
        m.key("interface").fail("invalid Linux interface name");
    c.bitrate = m.key("bitrate").integer(1, 1000000);
    c.node_id = m.key("node_id").integer(1, 127);
    c.heartbeat_ms = m.key("heartbeat_ms").integer(1, 65535, 100);
    c.preop_timeout_ms = m.key("preop_timeout_ms").integer(1, UINT32_MAX, 10000);
    c.op_timeout_ms = m.key("op_timeout_ms").integer(1, UINT32_MAX, 10000);
    c.command_timeout_ms = m.key("command_timeout_ms").integer(1, 60000, 1000);
}

Identity parseIdentity(const Field &f) {
    f.keys({"vendor_id", "product_code", "revision", "serial"});
    return {static_cast<uint32_t>(f.key("vendor_id").integer(0, UINT32_MAX)),
            static_cast<uint32_t>(f.key("product_code").integer(0, UINT32_MAX)), optionalU32(f.key("revision")),
            optionalU32(f.key("serial"))};
}

bool erob(const Identity &id) { return id.vendor_id == 0x5A65726F && id.product_code == 0x26483052; }

std::vector<InitCommand> parseCommands(const Field &list, const Identity &id) {
    std::vector<InitCommand> commands;
    if (!list.node)
        return commands;
    for (size_t i = 0; i < list.sequence(); ++i) {
        auto f = list.item(i);
        f.keys({"name", "index", "sub_index", "Type", "Value"});
        InitCommand cmd{};
        cmd.name = f.key("name").str();
        cmd.index = f.key("index").integer(0, 65535);
        cmd.sub_index = f.key("sub_index").integer(0, 255);
        // Explicit eRob commissioning objects. Communication and motion control belong to their own flows.
        if (!erob(id) || cmd.sub_index != 0 ||
            (cmd.index != 0x607c && cmd.index != 0x6083 && cmd.index != 0x6084 && cmd.index != 0x6085))
            f.key("index").fail("object absent from device initialization allowlist");
        const bool signed_value = cmd.index == 0x607c;
        const std::string expected = signed_value ? "int32" : "uint32";
        if (f.key("Type").str() != expected)
            f.key("Type").fail("device object requires " + expected);
        int64_t low = signed_value ? INT32_MIN : (cmd.index == 0x6085 ? 1 : 0);
        int64_t high = signed_value ? int64_t(INT32_MAX) : (cmd.index == 0x6085 ? 65500u : UINT32_MAX);
        uint32_t n = static_cast<uint32_t>(f.key("Value").integer(low, high));
        for (unsigned j = 0; j < 4; ++j)
            cmd.data.push_back(static_cast<uint8_t>(n >> (8 * j)));
        commands.push_back(std::move(cmd));
    }
    return commands;
}

struct BusIds {
    std::set<unsigned> nodes, pdos;
};

std::vector<Pdo> parsePdos(const Field &list, bool feedback, const Identity &id, MasterConfig &c, BusIds &bus) {
    std::set<unsigned> indices;
    std::set<std::string> names;
    std::vector<Pdo> result;
    auto &bytes = feedback ? c.input_bytes : c.output_bytes;
    const unsigned base = feedback ? 0x1a00 : 0x1600;
    for (size_t i = 0; i < list.sequence(); ++i) {
        const auto f = list.item(i);
        f.keys({"index", "cob_id", "transmission_type", "entries"});
        Pdo p{};
        p.index = f.key("index").integer(base, base + (erob(id) ? 3 : 511));
        if (!indices.insert(p.index).second)
            f.key("index").fail("duplicate mapping object");
        p.cob_id = f.key("cob_id").integer(1, 0x7ff);
        if (CO_IS_RESTRICTED_CAN_ID(p.cob_id) || p.cob_id <= 0x100 || p.cob_id == 0x700 ||
            !bus.pdos.insert(p.cob_id).second)
            f.key("cob_id").fail("reserved or duplicate COB-ID");
        f.key("transmission_type").integer(1, 1);
        auto entries = f.key("entries");
        if (!entries.sequence())
            entries.fail("PDO requires at least one entry");
        for (size_t j = 0; j < entries.sequence(); ++j) {
            auto e = entries.item(j);
            e.keys({"name", "index", "sub_index", "bit_length"});
            Mapping v{};
            v.name = e.key("name").str();
            if (v.name.size() >= MAX_PD_NAME_LEN || !names.insert(v.name).second)
                e.key("name").fail("duplicate or oversized PDO name");
            if (names.size() > (feedback ? MAX_PDINPUT_NUM : MAX_PDOUTPUT_NUM))
                e.fail("SHM variable capacity exceeded");
            v.index = e.key("index").integer(1, 65535);
            v.sub_index = e.key("sub_index").integer(0, 255);
            const auto bits = e.key("bit_length").integer(1, 64);
            if (bits % 8 || p.dlc + bits / 8 > 8)
                e.key("bit_length").fail("must be byte aligned and fit classic CAN DLC 8");
            v.bytes = bits / 8;
            v.frame_offset = p.dlc;
            v.image_offset = bytes;
            p.dlc += v.bytes;
            bytes += v.bytes;
            p.entries.push_back(std::move(v));
        }
        ++c.frames;
        c.cyclic_bits += frameBits(p.dlc);
        result.push_back(std::move(p));
    }
    return result;
}

MasterConfig compile(const Field &root, const Overrides &cli) {
    root.keys({"version", "master", "slaves"});
    root.key("version").integer(1, 1);
    MasterConfig c{};
    auto master = root.key("master");
    parseMaster(master, cli, c);
    BusIds bus{{c.node_id}, {}};
    auto list = root.key("slaves");
    const size_t count = list.sequence();
    if (!count || count > MAX_SLAVE_NUM)
        list.fail("expected 1..50 SHM slots");
    if (count > OD_CNT_ARR_1016)
        list.fail("local OD heartbeat consumer capacity exceeded");
    size_t rpdo_count = 0, tpdo_count = 0;
    for (size_t i = 0; i < count; ++i) {
        auto f = list.item(i);
        f.keys({"id", "node_id", "name", "identity", "heartbeat_ms", "heartbeat_timeout_ms",
                "master_heartbeat_timeout_ms", "pdo_policy", "init_commands", "rx_pdos", "tx_pdos"});
        SlaveConfig s{};
        f.key("id").integer(i, i);
        s.node_id = f.key("node_id").integer(1, 127);
        if (!bus.nodes.insert(s.node_id).second)
            f.key("node_id").fail("duplicate node ID");
        s.name = f.key("name").str();
        if (s.name.size() >= MAX_SLAVE_NAME_LEN)
            f.key("name").fail("SHM name too long");
        s.identity = parseIdentity(f.key("identity"));
        if (erob(s.identity) && c.bitrate != 1000000)
            master.key("bitrate").fail("eRob EDS V1.5 specifies 1000000 bit/s");
        s.heartbeat_ms = f.key("heartbeat_ms").integer(1, 65535, 100);
        const int64_t jitter_ms = (c.period_us + 999) / 1000;
        s.heartbeat_timeout_ms =
            f.key("heartbeat_timeout_ms").integer(s.heartbeat_ms + jitter_ms + 1, 65535, 300);
        s.master_heartbeat_timeout_ms =
            f.key("master_heartbeat_timeout_ms").integer(c.heartbeat_ms + jitter_ms + 1, 65535, 300);
        s.configure_pdos = f.key("pdo_policy").choice({"configure", "verify"}, "verify") == "configure";
        s.init_commands = parseCommands(f.key("init_commands"), s.identity);
        s.rx_pdos = parsePdos(f.key("rx_pdos"), false, s.identity, c, bus);
        s.tx_pdos = parsePdos(f.key("tx_pdos"), true, s.identity, c, bus);
        tpdo_count += s.rx_pdos.size();
        rpdo_count += s.tx_pdos.size();
        if (tpdo_count > OD_CNT_TPDO)
            f.key("rx_pdos").fail("local OD TPDO capacity exceeded");
        if (rpdo_count > OD_CNT_RPDO)
            f.key("tx_pdos").fail("local OD RPDO capacity exceeded");
        c.slaves.push_back(std::move(s));
    }
    // One burst of heartbeats plus an EMCY and one SDO request/response pair.
    c.reserve_bits = (count + 1) * frameBits(1) + 3 * frameBits(8);
    if (uint64_t(c.cyclic_bits + c.reserve_bits) * 1000000 >= uint64_t(c.bitrate) * c.period_us)
        master.key("cycle").fail("insufficient bus budget including management reserve");
    return c;
}
} // namespace

MasterConfig loadConfig(const std::string &file, const Overrides &cli) {
    const std::string path = std::filesystem::absolute(file).lexically_normal().string();
    try {
        auto docs = YAML::LoadAllFromFile(path);
        if (docs.size() != 1)
            throw std::runtime_error("expected one YAML document");
        auto c = compile({docs[0], "$", docs[0].Mark()}, cli);
        c.path = path;
        return c;
    } catch (const std::exception &e) {
        throw std::runtime_error("Configuration error (" + path + "): " + e.what());
    }
}

void printConfig(std::ostream &out, const MasterConfig &c, const Overrides &cli) {
    out << "ROCOS-CO " << ROCOS_CO_VERSION << "\nConfiguration " << c.path << "\nSHM ID " << c.id
        << (cli.master_id ? " (CLI)" : "") << " | cycle " << c.period_us << " us"
        << (cli.period_us ? " (CLI)" : "") << " | CPU " << (c.cpu ? std::to_string(*c.cpu) : "inherited")
        << (cli.cpu ? " (CLI)" : "") << "\nSocketCAN " << c.interface << " | " << c.bitrate
        << " bit/s | master node " << unsigned(c.node_id) << " | target " << c.state << "\n";
    for (size_t i = 0; i < c.slaves.size(); ++i) {
        const auto &s = c.slaves[i];
        out << "Slot " << i << " | node " << unsigned(s.node_id) << " | " << s.name << "\n";
        for (bool input : {true, false}) {
            const auto &pdos = input ? s.tx_pdos : s.rx_pdos;
            if (pdos.empty())
                out << (input ? "Input" : "Output") << " (none)\n";
            for (const auto &p : pdos)
                for (const auto &e : p.entries)
                    out << (input ? "Input " : "Output ") << e.name << " | COB-ID 0x" << std::hex << p.cob_id
                        << " | OD 0x" << e.index << ":" << unsigned(e.sub_index) << std::dec << " | DLC "
                        << unsigned(p.dlc) << " | frame+" << unsigned(e.frame_offset) << " | image+"
                        << e.image_offset << " | " << unsigned(e.bytes) * 8 << " bit | raw drive units\n";
        }
    }
    out << "Input " << c.input_bytes << " B | Output " << c.output_bytes << " B | " << c.frames
        << " frames/cycle | " << c.cyclic_bits << " bit | " << std::fixed << std::setprecision(2)
        << 1e8 * c.cyclic_bits / (double(c.bitrate) * c.period_us) << "% | management reserve "
        << c.reserve_bits << " bit\n";
}
} // namespace rocos_co
