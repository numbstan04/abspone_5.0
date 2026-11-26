// =============================================================================
// @file RtpPortAllocator.cpp
// @see RtpPortAllocator.h for design notes.
// =============================================================================

#include "RtpPortAllocator.h"
#include "ILogger.h"

#include <algorithm>

namespace vcs { namespace gateway {

namespace {
	static const char* const kLogTag = "[RtpPortAllocator]";
}

RtpPortAllocator::RtpPortAllocator(uint16_t rangeStart,
                                   uint16_t rangeEnd,
                                   uint16_t stride)
    : rangeStart_(rangeStart)
    , rangeEnd_(rangeEnd)
    , stride_(stride < 2 ? 2 : stride)  // never less than 2
{
    Reset(rangeStart_, rangeEnd_, stride_);
}

void RtpPortAllocator::Reset(uint16_t rangeStart, uint16_t rangeEnd, uint16_t stride) {
    std::lock_guard<std::mutex> lock(mutex_);

    rangeStart_ = rangeStart;
    rangeEnd_   = rangeEnd;
    stride_     = (stride < 2) ? 2 : stride;

    freeAudioPorts_.clear();
    // Build the free list. Each "slot" is a pair (start, start+stride)
    // but we only track the audio port (the lower one) and the video
    // port is implicitly audio + 1, since the default stride is 2 and
    // SDP assumes audio/video are adjacent even/odd numbers.
    //
    // For stride == 2 (the common case), we skip every other port to
    // keep audio/video as a contiguous even/odd pair.
    const uint16_t effectiveStride = stride_ == 2 ? 2 : stride_;

    for (uint16_t p = rangeStart_; p + effectiveStride <= rangeEnd_; p += effectiveStride) {
        freeAudioPorts_.insert(p);
    }

    VCS_GATEWAY_LOG_INFO(
        "%s Reset: range=[%u, %u], stride=%u, freePairs=%zu",
        kLogTag, rangeStart_, rangeEnd_, stride_, freeAudioPorts_.size());
}

bool RtpPortAllocator::Allocate(uint16_t& audioPort, uint16_t& videoPort) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (freeAudioPorts_.empty()) {
        VCS_GATEWAY_LOG_WARNING(
            "%s Allocate FAILED: pool exhausted (rangeStart=%u, rangeEnd=%u, stride=%u)",
            kLogTag, rangeStart_, rangeEnd_, stride_);
        return false;
    }

    // Take the lowest free port simple, deterministic, and naturally
    // recycles in FIFO order as calls come and go.
    auto it = freeAudioPorts_.begin();
    audioPort = *it;
    videoPort = static_cast<uint16_t>(audioPort + 1);  // audio/video as even/odd pair
    freeAudioPorts_.erase(it);

    VCS_GATEWAY_LOG_INFO(
        "%s Allocate OK: audio=%u, video=%u, remaining=%zu",
        kLogTag, audioPort, videoPort, freeAudioPorts_.size());
    return true;
}

void RtpPortAllocator::Release(uint16_t audioPort, uint16_t videoPort) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (audioPort < rangeStart_ || audioPort >= rangeEnd_) {
        VCS_GATEWAY_LOG_WARNING(
            "%s Release: audioPort=%u out of range [%u, %u), ignored",
            kLogTag, audioPort, rangeStart_, rangeEnd_);
        return;
    }
    if (videoPort != audioPort + 1) {
        VCS_GATEWAY_LOG_WARNING(
            "%s Release: videoPort=%u does not match audioPort+1=%u, ignored",
            kLogTag, videoPort, audioPort + 1);
        return;
    }
    if (freeAudioPorts_.count(audioPort) != 0) {
        // Already in the pool  avoid duplicate insertion.
        return;
    }

    freeAudioPorts_.insert(audioPort);
    VCS_GATEWAY_LOG_INFO(
        "%s Release: audio=%u, video=%u returned to pool, freePairs=%zu",
        kLogTag, audioPort, videoPort, freeAudioPorts_.size());
}

size_t RtpPortAllocator::FreeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return freeAudioPorts_.size();
}

size_t RtpPortAllocator::TotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (rangeEnd_ <= rangeStart_) return 0;
    const uint16_t effectiveStride = stride_ == 2 ? 2 : stride_;
    return static_cast<size_t>(rangeEnd_ - rangeStart_) / effectiveStride;
}

} // namespace gateway
} // namespace vcs
