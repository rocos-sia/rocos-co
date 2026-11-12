#include "can/transport.hpp"
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/netlink.h>
#include <linux/can/raw.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <ctime>

namespace rocos_co {
namespace {
void checked(int result, const char *operation) {
    if (result < 0)
        throw std::system_error(errno, std::generic_category(), operation);
}
struct Fd {
    int value{-1};
    ~Fd() { reset(); }
    void reset() noexcept {
        if (value >= 0)
            ::close(value);
        value = -1;
    }
};

template <class F> void attributes(rtattr *a, int len, F visit) {
    for (; RTA_OK(a, len); a = RTA_NEXT(a, len))
        visit(a->rta_type & NLA_TYPE_MASK, a);
}

/** Read the named interface and controller bit timing via RTM_GETLINK. */
LinkInfo linkInfo(unsigned idx) {
    Fd fd{socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)};
    checked(fd.value, "netlink socket");
    struct Request {
        nlmsghdr header;
        ifinfomsg info;
    } req{};
    req.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
    req.header.nlmsg_type = RTM_GETLINK;
    req.header.nlmsg_flags = NLM_F_REQUEST;
    req.info.ifi_family = AF_UNSPEC;
    req.info.ifi_index = static_cast<int>(idx);
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    checked(
        sendto(fd.value, &req, req.header.nlmsg_len, 0, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)),
        "RTM_GETLINK");
    alignas(nlmsghdr) char data[16384];
    int len = recv(fd.value, data, sizeof(data), 0);
    checked(len, "RTM_GETLINK reply");
    auto *h = reinterpret_cast<nlmsghdr *>(data);
    if (!NLMSG_OK(h, len) || h->nlmsg_type != RTM_NEWLINK || h->nlmsg_len < NLMSG_LENGTH(sizeof(ifinfomsg)))
        throw std::runtime_error("invalid RTM_GETLINK reply");
    auto *info = static_cast<ifinfomsg *>(NLMSG_DATA(h));
    if (info->ifi_type != ARPHRD_CAN || !(info->ifi_flags & IFF_UP))
        throw std::runtime_error("interface must be CAN and UP");
    LinkInfo result{};
    attributes(IFLA_RTA(info), IFLA_PAYLOAD(h), [&](int type, rtattr *a) {
        if (type != IFLA_LINKINFO)
            return;
        attributes(static_cast<rtattr *>(RTA_DATA(a)), RTA_PAYLOAD(a), [&](int kind, rtattr *b) {
            if (kind == IFLA_INFO_KIND && RTA_PAYLOAD(b) == 5 && !std::memcmp(RTA_DATA(b), "vcan", 5))
                result.virtual_link = true;
            if (kind != IFLA_INFO_DATA)
                return;
            attributes(static_cast<rtattr *>(RTA_DATA(b)), RTA_PAYLOAD(b), [&](int key, rtattr *value) {
                if (key == IFLA_CAN_BITTIMING && RTA_PAYLOAD(value) >= sizeof(can_bittiming)) {
                    can_bittiming t{};
                    std::memcpy(&t, RTA_DATA(value), sizeof(t));
                    result.bitrate = t.bitrate;
                }
            });
        });
    });
    return result;
}

Io ioError() noexcept {
    switch (errno) {
    case EAGAIN:
    case EBUSY:
    case ENOBUFS:
    case EINTR:
        return Io::again;
    default:
        return Io::disconnected;
    }
}

