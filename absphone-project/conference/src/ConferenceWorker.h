#pragma once

// Per-conference serial worker thread.
//
// Why this exists: EventBus has ONE dispatch thread for the whole process.
// Anything that blocks there - the 7-step media handshake, a gateway call,
// StopMedia()'s join of MediaWorker - delays every other event in the system,
// and a handler that takes a lock the blocked path needs is a deadlock.
//
// So the split is structural, not a matter of discipline:
//   dispatch thread : decide (state transition) + Submit(), never executes
//   this worker     : execute, serially, in submission order
//
// One thread per conference. Serial order is required: StartMedia and
// StopMedia for the same session must never interleave.
//
// Submit() never blocks, but the queue is BOUNDED and rejects explicitly when
// full: a task that hangs forever would otherwise grow the queue without limit
// and take the process down through memory exhaustion long before anyone
// notices. A rejection is never silent - it is counted and logged with the
// task name, so a wedged task is visible in the log instead of being papered
// over by dropping newest work quietly.
//
// Symmetrically, Stop() is BOUNDED: a wedged task must not turn shutdown into
// an eternal join. Stop() reports whether the thread actually exited; the
// owner decides what to do on failure (ConferenceControl abandons the worker
// rather than hanging the process).

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace vcs {
	namespace conference {

		class ConferenceWorker {
		public:
			// stopTimeout bounds Stop(). The default is generous enough for the
			// media handshake plus StopMedia()'s joins on a loaded box, but short
			// enough that a wedged task cannot hang process shutdown forever.
			//
			// maxPending bounds the queue. 256 is far above any normal burst;
			// raising it only delays the moment a wedged task becomes visible,
			// so change it only with a measured reason.
			explicit ConferenceWorker(
				std::string name,
				std::chrono::milliseconds stopTimeout = std::chrono::milliseconds(15000),
				std::size_t maxPending = 256);
			~ConferenceWorker();

			ConferenceWorker(const ConferenceWorker&) = delete;
			ConferenceWorker& operator=(const ConferenceWorker&) = delete;

			// Idempotent. Returns false only if a previous run is still winding
			// down (single-shot, same rule as MediaWorker).
			bool Start();

			// Idempotent. Stops accepting work, then waits up to the configured
			// timeout for the current task to finish and the thread to exit.
			// Returns true when the thread is gone (joined) and false on timeout,
			// in which case the worker is still holding a wedged thread and the
			// owner must abandon the object instead of destroying it.
			// Safe to call from the worker thread itself: it signals only and
			// returns false (the caller cannot wait on its own task).
			bool Stop();

			bool IsRunning() const;

			// Non-blocking. taskName tags the task for diagnostics (which task is
			// running when a stop times out, which one was rejected). Returns
			// false when the worker is stopping, or when the bounded queue is
			// full - callers must treat that as "this fact was not processed".
			bool Submit(const char* taskName, std::function<void()> task);

			size_t PendingCount() const;
			uint64_t RejectedCount() const;

			// Schedule a periodic task. It runs on the worker thread,
			// serialised with submitted tasks and only when the queue is empty.
			// Missed periods are not queued up: at most one invocation is
			// pending, so a slow task can never cause a burst of catch-up runs.
			// An empty task or a non-positive period disables it. Safe to call
			// before or after Start().
			void SetPeriodic(std::function<void()> task,
			                 std::chrono::milliseconds period);

		private:
			struct Task {
				std::string name;
				std::function<void()> fn;
			};

			void RunLoop();

			const std::string name_;
			const std::chrono::milliseconds stopTimeout_;
			const std::size_t maxPending_;

			mutable std::mutex mutex_;
			std::condition_variable cv_;
			std::deque<Task> queue_;
			bool running_ = false;
			bool stopping_ = false;
			// Set at the very end of RunLoop: the thread function is about to
			// return, so joining it cannot block on a wedged task.
			bool loopExited_ = false;

			// Diagnostics for Stop()'s timeout report.
			std::string runningTaskName_;
			std::chrono::steady_clock::time_point runningSince_;
			std::chrono::steady_clock::time_point lastRejectLogAt_;

			uint64_t rejected_ = 0;

			std::function<void()> periodic_;
			std::chrono::milliseconds periodicPeriod_{ 0 };
			std::chrono::steady_clock::time_point nextPeriodic_{};

			std::thread thread_;
		};

	} // namespace conference
} // namespace vcs
