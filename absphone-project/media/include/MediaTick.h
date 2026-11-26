/**
 * @file MediaTick.h
 * @brief Per-conference periodic tick driver (Q1 Section 16.6)
 *
 * [Design principles]
 * 1. Replaces the legacy "central MediaScheduler" pattern (V5 design, Section 9).
 * 2. Exactly one MediaTick exists per conference. It owns a single worker
 *    thread and drives audio mixing on its fixed cadence (20 ms by default).
 * 3. The tick thread drives audio mixing (TriggerMix); video composition is
 *    self-driven by VideoProcessor::WorkerLoop (33 ms). The audio and video
 *    processors themselves are stateless from the tick's perspective.
 * 4. Output distribution happens via callbacks (audio: to gateway via
 *    AudioOutputCallback; video: to RTPEngine for RTP packaging). MediaTick
 *    does NOT call transport directly.
 *
 * [Differences vs. legacy MediaScheduler]
 * - Single thread (was: 2 threads). Reduces per-conference thread count
 *   from 2 to 1, matching the design's "1 worker per conference" rule.
 * - Per-conference ownership (was: globally owned by MediaControl v1).
 *   Each ConferenceMediaResource owns exactly one MediaTick.
 * - Simpler API: SetProcessors + Start/Stop + Set*OutputCallback. The
 *   legacy "Statistics" / "ProcessRtpTasks" interfaces are removed because
 *   the MediaWorker (Q1 16.5) now owns the SPSC pipeline and the
 *   per-conference eventBus carries observability events.
 *
 * [References]
 * - Section 16.6 (Q1 per-conference tick driver)
 * - Section 9 (V5 decision: remove MediaScheduler, replaced by per-conference MediaTick)
 */

#pragma once

#include "media_export.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace vcs { namespace media {

class AudioProcessor;
class VideoProcessor;
class RTPEngine;

using AudioProcessorPtr = std::shared_ptr<AudioProcessor>;
using VideoProcessorPtr = std::shared_ptr<VideoProcessor>;
using RTPEnginePtr      = std::shared_ptr<RTPEngine>;

/**
 * @brief Per-conference periodic tick driver.
 *
 * Owns one thread that, while running, alternates between:
 *   - 20 ms audio tick: calls AudioProcessor::TriggerMix then GetMixOutput
 *     and dispatches to all members via AudioOutputCallback (echo-suppressed).
 *   - 33 ms video tick: video is self-driven by VideoProcessor::WorkerLoop;
 *     the tick only forwards encoded frames to RTPEngine for RTP packaging.
 */
class VCS_MEDIA_API MediaTick {
public:
    struct Config {
        int  audioFrameIntervalMs = 20;   // 20 ms = 50 fps audio
        int  videoFrameIntervalMs = 33;   // ~30 fps video
        bool enableAudio          = true;
        bool enableVideo          = true;
    };

    using AudioOutputCallback = std::function<void(
        const std::string& memberId,
        uint32_t           ssrc,
        const uint8_t*     data,
        size_t             length,
        uint64_t           timestamp,
        uint16_t           sequenceNumber)>;

    using VideoOutputCallback = std::function<void(
        const std::string& memberId,
        uint32_t           ssrc,
        const uint8_t*     data,
        size_t             length,
        uint64_t           timestamp,
        uint16_t           sequenceNumber,
        bool               isKeyFrame)>;

    explicit MediaTick(const Config& config);
    ~MediaTick();

    MediaTick(const MediaTick&)            = delete;
    MediaTick& operator=(const MediaTick&) = delete;

    /// Wire the per-conference processors. Must be called before Start().
    void SetProcessors(AudioProcessorPtr audio,
                       VideoProcessorPtr video,
                       RTPEnginePtr      rtp);

    /// Output callbacks. Either may be null.
    void SetAudioOutputCallback(AudioOutputCallback cb);
    void SetVideoOutputCallback(VideoOutputCallback cb);

    /// Start the tick thread. Idempotent; returns false if config disables
    /// both audio and video ticks.
    bool Start();

    /// Stop the tick thread and join. Idempotent.
    void Stop();

    bool IsRunning() const { return running_.load(); }

private:
    void RunLoop();
    void ProcessAudioMix();

    const Config config_;

    AudioProcessorPtr audioProcessor_;
    VideoProcessorPtr videoProcessor_;
    RTPEnginePtr      rtpEngine_;

    AudioOutputCallback audioCallback_;
    VideoOutputCallback videoCallback_;

    std::atomic<bool>               running_{false};
    std::unique_ptr<std::thread>    thread_;

    // Encoded output buffers and sequence numbers
    std::vector<uint8_t> audioEncodedBuffer_;
    std::vector<uint8_t> videoEncodedBuffer_;
    uint16_t audioSeq_ = 0;
    uint16_t videoSeq_ = 0;
};

} // namespace media
} // namespace vcs
