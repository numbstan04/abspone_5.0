/**
 * @file MediaWorker.h
 * @brief Per-conference media worker (Q1 Section 16)
 *
 * [Design principles]
 * 1. Exactly one MediaWorker exists per conference; it owns ONE CONSUMER
 *    THREAD and one SPSC input queue PER STREAM KIND.
 *
 
 *    Why per-kind queues are mandatory, not an optimisation:
 *    every MediaStreamKind is backed by exactly one transport
 *    (IRtpTransport contract), and every transport has its own receive
 *    thread. A single shared queue would therefore have two or three
 *    producers concurrently, which violates the SPSC contract in
 *    SpscQueue.h and is undefined behaviour. Splitting by kind restores
 *    "one producer thread per queue" by construction.
 *
 * 2. Audio outranks video, video outranks the secondary stream. The
 *    consumer drains strictly in that order, so a video IDR burst can
 *    never push an audio packet past its 20 ms latency budget. The
 *    previous single-FIFO design coupled them.
 *
 * 3. Queue depth is a LATENCY parameter, not a memory parameter: it is
 *    sized from the per-kind latency budget, and each lane counts its own
 *    drops so "which stream is starving" is answerable.
 *
 * 4. The worker is the consumer end of the SPSC pipeline; the producer
 *    end is MediaTransportCallback (gateway-side). This is the only
 *    place where the gateway thread hands work to the media layer.
 *
 * 5. Pops are batched (PopBulk) to amortise per-packet atomic traffic;
 *    batches are dispatched one-by-one to the per-conference RTPEngine.
 *
 * 6. The worker thread can be stopped cooperatively (Stop()) or by
 *    destruction. Stop() joins before draining, so no packets are
 *    silently dropped on graceful shutdown.
 *
 * [References]
 * - Section 16.2 (Q1 core processing loop)
 * - Section 21.10 (Q6 graceful shutdown: drain SPSC, then stop)
 */

#pragma once

#include "SpscQueue.h"
#include "MediaPacket.h"
#include "MediaStreamKind.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vcs { namespace media {

class RTPEngine;

/**
 * @brief Per-conference worker thread that drains the SPSC queue and
 *        hands packets off to the conference's RTPEngine.
 *
 * The handler is the only callback needed; the worker is otherwise
 * self-contained.
 */
class MediaWorker {
public:
    using PacketHandler = std::function<void(MediaPacket&&)>;

    // Runs on the worker thread once per second. It is the ONLY clock in the
    // media layer: per-second deltas (bitrate, packet rate, fps) and the
    // observer stats push are derived here, so no extra timer thread exists.
    using SecondTickHandler = std::function<void()>;

    // Per-kind capacities derived from a latency budget, not from memory.
    //   audio: 50 pkt/s, 20 ms packets  -> 16 slots is already 320 ms worst case
    //   video: ~200-400 pkt/s + 100-packet IDR bursts -> 256 slots
    //   aux  : content sharing tolerates more delay than the main video
    struct LaneConfig {
        size_t depth = 128;
        size_t batch = 16;
    };

    struct Config {
        LaneConfig audio;
        LaneConfig video;
        LaneConfig aux;

        Config() {
            audio.depth = 16;  audio.batch = 8;
            video.depth = 256; video.batch = 32;
            aux.depth   = 128; aux.batch   = 16;
        }
    };

    /**
     * @param confId       conference id (for logging)
     * @param rtpEngine    destination; must outlive the worker
     * @param config       per-lane depth/batch; defaults match the budgets above
     * @param secondTick   optional 1 Hz handler; runs on the worker thread and
     *                     must not block (it shares the lane dispatch thread)
     */
    MediaWorker(std::string confId,
                RTPEngine* rtpEngine,
                const Config& config = Config(),
                SecondTickHandler secondTick = SecondTickHandler());
    ~MediaWorker();

    MediaWorker(const MediaWorker&) = delete;
    MediaWorker& operator=(const MediaWorker&) = delete;

    /// Start the worker thread. Idempotent; single-shot per object - a
    /// stopped worker is never restarted (a new conference creates a new
    /// worker instead). Start() and Stop() must be called from the owning
    /// (conference) thread; Enqueue() may be called from any producer.
    void Start();

    /// Cooperative shutdown: stop accepting work, join the worker, then
    /// drain whatever is left. Join happens first so the leftover drain
    /// cannot run concurrently with the worker's own dispatch.
    ///
    /// May be called from the worker thread itself (teardown triggered by
    /// a dispatch callback); in that case it only signals and returns -
    /// joining ourselves is impossible and the queue must not be drained
    /// while the loop still owns it. Destroying the worker from its own
    /// thread is a programming error and is reported as such.
    void Stop();

    /// True if the worker thread is running.
    bool IsRunning() const { return running_.load(); }

    /// Push a packet from the gateway (producer) side. The stream kind
    /// selects the lane, so each lane still has exactly one producer.
    /// Returns false when that lane is full.
    bool Enqueue(vcs::MediaStreamKind kind, MediaPacket pkt);

    /// Total packets dropped across all lanes.
    uint64_t DroppedCount() const;
    /// Drops on one lane: "which stream is starving" must be answerable.
    uint64_t DroppedCount(vcs::MediaStreamKind kind) const;

    /// Approximate queue depth (for metrics / health).
    size_t QueueDepth() const;
    size_t QueueDepth(vcs::MediaStreamKind kind) const;

private:
    struct Lane {
        explicit Lane(size_t depth) : queue(depth) {}
        SpscQueue<MediaPacket> queue;
        std::atomic<uint64_t>  dropped{0};
    };

    void RunLoop();
    void DrainRemaining();
    size_t DrainLane(Lane& lane, size_t batch);
    bool AnyPending() const;

    Lane& LaneFor(vcs::MediaStreamKind kind);
    const Lane& LaneFor(vcs::MediaStreamKind kind) const;

    const std::string confId_;
    RTPEngine* const  rtpEngine_;        // not owned
    const SecondTickHandler secondTick_;

    Lane audio_;
    Lane video_;
    Lane aux_;
    LaneConfig audioCfg_;
    LaneConfig videoCfg_;
    LaneConfig auxCfg_;

    std::atomic<bool>      running_{false};

    // Owned worker thread. Joined by Stop() before the leftover drain so
    // the queue is only ever consumed by one thread at a time.
    std::thread            thread_;

    // RunLoop sync: we use a condition variable so that Stop() can
    // wake an idle worker immediately instead of waiting for the next
    // timed wait. The cv is only ever signalled from Enqueue() and
    // Stop(); the worker never holds the mutex while doing real work.
    std::mutex              wakeMutex_;
    std::condition_variable wakeCv_;
};

} // namespace media
} // namespace vcs
