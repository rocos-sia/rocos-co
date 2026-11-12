#include "report/terminal.hpp"
#include "version.hpp"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <unistd.h>

namespace rocos_co {
namespace {
void heading(std::ostream &out, const char *title) {
    auto *term = std::getenv("TERM");
    bool color = (&out == &std::cout) && isatty(STDOUT_FILENO) && !std::getenv("NO_COLOR") && term &&
                 std::string_view(term) != "dumb";
    if (color)
        out << "\033[36m";
    out << '[' << title << "] ----------------------------------------";
    if (color)
        out << "\033[0m";
    out << '\n';
}
template <class T> void row(std::ostream &out, const char *label, const T &value) {
    out << "  " << std::left << std::setw(25) << label << " : " << value << '\n';
}
} // namespace
void printStartup(std::ostream &out, const MasterConfig &c, const Overrides &cli) {
    heading(out, "Startup");
    row(out, "backend", "co (3) / CANopenNode " ROCOS_CO_STACK_REV " / SocketCAN");
    row(out, "product", ROCOS_CO_VERSION);
    heading(out, "Config");
    printConfig(out, c, cli);
    row(out, "duration (ms)", c.duration_ms);
    row(out, "verbose", c.verbose);
    row(out, "heartbeat (ms)", c.heartbeat_ms);
    row(out, "PREOP timeout (ms)", c.preop_timeout_ms);
    row(out, "OP timeout (ms)", c.op_timeout_ms);
    row(out, "command timeout (ms)", c.command_timeout_ms);
    for (const auto &s : c.slaves) {
        out << "  " << s.name << " | expected vendor/product 0x" << std::hex << s.identity.vendor_id << "/0x"
            << s.identity.product_code << std::dec << " | PDO " << (s.configure_pdos ? "configure" : "verify")
            << " | heartbeat production/consumption/master " << s.heartbeat_ms << '/' << s.heartbeat_timeout_ms
            << '/' << s.master_heartbeat_timeout_ms << " ms\n";
    }
    heading(out, "SHM");
    row(out, "ABI", rocos::SHM_ABI_VERSION);
    row(out, "header (B)", sizeof(rocos::ShmHeader));
    row(out, "bus (B)", sizeof(rocos::EcatBus));
    auto id = std::to_string(c.id);
    row(out, "bus object", "/ecm_v2_" + id);
    row(out, "input", "/pd_input_v2_" + id);
    row(out, "output", "/pd_output_v2_" + id);
    row(out, "App semaphore", "/sync_v2_" + id + "_8");
    row(out, "runtime license", "N/A");
    out.flush();
}
void printOnline(std::ostream &out, const MasterConfig &c, const rocos::EcatBus &b) {
    heading(out, "NMT");
    for (int i = 0; i < b.slave_num; ++i) {
        const auto &s = b.slaves[i];
        out << "  slot " << i << " node " << unsigned(c.slaves[i].node_id) << " " << s.name
            << " | online identity 0x" << std::hex << s.vendor_id << ":0x" << s.product_code;
        if (s.info_valid & 4)
            out << " revision 0x" << s.revision_number;
        if (s.info_valid & 8)
            out << " serial 0x" << s.serial_number;
        out << std::dec;
        if (s.info_valid & 16)
            out << " firmware " << s.firmware_version;
        out << '\n';
    }
    heading(out, "Ready");
    row(out, "startup target", c.state);
    out.flush();
}
void printSummary(std::ostream &out, const MasterConfig &c, const CycleStats &s) {
    heading(out, "Shutdown");
    row(out, "SYNC count", s.syncs);
    row(out, "complete feedback windows", s.complete);
    row(out, "normal target batches", s.commands);
    row(out, "missed cycles", s.missed);
    row(out, "last feedback RX (ns)", s.feedback_ns);
    row(out, "last OD update (ns)", s.od_ns);
    row(out, "last target submit (ns)", s.last_target_ns);
    if (s.fault) {
        row(out, "fault", s.fault);
        row(out, "fault host mono (ns)", s.fault_ns);
        out << "  fault CAN-ID              : 0x" << std::hex << s.fault_id << std::dec << '\n';
        for (size_t i = 0; i < c.slaves.size(); ++i) {
            const auto &node = c.slaves[i];
            bool match = s.fault_id == 0x700 + node.node_id || s.fault_id == 0x80 + node.node_id;
            for (const auto &p : node.tx_pdos)
                match |= s.fault_id == p.cob_id;
            if (match)
                out << "  affected slot " << i << " node " << unsigned(node.node_id) << '\n';
        }
    }
    row(out, "max host SYNC jitter (ns)", s.max_sync_jitter_ns);
    row(out, "max feedback lag (ns)", s.max_feedback_lag_ns);
    row(out, "min submit margin (ns)", s.min_target_margin_ns);
    row(out, "RX queue overflow events", s.rx_overflows);
    row(out, "node heartbeat faults", s.heartbeat_losses);
    row(out, "stop SYNC count", s.stop_syncs);
    for (size_t i = 0; i < c.slaves.size(); ++i) {
        const auto bit = uint64_t{1} << i;
        out << "  stop slot " << i << " | status ";
        if (s.stop_status_seen & bit)
            out << "0x" << std::hex << s.stop_status[i] << std::dec;
        else
            out << "unconfirmed";
        out << " | raw velocity ";
        if (s.stop_velocity_seen & bit)
            out << s.stop_velocity[i];
        else
            out << "unconfirmed";
        if (s.first_stop_status_ns[i])
            out << " | first stop status RX mono (ns) " << s.first_stop_status_ns[i];
        out << '\n';
    }
}
} // namespace rocos_co
