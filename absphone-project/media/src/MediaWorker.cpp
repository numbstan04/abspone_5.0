/**
 * @file MediaWorker.cpp
 * @brief Per-conference media worker implementation (Q1 Section 16, Q5 step 5e)
 *
 * [Run loop]
 *   1. Block on cv until running_ is false OR queue is non-empty.
 *   2. Drain a batch with PopBulk.
 *   3. Dispatch each packet to RTPEngine::OnRtpReceivedParsed (sync,
 *      but on the worker thread, not the gateway thread). The packet
 *      is already parsed, so the backend fires the RtpReceiveCallback
 *      without re-serialising / re-parsing the RTP header.
 *   4. If the queue is still empty, go back to step 1.
 *
 * The batch pop keeps us off the per-packet atomic fast path and
 * amortises wakeup latency across many packets.
 *
 * [Q1/Q5 boundary  -  Q5 step 5e landed]
 * The Q1 bridge that reconstructed an RTP datagram from a MediaPacket
 * (BuildRtpDatagram + RtpPacket) was deleted. We now go
 * straight to the parsed-packet entry point on RTPEngine, which in
 * turn forwards to IRtpBackend::OnRtpReceivedParsed. The legacy
 * OnRtpReceived(raw bytes) path is still used by the synchronous
 * MediaControl fallback (no worker), but the async path is fully
 * header-reconstruction-free.
 */

#include "MediaWorker.h"
#include "RTPEngine.h"
#include "ILogger.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iterator>
#include <utility>
#include <vector>

namespace vcs { namespace media {

namespace {

// Dispatch a single MediaPacket to RTPEngine via the Q5 fast path
// (OnRtpReceivedParsed). The previous Q1 bridge  -  BuildRtpDatagram +
// RtpPacket reconstruction  -  was deleted in Q5 step 5e: the
// header bytes are already represented by the MediaPacket's parsed
// fields, so reconstructing them just to have the backend re-parse was
// pure overhead.
void DispatchPacket(RTPEngine* engine, MediaPacket& pkt) {
    if (!engine) {
        return;
    }
    engine->OnRtpReceivedParsed(pkt.memberId,
                                 pkt.payloadType,
                                 pkt.payload.data(), pkt.payload.size(),
                                 pkt.ssrc,
                                 pkt.timestamp,
                                 pkt.sequenceNumber,
		pkt.markerBit);
}

} // anonymous namespace

// ---- MediaWorker --------------------------------------------------------

MediaWorker::MediaWorker(std::string confId,
                         RTPEngine* rtpEngine,
                         const Config& config,
                         SecondTickHandler secondTick)
    : confId_(std::move(confId)),
      rtpEngine_(rtpEngine),
      secondTick_(std::move(secondTick)),
      audio_(config.audio.depth == 0 ? 2 : config.audio.depth),
      video_(config.video.depth == 0 ? 2 : config.video.depth),
      aux_(config.aux.depth == 0 ? 2 : config.aux.depth),
      audioCfg_(config.audio),
      videoCfg_(config.video),
      auxCfg_(config.aux) {
    if (audioCfg_.batch == 0) audioCfg_.batch = 1;
    if (videoCfg_.batch == 0) videoCfg_.batch = 1;
    if (auxCfg_.batch == 0)   auxCfg_.batch = 1;

    VCS_MEDIA_LOG_INFO("[MediaWorker %s] constructed: audio %zu/%zu, video %zu/%zu, "
                       "aux %zu/%zu (capacity/batch)",
                       confId_.c_str(),
                       audio_.queue.Capacity(), audioCfg_.batch,
                       video_.queue.Capacity(), videoCfg_.batch,
                       aux_.queue.Capacity(),   auxCfg_.batch);
}

MediaWorker::Lane& MediaWorker::LaneFor(vcs::MediaStreamKind kind) {
    if (kind == vcs::MediaStreamKind::Audio) return audio_;
    if (kind == vcs::MediaStreamKind::Video) return video_;
    return aux_;
}

const MediaWorker::Lane& MediaWorker::LaneFor(vcs::MediaStreamKind kind) const {
    if (kind == vcs::MediaStreamKind::Audio) return audio_;
    if (kind == vcs::MediaStreamKind::Video) return video_;
    return aux_;
}

bool MediaWorker::AnyPending() const {
    return audio_.queue.SizeApprox() > 0 ||
           video_.queue.SizeApprox() > 0 ||
           aux_.queue.SizeApprox() > 0;
}

uint64_t MediaWorker::DroppedCount() const {
    return audio_.dropped.load() + video_.dropped.load() + aux_.dropped.load();
}

uint64_t MediaWorker::DroppedCount(vcs::MediaStreamKind kind) const {
    return LaneFor(kind).dropped.load();
}

size_t MediaWorker::QueueDepth() const {
    return audio_.queue.SizeApprox() + video_.queue.SizeApprox() + aux_.queue.SizeApprox();
}

size_t MediaWorker::QueueDepth(vcs::MediaStreamKind kind) const {
    return LaneFor(kind).queue.SizeApprox();
}

MediaWorker::~MediaWorker() {
    Stop();
    if (thread_.joinable()) {
        // Reached only when Stop() ran on the worker thread itself: we
        // cannot join ourselves, but ~std::thread on a joinable thread
        // terminates the process, so detach. The object is being destroyed
        // underneath a live loop - report it instead of crashing silently.
        VCS_MEDIA_LOG_ERROR("[MediaWorker %s] destroyed from its own worker "
                            "thread; detaching the still-exiting loop",
                            confId_.c_str());
        thread_.detach();
    }
}

void MediaWorker::Start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;  // already running
    }
    if (thread_.joinable()) {
        // Previous run never got joined (Stop() was called from the
        // worker thread). Assigning a new std::thread over a joinable one
        // calls std::terminate(), so refuse the restart: the worker is
        // single-shot and a stopped conference is torn down, not reused.
        running_.store(false);
        VCS_MEDIA_LOG_ERROR("[MediaWorker %s] Start() while a previous run is "
                            "still winding down; worker is single-shot, refused",
                            confId_.c_str());
        return;
    }
    thread_ = std::thread(&MediaWorker::RunLoop, this);
}

