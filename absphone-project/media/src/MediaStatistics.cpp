/**
 * @file MediaStatistics.cpp
 * @brief Kind-aware media statistics store behind MediaInfoSnapshot
 *
 * See MediaStatistics.h for the threading model. In short:
 *   - per-packet counters are atomics (several producers, no lock);
 *   - everything else is serialised by one mutex, with the 1 Hz Tick as the
 *     only writer of the derived per-second values.
 */

#include "MediaStatistics.h"
#include "ILogger.h"
#include <memory>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace vcs { namespace media {

namespace {

std::size_t KindIndex(vcs::MediaStreamKind kind) {
    switch (kind) {
    case vcs::MediaStreamKind::Audio:          return 0;
    case vcs::MediaStreamKind::Video:          return 1;
    case vcs::MediaStreamKind::SecondaryVideo: return 2;
    case vcs::MediaStreamKind::Data:           return 3;
    }
    return 0;
}

vcs::MediaStreamKind KindFromIndex(std::size_t index) {
    switch (index) {
    case 0:  return vcs::MediaStreamKind::Audio;
    case 1:  return vcs::MediaStreamKind::Video;
    case 2:  return vcs::MediaStreamKind::SecondaryVideo;
    default: return vcs::MediaStreamKind::Data;
    }
}

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint32_t ToBps(uint64_t bytes) {
    const uint64_t bits = bytes * 8;
    const uint64_t cap = std::numeric_limits<uint32_t>::max();
    return static_cast<uint32_t>(std::min(bits, cap));
}

uint32_t ToU32(uint64_t value) {
    return static_cast<uint32_t>(
        std::min(value, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
}

struct StreamStats {
    // ---- Hot path: atomic only ----
    std::atomic<uint64_t> bytesSent{0};
    std::atomic<uint64_t> bytesReceived{0};
    std::atomic<uint64_t> packetsSent{0};
    std::atomic<uint64_t> packetsReceived{0};
    std::atomic<uint64_t> packetsLost{0};
    std::atomic<uint64_t> jitterUs{0};
    std::atomic<uint64_t> rttMs{0};
    std::atomic<int64_t>  lastPacketAtMs{0};

    // ---- Guarded by Impl::mutex ----
    bool active = false;
    uint32_t localSsrc = 0;
    vcs::NegotiatedCodec codec;
    std::string remoteAddress;
    std::string localAddress;

    // Cumulative values at the previous tick (delta bookkeeping).
    uint64_t prevBytesSent = 0;
    uint64_t prevBytesReceived = 0;
    uint64_t prevPacketsSent = 0;
    uint64_t prevPacketsReceived = 0;
    uint64_t prevPacketsLost = 0;
    uint64_t prevFramesEncoded = 0;
    uint64_t prevFramesDecoded = 0;

    // Derived per-second values published in the snapshot.
    uint32_t packetsSentInSecond = 0;
    uint32_t packetsReceivedInSecond = 0;
    uint32_t packetsLostInSecond = 0;
    uint32_t sendBitrateBps = 0;
    uint32_t receiveBitrateBps = 0;
    float fractionLost = 0.0f;

    uint32_t encodedWidth = 0;
    uint32_t encodedHeight = 0;
    uint32_t decodedWidth = 0;
    uint32_t decodedHeight = 0;
    uint64_t framesEncoded = 0;
    uint64_t framesDecoded = 0;
    uint64_t framesDropped = 0;
    uint64_t firRequests = 0;
    bool firstKeyframeDecoded = false;
    uint32_t framesSentPerSecond = 0;
    uint32_t framesDecodedPerSecond = 0;
};

struct MemberStatsData {
    std::string memberId;
    StreamStats streams[4];
};

uint64_t Delta(uint64_t current, uint64_t previous) {
    // Counters are monotonic, but a stream restart resets them; never publish
    // an underflow-wrapped "rate".
    return current >= previous ? current - previous : 0;
}

void TickStream(StreamStats& s) {
    const uint64_t bytesSent = s.bytesSent.load(std::memory_order_relaxed);
    const uint64_t bytesReceived = s.bytesReceived.load(std::memory_order_relaxed);
    const uint64_t packetsSent = s.packetsSent.load(std::memory_order_relaxed);
    const uint64_t packetsReceived = s.packetsReceived.load(std::memory_order_relaxed);
    const uint64_t packetsLost = s.packetsLost.load(std::memory_order_relaxed);

    const uint64_t deltaBytesSent = Delta(bytesSent, s.prevBytesSent);
    const uint64_t deltaBytesReceived = Delta(bytesReceived, s.prevBytesReceived);
    const uint64_t deltaPacketsSent = Delta(packetsSent, s.prevPacketsSent);
    const uint64_t deltaPacketsReceived = Delta(packetsReceived, s.prevPacketsReceived);
    const uint64_t deltaPacketsLost = Delta(packetsLost, s.prevPacketsLost);

    s.sendBitrateBps = ToBps(deltaBytesSent);
    s.receiveBitrateBps = ToBps(deltaBytesReceived);
    s.packetsSentInSecond = ToU32(deltaPacketsSent);
    s.packetsReceivedInSecond = ToU32(deltaPacketsReceived);
    s.packetsLostInSecond = ToU32(deltaPacketsLost);

    const uint64_t expected = deltaPacketsReceived + deltaPacketsLost;
    s.fractionLost = expected > 0
        ? static_cast<float>(100.0 * static_cast<double>(deltaPacketsLost) /
                             static_cast<double>(expected))
        : 0.0f;

    s.framesSentPerSecond = ToU32(Delta(s.framesEncoded, s.prevFramesEncoded));
    s.framesDecodedPerSecond = ToU32(Delta(s.framesDecoded, s.prevFramesDecoded));

    s.prevBytesSent = bytesSent;
    s.prevBytesReceived = bytesReceived;
    s.prevPacketsSent = packetsSent;
    s.prevPacketsReceived = packetsReceived;
    s.prevPacketsLost = packetsLost;
    s.prevFramesEncoded = s.framesEncoded;
    s.prevFramesDecoded = s.framesDecoded;
}

} // anonymous namespace

// ============================================================
// Impl
// ============================================================
struct MediaStatisticsCollector::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<MemberStatsData>> members;

    std::shared_ptr<MemberStatsData> FindLocked(const std::string& memberId) const {
        auto it = members.find(memberId);
        return it == members.end() ? nullptr : it->second;
    }

    std::shared_ptr<MemberStatsData> Find(const std::string& memberId) const {
        std::lock_guard<std::mutex> lock(mutex);
        return FindLocked(memberId);
    }
};

MediaStatisticsCollector::MediaStatisticsCollector()
    : impl_(std::make_unique<Impl>()) {
}

MediaStatisticsCollector::~MediaStatisticsCollector() = default;

// ============================================================
// Hot-path handle
// ============================================================
struct MediaStatisticsCollector::StreamCounter::Impl {
    // Shared ownership: RemoveMember() drops the collector's reference while a
    // packet dispatched before teardown may still be incrementing here.
    std::shared_ptr<MemberStatsData> data;
    std::size_t kindIndex = 0;
};

MediaStatisticsCollector::StreamCounter::StreamCounter()
    : impl_(std::make_shared<Impl>()) {
}

MediaStatisticsCollector::StreamCounter::~StreamCounter() = default;

void MediaStatisticsCollector::StreamCounter::AddReceived(
    uint64_t bytes, uint64_t packets, uint64_t lost, uint64_t jitterUs) const {

    StreamStats& s = impl_->data->streams[impl_->kindIndex];
    s.bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
    s.packetsReceived.fetch_add(packets, std::memory_order_relaxed);
    s.packetsLost.fetch_add(lost, std::memory_order_relaxed);
    if (jitterUs != 0) {
        s.jitterUs.store(jitterUs, std::memory_order_relaxed);
    }
    s.lastPacketAtMs.store(SteadyNowMs(), std::memory_order_relaxed);
}

void MediaStatisticsCollector::StreamCounter::AddSent(
    uint64_t bytes, uint64_t packets) const {

    StreamStats& s = impl_->data->streams[impl_->kindIndex];
    s.bytesSent.fetch_add(bytes, std::memory_order_relaxed);
    s.packetsSent.fetch_add(packets, std::memory_order_relaxed);
}

std::shared_ptr<MediaStatisticsCollector::StreamCounter>
MediaStatisticsCollector::GetStreamCounter(
    const std::string& memberId, vcs::MediaStreamKind kind) {

    if (memberId.empty()) return nullptr;

    std::shared_ptr<MemberStatsData> data;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto it = impl_->members.find(memberId);
        if (it == impl_->members.end()) {
            // Not registered yet: the caller falls back to the slow per-packet
            // AddReceived/AddSent path instead of silently dropping counters.
            return nullptr;
        }
        data = it->second;
    }

