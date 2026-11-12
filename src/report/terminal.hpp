#pragma once
#include "master/runtime.hpp"
#include "rocos_shm/abi.hpp"
#include <iosfwd>
namespace rocos_co {
/** Initial and final reports run outside the CAN event loop. */
void printStartup(std::ostream &, const MasterConfig &, const Overrides &);
void printOnline(std::ostream &, const MasterConfig &, const rocos::EcatBus &);
void printSummary(std::ostream &, const MasterConfig &, const CycleStats &);
} // namespace rocos_co
