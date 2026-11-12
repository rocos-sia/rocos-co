#include "shm/shm_owner.hpp"
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using namespace rocos_co;
void client(const char *exe, int id, const char *mode) {
    auto arg = std::to_string(id);
    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error("fork");
    if (!pid) {
        execl(exe, exe, arg.c_str(), mode, nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error(mode);
}
int main(int argc, char **argv) {
    if (argc != 3)
        return 1;
    try {
        auto c = loadConfig(argv[1]);
        c.id = 500000 + getpid();
        ShmOwner owner(c);
        client(argv[2], c.id, "early");
        client(argv[2], c.id, "duplicate");
        std::vector<NodeObservation> nodes(6);
        auto now = monotonicNs();
        for (auto &n : nodes) {
            n.verified = true;
            n.last_heartbeat = now;
            n.identity = c.slaves[0].identity;
            n.identity.revision = 0x20111;
            n.firmware = "fixture";
            n.nmt = 5;
        }
        std::vector<uint8_t> input(66);
        owner.tick(now, nodes);
        owner.ready(nodes, input.data());
        client(argv[2], c.id, "monitor");
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