    auto counter = std::make_shared<StreamCounter>();
    counter->impl_->data = std::move(data);
    counter->impl_->kindIndex = KindIndex(kind);
    return counter;
}

// ============================================================
// Session lifetime
// ============================================================
void MediaStatisticsCollector::AddMember(const std::string& memberId) {
    if (memberId.empty()) return;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->members.count(memberId) > 0) return;

    auto data = std::make_shared<MemberStatsData>();
    data->memberId = memberId;
    impl_->members[memberId] = data;

    VCS_MEDIA_LOG_DEBUG("[MediaStatistics] member added: %s", memberId.c_str());
}

void MediaStatisticsCollector::RemoveMember(const std::string& memberId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->members.erase(memberId);
}

void MediaStatisticsCollector::ClearAll() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->members.clear();
}

bool MediaStatisticsCollector::HasMember(const std::string& memberId) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->members.count(memberId) > 0;
}

// ============================================================
// Session shape
// ============================================================
void MediaStatisticsCollector::SetStreamActive(
    const std::string& memberId, vcs::MediaStreamKind kind,
    bool active, uint32_t localSsrc,
    const std::string& remoteAddress, const std::string& localAddress) {

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return;

    StreamStats& s = data->streams[KindIndex(kind)];
    s.active = active;
    s.localSsrc = localSsrc;
    s.remoteAddress = remoteAddress;
    s.localAddress = localAddress;
}

