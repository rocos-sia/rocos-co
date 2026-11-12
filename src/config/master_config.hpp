#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace rocos_co {

/** A mapped OD scalar. Offsets count bytes from the frame and image origins. */
struct Mapping {
    std::string name;
    uint16_t index;
    uint8_t sub_index, bytes, frame_offset;
    unsigned image_offset;
};

/** One classic PDO, in drive perspective. The master transports raw little-endian bytes. */
struct Pdo {
    uint16_t index, cob_id;
    uint8_t dlc{};
    std::vector<Mapping> entries;
};

struct Identity {
    uint32_t vendor_id, product_code;
    std::optional<uint32_t> revision, serial;
};

struct InitCommand {
    std::string name;
    uint16_t index;
    uint8_t sub_index;
    std::vector<uint8_t> data;
};

/** The index in MasterConfig::slaves is the SHM slot, independent of node_id. */
struct SlaveConfig {
    std::string name;
    uint8_t node_id;
    Identity identity;
    uint16_t heartbeat_ms, heartbeat_timeout_ms, master_heartbeat_timeout_ms;
    bool configure_pdos;
    std::vector<InitCommand> init_commands;
    std::vector<Pdo> rx_pdos, tx_pdos;
};

/** Effective configuration including budget and globally packed per-direction images. */
struct MasterConfig {
    std::string path, interface, state, verbose;
    int id;
    std::optional<int> cpu;
    uint8_t node_id;
    uint16_t heartbeat_ms;
    uint32_t period_us, bitrate, duration_ms, preop_timeout_ms, op_timeout_ms, command_timeout_ms;
    std::vector<SlaveConfig> slaves;
    unsigned input_bytes{}, output_bytes{}, frames{1}, cyclic_bits{55}, reserve_bits{};
};

/** Explicit CLI values override the corresponding YAML fields before validation. */
struct Overrides {
    std::optional<int64_t> master_id, period_us, cpu;
};

/** Reads exactly one YAML document. Throws with source location before acquiring any IPC or CAN resource. */
MasterConfig loadConfig(const std::string &file, const Overrides &overrides = {});

/** Describe the effective deployment and raw image offsets without opening the transport. */
void printConfig(std::ostream &out, const MasterConfig &config, const Overrides &overrides);

} // namespace rocos_co
