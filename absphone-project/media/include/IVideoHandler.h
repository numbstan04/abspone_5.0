#pragma once

/**
 * @file IVideoHandler.h
 * @brief Video Handler interface (Q3 Section 18.3)
 *
 * [Q3 Section 18.3 Decision 1]: 8 handlers per chain, originally 1:1 mapped to
 * the legacy `TriggerCompose()` steps. The legacy god function has been
 * removed; the chain is now driven by VideoProcessor::WorkerLoop /
 * ProcessOnce.
 *   1. Input       - pull a frame from the per-member queue
 *   2. Decode      - H.264/H.265 -> YUV
 *   3. Scale       - resize to target resolution (multi-resolution when used)
 *   4. Layout      - compute per-frame layout (1/2/4/9 cells)
 *   5. Compose     - blit decoded frames onto the canvas
 *   6. Watermark   - OSD / subtitle overlay
 *   7. Encode      - YUV -> H.264/H.265
 *   8. Egress      - hand the encoded packet to RtpEngine
 *
 * [Q3 Section 18.3 Decision 2]: `ProcessContext` is the data bus; it carries DATA
 * ONLY, no control flow. Each handler reads upstream fields and writes
 * downstream fields. (See media/src/handler/ProcessContext.h.)
 *
 * [Q3 Section 18.3 Decision 3]: Runtime enable/disable is supported via
 * `IsEnabled()` / `SetEnabled()`. The video processor skips a disabled
 * handler but keeps the chain alive.
 *
 * [Q3 Section 18.5 vs. Q1 Section 16]: Decode/Encode internally submit their CPU-heavy
 * work to CMediaWorkerPool (per Q1). The Handler interface itself is
 * synchronous: `Process()` runs on the chain driver thread (the
 * VideoProcessor::WorkerLoop thread) and dispatches async work internally.
 *
 * [Q3 Section 18.7 Out of scope]: layout algorithm, worker pool sizing, multi-
 * resolution ScaleHandler composition, and chain persistence are not
 * specified here.
 */

#include "media_export.h"
#include "ErrorCode.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace vcs { namespace media {

// Forward decl; full definition in media/src/handler/ProcessContext.h
struct ProcessContext;

/**
 * @brief Video Handler interface (Q3 Section 18.3).
 *
 * The contract is intentionally minimal: `Process()` does one thing and
 * returns a status. No two-arg Handle(), no priority field (priority is
 * set at construction), no CanProcess() (each handler decides internally).
 */
class VCS_MEDIA_API IVideoHandler {
public:
    virtual ~IVideoHandler() = default;

    // ----- Identity -------------------------------------------------------

    /**
     * @brief Stable name used for lookup, logging, and configuration
     *        (e.g. "Decode", "Layout", "Egress"). Must be unique per chain.
     */
    virtual const char* Name() const = 0;

    /**
     * @brief Chain priority. Lower runs first. The 8 canonical handlers
     *        use 10/20/30/40/50/60/70/80 (Input/Egress) respectively.
     *        Subclasses that introduce new steps must pick values that
     *        preserve the documented ordering.
     */
    virtual int Priority() const = 0;

    // ----- Runtime enable/disable (Q3 Section 18.3 Decision 3) -------------------

    /**
     * @brief Whether the handler will run on the next Process() call.
     *        Default: true (for required handlers) or false (for opt-in
     *        handlers such as Watermark / Scale).
     */
    virtual bool IsEnabled() const = 0;

    /**
     * @brief Toggle the handler without removing it from the chain.
     *        Thread-safe (atomic).
     */
    virtual void SetEnabled(bool enabled) = 0;

    // ----- The actual step -----------------------------------------------

    /**
     * @brief Run this handler's step on the current frame.
     * @param ctx Data bus. Read upstream fields, write downstream fields.
     * @return MediaError::Success to continue the chain.
     *         Any other value aborts the chain (the driver logs and stops).
     *
     * Implementations must:
     *   1. Be exception-safe (catch all, never let an exception escape).
     *   2. Be reentrant w.r.t. the same handler instance (the chain driver
     *      is single-threaded today, but a future Q-iteration may invoke
     *      a handler from a worker thread for async I/O).
     *   3. Be quick to no-op when nothing useful can be done (return
     *      Success and leave ctx unchanged).
     */
    virtual vcs::ErrorCode Process(ProcessContext& ctx) = 0;

    // ----- Optional: lifecycle / diagnostics ------------------------------

    /**
     * @brief Called once when the handler is attached to a chain.
     *        Default: no-op. Override to start background threads,
     *        allocate per-chain buffers, etc.
     */
    virtual vcs::ErrorCode OnAttach() { return vcs::ErrorCode::Success; }

    /**
     * @brief Called once when the handler is detached (chain teardown or
     *        per-conference Teardown). Default: no-op.
     */
    virtual void OnDetach() {}

    /**
     * @brief Free-form statistics string for log/metrics.
     */
    virtual std::string GetStatistics() const { return std::string(); }
};

} // namespace media
} // namespace vcs