void MediaStatisticsCollector::SetNegotiatedCodec(
    const std::string& memberId, vcs::MediaStreamKind kind,
    const vcs::NegotiatedCodec& codec) {

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return;

    data->streams[KindIndex(kind)].codec = codec;
}

// ============================================================
// RTP hot path
// ============================================================
void MediaStatisticsCollector::AddReceived(
    const std::string& memberId, vcs::MediaStreamKind kind,
    uint64_t bytes, uint64_t packets, uint64_t lost, uint64_t jitterUs) {

    auto data = impl_->Find(memberId);
    if (!data) return;

    StreamStats& s = data->streams[KindIndex(kind)];
    s.bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
    s.packetsReceived.fetch_add(packets, std::memory_order_relaxed);
    s.packetsLost.fetch_add(lost, std::memory_order_relaxed);
    if (jitterUs != 0) {
        s.jitterUs.store(jitterUs, std::memory_order_relaxed);
    }
    s.lastPacketAtMs.store(SteadyNowMs(), std::memory_order_relaxed);
}

void MediaStatisticsCollector::AddSent(
    const std::string& memberId, vcs::MediaStreamKind kind,
    uint64_t bytes, uint64_t packets) {

    auto data = impl_->Find(memberId);
    if (!data) return;

    StreamStats& s = data->streams[KindIndex(kind)];
    s.bytesSent.fetch_add(bytes, std::memory_order_relaxed);
    s.packetsSent.fetch_add(packets, std::memory_order_relaxed);
}

