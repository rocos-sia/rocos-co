#include "master/cycle.hpp"
namespace rocos_co {
FeedbackWindow::FeedbackWindow(const MasterConfig &c) {
    for (const auto &s : c.slaves)
        for (const auto &p : s.tx_pdos) {
            expected_[p.cob_id].dlc = p.dlc;
            ++total_;
        }
}
void FeedbackWindow::begin(uint64_t start, uint64_t end) noexcept {
    begin_ = start;
    end_ = end;
    count_ = 0;
    for (auto &e : expected_)
        e.seen = false;
}
FeedbackWindow::Event FeedbackWindow::accept(const Frame &f) noexcept {
    if (f.id >= expected_.size() || !expected_[f.id].dlc || f.rtr)
        return Event::unrelated;
    if (f.stamp_ns < begin_ || f.stamp_ns >= end_)
        return Event::stale;
    auto &p = expected_[f.id];
    if (f.size != p.dlc)
        return Event::wrong_dlc;
    if (p.seen)
        return Event::duplicate;
    p.seen = true;
    ++count_;
    return complete() ? Event::complete : Event::valid;
}
uint16_t FeedbackWindow::missing() const noexcept {
    for (size_t i = 0; i < expected_.size(); ++i)
        if (expected_[i].dlc && !expected_[i].seen)
            return i;
    return 0;
}
uint64_t advanceDeadline(uint64_t now, uint64_t period, uint64_t &due) noexcept {
    if (now < due)
        return 0;
    auto missed = (now - due) / period;
    due += (missed + 1) * period;
    return missed;
}
} // namespace rocos_co