void MediaWorker::Stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;  // already stopped
    }
    wakeCv_.notify_all();

    if (thread_.joinable() && thread_.get_id() == std::this_thread::get_id()) {
        // Stop() from inside a dispatch callback. The loop exits right
        // after the current batch; the caller must not drain (the loop
        // still owns the queue) and must not destroy this object here.
        VCS_MEDIA_LOG_WARNING("[MediaWorker %s] Stop() called from the worker "
                              "thread; signalling only, queue left to the loop",
                              confId_.c_str());
        return;
    }

    // Join BEFORE draining: otherwise the worker thread and the caller
    // both pop from the queue and dispatch into RTPEngine concurrently.
    if (thread_.joinable()) {
        thread_.join();
    }
    DrainRemaining();
}

void MediaWorker::DrainRemaining() {
    // Drain remaining work so no packets are lost on graceful shutdown
    // (Q6 Section 21.10). Only safe once the worker thread is gone, so
    // this is the sole remaining consumer. Loop because a single batch
    // may not cover everything left in the queue; producers can no
    // longer enqueue (running_ is false), so this terminates.
    // Priority order is preserved on shutdown too, so a final audio packet is
    // never overtaken by the last video packets.
    size_t total = 0;
    total += DrainLane(audio_, audioCfg_.batch);
    total += DrainLane(video_, videoCfg_.batch);
    total += DrainLane(aux_,   auxCfg_.batch);

    if (total > 0) {
        VCS_MEDIA_LOG_INFO("[MediaWorker %s] drained %zu leftover packets",
                           confId_.c_str(), total);
    }
}

size_t MediaWorker::DrainLane(Lane& lane, size_t batch) {
    std::vector<MediaPacket> leftovers;
    leftovers.reserve(batch);
    size_t total = 0;
    for (;;) {
        leftovers.clear();
        const size_t n = lane.queue.PopBulk(std::back_inserter(leftovers), batch);
        if (n == 0) {
            break;
        }
        total += n;
        for (size_t i = 0; i < leftovers.size(); ++i) {
            DispatchPacket(rtpEngine_, leftovers[i]);
        }
    }
    return total;
}

bool MediaWorker::Enqueue(vcs::MediaStreamKind kind, MediaPacket pkt) {
    Lane& lane = LaneFor(kind);
    if (!running_.load()) {
        // Worker is not running; drop the packet and count it.
        lane.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!lane.queue.TryPush(std::move(pkt))) {
        lane.dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    wakeCv_.notify_one();
    return true;
}

void MediaWorker::RunLoop() {
    VCS_MEDIA_LOG_INFO("[MediaWorker %s] run loop started", confId_.c_str());

    std::vector<MediaPacket> batch;
    batch.reserve(std::max(audioCfg_.batch, std::max(videoCfg_.batch, auxCfg_.batch)));

    typedef std::chrono::steady_clock Clock;
    const std::chrono::seconds kTickInterval(1);
    Clock::time_point nextTick = Clock::now() + kTickInterval;

    while (running_.load()) {
        // 1 Hz housekeeping runs on this thread (no extra timer thread). It is
        // exception-isolated for the same reason packets are: a throwing
        // stats/observer callback must never take the media thread down.
        if (secondTick_ && Clock::now() >= nextTick) {
            try {
                secondTick_();
            } catch (const std::exception& e) {
                VCS_MEDIA_LOG_ERROR("[MediaWorker %s] second tick threw: %s",
                                    confId_.c_str(), e.what());
            } catch (...) {
                VCS_MEDIA_LOG_ERROR("[MediaWorker %s] second tick threw (unknown)",
                                    confId_.c_str());
            }
            nextTick += kTickInterval;
            const Clock::time_point now = Clock::now();
            if (nextTick <= now) {
                // A stalled tick (suspended process, long handler) must not
                // produce a burst of catch-up ticks.
                nextTick = now + kTickInterval;
            }
        }

        batch.clear();

        // Strict priority: audio, then video, then the secondary stream.
        // Only one lane is drained per iteration so a continuous audio lane
        // still yields the thread between batches, and a video IDR burst can
        // never delay an audio packet.
        size_t n = audio_.queue.PopBulk(std::back_inserter(batch), audioCfg_.batch);
        if (n == 0) {
            n = video_.queue.PopBulk(std::back_inserter(batch), videoCfg_.batch);
        }
        if (n == 0) {
            n = aux_.queue.PopBulk(std::back_inserter(batch), auxCfg_.batch);
        }
        if (n == 0) {
            // Sleep until enqueue or stop. wait_for gives us a
            // bounded upper bound on shutdown latency even if no
            // packets arrive.
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wakeCv_.wait_for(lock, std::chrono::milliseconds(50),
                             [this] { return !running_.load() || AnyPending(); });
            continue;
        }

        for (size_t i = 0; i < batch.size(); ++i) {
            DispatchPacket(rtpEngine_, batch[i]);
        }
    }

    VCS_MEDIA_LOG_INFO("[MediaWorker %s] run loop exiting", confId_.c_str());
}

} // namespace media
} // namespace vcs