// ============================================================
// Processing counters
// ============================================================
void MediaStatisticsCollector::UpdateVideoProcess(
    const std::string& memberId, vcs::MediaStreamKind kind,
    uint32_t encodedWidth, uint32_t encodedHeight,
    uint32_t decodedWidth, uint32_t decodedHeight,
    uint64_t framesEncoded, uint64_t framesDecoded,
    uint64_t framesDropped, uint64_t firRequests,
    bool firstKeyframeDecoded) {

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return;

    StreamStats& s = data->streams[KindIndex(kind)];
    s.encodedWidth = encodedWidth;
    s.encodedHeight = encodedHeight;
    s.decodedWidth = decodedWidth;
    s.decodedHeight = decodedHeight;
    s.framesEncoded = framesEncoded;
    s.framesDecoded = framesDecoded;
    s.framesDropped = framesDropped;
    s.firRequests = firRequests;
    s.firstKeyframeDecoded = firstKeyframeDecoded;
}

void MediaStatisticsCollector::UpdateAudioProcess(
    const std::string& memberId,
    uint64_t framesEncoded, uint64_t framesDecoded) {

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return;

    StreamStats& s = data->streams[KindIndex(vcs::MediaStreamKind::Audio)];
    s.framesEncoded = framesEncoded;
    s.framesDecoded = framesDecoded;
}

// ============================================================
// 1 Hz tick
// ============================================================
void MediaStatisticsCollector::Tick(const std::string& memberId) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return;

    for (std::size_t i = 0; i < 4; ++i) {
        TickStream(data->streams[i]);
    }
}

// ============================================================
// Query
// ============================================================
bool MediaStatisticsCollector::GetSnapshot(const std::string& memberId,
                                           vcs::MediaInfoSnapshot& out) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto data = impl_->FindLocked(memberId);
    if (!data) return false;

    out.streams.clear();
    out.streams.reserve(4);

    for (std::size_t i = 0; i < 4; ++i) {
        const StreamStats& s = data->streams[i];

        vcs::MediaStreamSnapshot snapshot;
        snapshot.kind = KindFromIndex(i);
        snapshot.active = s.active;
        snapshot.localSsrc = s.localSsrc;
        snapshot.codec = s.codec;

        snapshot.bytesSent = s.bytesSent.load(std::memory_order_relaxed);
        snapshot.bytesReceived = s.bytesReceived.load(std::memory_order_relaxed);
        snapshot.packetsSent = ToU32(s.packetsSent.load(std::memory_order_relaxed));
        snapshot.packetsReceived = ToU32(s.packetsReceived.load(std::memory_order_relaxed));
        snapshot.packetsLost = ToU32(s.packetsLost.load(std::memory_order_relaxed));
        snapshot.jitterUs = ToU32(s.jitterUs.load(std::memory_order_relaxed));
        snapshot.rttMs = ToU32(s.rttMs.load(std::memory_order_relaxed));

        snapshot.packetsSentInSecond = s.packetsSentInSecond;
        snapshot.packetsReceivedInSecond = s.packetsReceivedInSecond;
        snapshot.packetsLostInSecond = s.packetsLostInSecond;
        snapshot.sendBitrateBps = s.sendBitrateBps;
        snapshot.receiveBitrateBps = s.receiveBitrateBps;
        snapshot.fractionLost = s.fractionLost;

        snapshot.encodedWidth = s.encodedWidth;
        snapshot.encodedHeight = s.encodedHeight;
        snapshot.decodedWidth = s.decodedWidth;
        snapshot.decodedHeight = s.decodedHeight;
        snapshot.framesSentPerSecond = s.framesSentPerSecond;
        snapshot.framesDecodedPerSecond = s.framesDecodedPerSecond;
        snapshot.framesEncoded = s.framesEncoded;
        snapshot.framesDecoded = s.framesDecoded;
        snapshot.framesDropped = s.framesDropped;

        snapshot.lastPacketAtMs = s.lastPacketAtMs.load(std::memory_order_relaxed);
        snapshot.firstKeyframeDecoded = s.firstKeyframeDecoded;
        snapshot.firRequests = s.firRequests;

        snapshot.remoteAddress = s.remoteAddress;
        snapshot.localAddress = s.localAddress;

        out.streams.push_back(std::move(snapshot));
    }

    out.sampledAtMs = SteadyNowMs();
    return true;
}

} // namespace media
} // namespace vcs
