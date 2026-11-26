#include "ConferenceWorker.h"

#include "ILogger.h"

#include <utility>

namespace vcs {
	namespace conference {

		namespace {
			long long ElapsedMs(std::chrono::steady_clock::time_point since,
			                    std::chrono::steady_clock::time_point now) {
				return static_cast<long long>(
					std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count());
			}
		} // namespace

		ConferenceWorker::ConferenceWorker(std::string name,
		                                       std::chrono::milliseconds stopTimeout,
		                                       std::size_t maxPending)
			: name_(std::move(name)),
			stopTimeout_(stopTimeout),
			maxPending_(maxPending == 0 ? 1 : maxPending) {
		}

		ConferenceWorker::~ConferenceWorker() {
			// Stop() is bounded, but the destructor cannot leak: a joinable
			// std::thread would call std::terminate. An owner that hit Stop()'s
			// timeout must abandon the object (see ConferenceControl::Shutdown)
			// instead of destroying it; if it did not, we fall back to an
			// unconditional join, which is the only remaining safe choice.
			if (!Stop() && thread_.joinable()) {
				VCS_CONFERENCE_LOG_ERROR(
					"[ConferenceWorker %s] destructor joining a wedged thread; "
					"the owner should have abandoned this worker", name_.c_str());
				thread_.join();
			}
		}

		bool ConferenceWorker::Start() {
			std::lock_guard<std::mutex> lock(mutex_);
			if (running_) {
				return true;
			}
			if (thread_.joinable()) {
				// A previous Stop() was called from the worker thread itself;
				// the thread is still winding down. Single-shot: no restart.
				VCS_CONFERENCE_LOG_ERROR(
					"[ConferenceWorker %s] Start() while a previous run is "
					"winding down; refused", name_.c_str());
				return false;
			}
			stopping_ = false;
			loopExited_ = false;
			running_ = true;
			if (periodicPeriod_.count() > 0) {
				nextPeriodic_ = std::chrono::steady_clock::now() + periodicPeriod_;
			}
			thread_ = std::thread(&ConferenceWorker::RunLoop, this);
			return true;
		}

		bool ConferenceWorker::Stop() {
			std::unique_lock<std::mutex> lock(mutex_);
			if (!running_ && !thread_.joinable()) {
				return true;
			}
			running_ = false;
			stopping_ = true;
			cv_.notify_all();

			if (thread_.joinable() &&
				thread_.get_id() == std::this_thread::get_id()) {
				// Stop() from a task: the loop exits after the current task,
				// the object must outlive this call. The owner is expected to
				// join from another thread. We cannot wait on ourselves.
				VCS_CONFERENCE_LOG_WARNING(
					"[ConferenceWorker %s] Stop() called from its own thread; "
					"signalling only", name_.c_str());
				return false;
			}

			if (!cv_.wait_for(lock, stopTimeout_, [this] { return loopExited_; })) {
				const auto now = std::chrono::steady_clock::now();
				VCS_CONFERENCE_LOG_ERROR(
					"[ConferenceWorker %s] stop timed out after %lld ms: running "
					"task=\"%s\" for %lld ms, pending=%zu, rejected=%llu. The worker "
					"thread is wedged; the owner must abandon this worker.",
					name_.c_str(), static_cast<long long>(stopTimeout_.count()),
					runningTaskName_.empty() ? "<none>" : runningTaskName_.c_str(),
					runningTaskName_.empty() ? 0 : ElapsedMs(runningSince_, now),
					queue_.size(), static_cast<unsigned long long>(rejected_));
				return false;
			}

			// The loop has returned; joining cannot block on a task any more.
			lock.unlock();
			if (thread_.joinable()) {
				thread_.join();
			}
			return true;
		}

		bool ConferenceWorker::IsRunning() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return running_;
		}

