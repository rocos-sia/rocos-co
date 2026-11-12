#include "canopen/sdo.hpp"
#include <algorithm>
#include <stdexcept>

namespace rocos_co {
SdoTransfer::~SdoTransfer() {
    if (result_ == Result::pending)
        CO_SDOclientClose(client_);
}
SdoTransfer::Result SdoTransfer::finish(Result status) noexcept {
    CO_SDOclientClose(client_);
    result_ = status;
    return status;
}
void SdoTransfer::start(uint8_t node, uint16_t index, uint8_t sub, const uint8_t *write_data, size_t length,
                        uint64_t deadline_ns, uint64_t now_ns) {
    if (result_ == Result::pending || !node || node > 127 || !index || length == 0 || length > bytes_.size())
        throw std::invalid_argument("invalid or overlapping SDO transfer");
    deadline_ = deadline_ns;
    previous_ = now_ns;
    writing_ = write_data != nullptr;
    dispatched_ = false;
    abort_ = 0;
    size_ = 0;
    capacity_ = length;
    if (now_ns >= deadline_) {
        result_ = Result::deadline;
        return;
    }
    uint16_t timeout = std::min<uint64_t>(65535, (deadline_ - now_ns + 999999) / 1000000);
    if (CO_SDOclient_setup(client_, 0x600 + node, 0x580 + node, node) != CO_SDO_RT_ok_communicationEnd) {
        finish(Result::failed);
        return;
    }
    auto r = writing_ ? CO_SDOclientDownloadInitiate(client_, index, sub, length, timeout, false)
                      : CO_SDOclientUploadInitiate(client_, index, sub, timeout, false);
    if (r != CO_SDO_RT_ok_communicationEnd ||
        (writing_ && CO_SDOclientDownloadBufWrite(client_, write_data, length) != length)) {
        finish(Result::failed);
        return;
    }
    result_ = Result::pending;
}
SdoTransfer::Result SdoTransfer::step(uint64_t now) noexcept {
    if (result_ != Result::pending)
        return result_;
    if (now >= deadline_)
        return finish(Result::deadline);
    const uint32_t elapsed = (now - previous_) / 1000;
    previous_ += uint64_t(elapsed) * 1000;
    CO_SDO_abortCode_t abort = CO_SDO_AB_NONE;
    size_t transferred{}, indicated{};
    CO_SDO_return_t r;
    if (writing_) {
        r = CO_SDOclientDownload(client_, elapsed, false, false, &abort, &transferred, nullptr);
        dispatched_ = true;
    } else {
        r = CO_SDOclientUpload(client_, elapsed, false, &abort, &indicated, &transferred, nullptr);
        if (indicated > capacity_ || transferred > capacity_)
            return finish(Result::too_large);
        // The stack reserves room for a full 7-byte segment before issuing its next request.
        // Drain completed segments even when the next (final) segment has only one byte.
        if (r == CO_SDO_RT_uploadDataBufferFull || r == CO_SDO_RT_ok_communicationEnd)
            size_ += CO_SDOclientUploadBufRead(client_, bytes_.data() + size_, capacity_ - size_);
    }
    if (r == CO_SDO_RT_endedWithServerAbort) {
        abort_ = abort;
        return finish(Result::remote_abort);
    }
    if (r < 0)
        return finish(abort == CO_SDO_AB_TIMEOUT ? Result::deadline : Result::failed);
    if (r == CO_SDO_RT_ok_communicationEnd) {
        if (!writing_) {
            if (size_ != transferred)
                return finish(Result::failed);
        }
        return finish(Result::done);
    }
    return result_;
}
} // namespace rocos_co