uint64_t clockNs(clockid_t clock) noexcept {
    timespec t{};
    clock_gettime(clock, &t);
    return static_cast<uint64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

class SocketCan final : public Transport {
    Fd fd_;
    uint32_t drops_{};

  public:
    LinkInfo open(const std::string &name, uint32_t bitrate) override {
        close();
        unsigned idx = if_nametoindex(name.c_str());
        if (!idx)
            throw std::system_error(errno, std::generic_category(), name);
        const auto link = linkInfo(idx);
        if (!link.virtual_link && link.bitrate != bitrate)
            throw std::runtime_error(name + ": actual CAN bitrate differs from configuration");
        Fd next{socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, CAN_RAW)};
        checked(next.value, "SocketCAN socket");
        const int one = 1, zero = 0;
        can_err_mask_t errors = CAN_ERR_MASK;
        checked(setsockopt(next.value, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errors, sizeof(errors)),
                "CAN_RAW_ERR_FILTER");
        checked(setsockopt(next.value, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &zero, sizeof(zero)),
                "CAN_RAW_RECV_OWN_MSGS");
        checked(setsockopt(next.value, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)), "SO_RXQ_OVFL");
        checked(setsockopt(next.value, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)), "SO_TIMESTAMPNS");
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = static_cast<int>(idx);
        checked(bind(next.value, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)), "bind CAN interface");
        fd_.value = next.value;
        next.value = -1;
        drops_ = 0;
        return link;
    }
    void setFilters(const std::vector<CanFilter> &filters) override {
        std::vector<can_filter> raw;
        for (const auto &f : filters)
            raw.push_back({f.id, static_cast<canid_t>(f.mask | CAN_EFF_FLAG | CAN_RTR_FLAG)});
        checked(setsockopt(fd_.value, SOL_CAN_RAW, CAN_RAW_FILTER, raw.data(), raw.size() * sizeof(can_filter)),
                "CAN_RAW_FILTER");
    }
    Io receive(Frame &f) noexcept override {
        can_frame raw{};
        alignas(cmsghdr) char meta[CMSG_SPACE(sizeof(timespec)) + CMSG_SPACE(sizeof(uint32_t))]{};
        iovec iov{&raw, sizeof(raw)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = meta;
        msg.msg_controllen = sizeof(meta);
        auto n = recvmsg(fd_.value, &msg, MSG_DONTWAIT);
        if (n < 0)
            return ioError();
        if (n != CAN_MTU || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
            return Io::invalid;
        if (raw.can_id & CAN_ERR_FLAG) {
            if (raw.can_id & CAN_ERR_BUSOFF)
                return Io::bus_off;
            if ((raw.can_id & CAN_ERR_CRTL) &&
                (raw.data[1] & (CAN_ERR_CRTL_RX_OVERFLOW | CAN_ERR_CRTL_TX_OVERFLOW)))
                return Io::overflow;
            if (raw.can_id & (CAN_ERR_RESTARTED | CAN_ERR_TX_TIMEOUT))
                return Io::disconnected;
            return Io::ignored;
        }
        if (raw.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG) || (msg.msg_flags & MSG_CONFIRM))
            return Io::ignored;
        if (raw.can_dlc > 8)
            return Io::invalid;
        f = {};
        f.stamp_ns = monotonicNs();
        for (auto *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET)
                continue;
            if (c->cmsg_type == SO_RXQ_OVFL) {
                uint32_t drops;
                std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                if (drops != drops_) {
                    drops_ = drops;
                    return Io::overflow;
                }
            }
            if (c->cmsg_type == SCM_TIMESTAMPNS) {
                timespec stamp;
                std::memcpy(&stamp, CMSG_DATA(c), sizeof(stamp));
                uint64_t realtime = clockNs(CLOCK_REALTIME);
                uint64_t rx = uint64_t(stamp.tv_sec) * 1000000000 + stamp.tv_nsec;
                if (rx > realtime || realtime - rx > f.stamp_ns)
                    return Io::invalid;
                f.stamp_ns -= realtime - rx;
            }
        }
        f.id = raw.can_id & CAN_SFF_MASK;
        f.size = raw.can_dlc;
        std::memcpy(f.data, raw.data, f.size);
        return Io::ok;
    }
    Io send(const Frame &f) noexcept override {
        if (f.id > CAN_SFF_MASK || f.size > 8)
            return Io::invalid;
        can_frame raw{};
        raw.can_id = f.id | (f.rtr ? CAN_RTR_FLAG : 0);
        raw.can_dlc = f.size;
        std::memcpy(raw.data, f.data, f.size);
        const auto n = ::send(fd_.value, &raw, sizeof(raw), MSG_DONTWAIT);
        if (n < 0)
            return ioError();
        return n == CAN_MTU ? Io::ok : Io::invalid;
    }
    void close() noexcept override { fd_.reset(); }
};
} // namespace
uint64_t monotonicNs() noexcept { return clockNs(CLOCK_MONOTONIC); }
std::unique_ptr<Transport> socketCan() { return std::make_unique<SocketCan>(); }
} // namespace rocos_co