		bool ConferenceWorker::Submit(const char* taskName, std::function<void()> task) {
			if (!task) {
				return false;
			}
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (!running_ || stopping_) {
					VCS_CONFERENCE_LOG_DEBUG(
						"[ConferenceWorker %s] task \"%s\" rejected: worker stopping",
						name_.c_str(), taskName ? taskName : "?");
					return false;
				}
				if (queue_.size() >= maxPending_) {
					++rejected_;
					// Bounded queue, explicit rejection. Throttled to one line
					// per second so a burst of events cannot flood the log; the
					// total count keeps the volume visible.
					const auto now = std::chrono::steady_clock::now();
					if (rejected_ == 1 ||
						now - lastRejectLogAt_ >= std::chrono::seconds(1)) {
						lastRejectLogAt_ = now;
						VCS_CONFERENCE_LOG_ERROR(
							"[ConferenceWorker %s] queue full (%zu), task \"%s\" "
							"rejected (total rejected=%llu). Running task \"%s\" is stuck: "
							"the event was NOT processed.",
							name_.c_str(), queue_.size(), taskName ? taskName : "?",
							static_cast<unsigned long long>(rejected_),
							runningTaskName_.empty() ? "<none>" : runningTaskName_.c_str());
					}
					return false;
				}
				Task entry;
				entry.name = taskName ? taskName : "?";
				entry.fn = std::move(task);
				queue_.push_back(std::move(entry));
				if (queue_.size() == maxPending_ / 2) {
					VCS_CONFERENCE_LOG_WARNING(
						"[ConferenceWorker %s] %zu tasks pending - a task is "
						"likely blocked", name_.c_str(), queue_.size());
				}
			}
			cv_.notify_one();
			return true;
		}

		size_t ConferenceWorker::PendingCount() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return queue_.size();
		}

		uint64_t ConferenceWorker::RejectedCount() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return rejected_;
		}

		void ConferenceWorker::SetPeriodic(std::function<void()> task,
		                                     std::chrono::milliseconds period) {
			std::lock_guard<std::mutex> lock(mutex_);
			periodic_ = std::move(task);
			periodicPeriod_ = period.count() > 0 ? period : std::chrono::milliseconds(0);
			if (periodicPeriod_.count() > 0) {
				nextPeriodic_ = std::chrono::steady_clock::now() + periodicPeriod_;
			}
			cv_.notify_all();
		}

		void ConferenceWorker::RunLoop() {
			std::unique_lock<std::mutex> lock(mutex_);
			for (;;) {
				if (stopping_) {
					break;
				}

				// Submitted work has priority: facts and triggers are never
				// delayed behind housekeeping.
				if (queue_.empty()) {
					const bool hasPeriodic =
						periodic_ && periodicPeriod_.count() > 0;
					if (hasPeriodic) {
						const auto now = std::chrono::steady_clock::now();
						if (now >= nextPeriodic_) {
							Task tick;
							tick.name = "periodic";
							tick.fn = periodic_;
							nextPeriodic_ = now + periodicPeriod_;
							runningTaskName_ = tick.name;
							runningSince_ = now;
							lock.unlock();
							try {
								tick.fn();
							} catch (...) {
								VCS_CONFERENCE_LOG_ERROR(
									"[ConferenceWorker %s] periodic task threw; "
									"continuing", name_.c_str());
							}
							tick.fn = nullptr;
							lock.lock();
							runningTaskName_.clear();
							continue;
						}
						cv_.wait_until(lock, nextPeriodic_, [this] {
							return stopping_ || !queue_.empty();
						});
						continue;
					}
					cv_.wait(lock, [this] {
						return stopping_ || !queue_.empty();
					});
					continue;
				}

				Task task = std::move(queue_.front());
				queue_.pop_front();
				runningTaskName_ = task.name;
				runningSince_ = std::chrono::steady_clock::now();
				lock.unlock();

				// Executed outside the lock: a task may Submit() more work.
				try {
					task.fn();
				} catch (...) {
					VCS_CONFERENCE_LOG_ERROR(
						"[ConferenceWorker %s] task \"%s\" threw; continuing",
						name_.c_str(), task.name.c_str());
				}
				task.fn = nullptr;
				lock.lock();
				runningTaskName_.clear();
			}

			// Stop() was requested: discard what was never started. A task must
			// not run after the owner announced shutdown.
			std::deque<Task> dropped;
			dropped.swap(queue_);
			runningTaskName_.clear();
			loopExited_ = true;
			lock.unlock();
			cv_.notify_all();
			if (!dropped.empty()) {
				VCS_CONFERENCE_LOG_INFO(
					"[ConferenceWorker %s] discarded %zu pending tasks on shutdown",
					name_.c_str(), dropped.size());
			}
		}

	} // namespace conference
} // namespace vcs
