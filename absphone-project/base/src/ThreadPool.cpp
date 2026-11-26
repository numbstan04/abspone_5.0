#include "ThreadPool.h"

namespace vcs {
	namespace base {

		ThreadPool::ThreadPool(size_t threadCount) : shutdown_(false), activeCount_(0) {
			if (threadCount == 0) threadCount = 4;
			threads_.reserve(threadCount);
			for (size_t i = 0; i < threadCount; ++i) {
				threads_.emplace_back([this] { WorkerThread(); });
			}
		}

		ThreadPool::~ThreadPool() {
			Shutdown();
		}

		void ThreadPool::WorkerThread() {
			while (true) {
				TaskNode taskNode;
				{
					std::unique_lock<std::mutex> lock(mutex_);
					cond_.wait(lock, [this] {
						return shutdown_.load() || !tasks_.empty();
					});

					if (shutdown_.load() && tasks_.empty()) {
						return;
					}

					if (!tasks_.empty()) {
						taskNode = tasks_.top();
						tasks_.pop();
					}
				}

				if (taskNode.task) {
					activeCount_.fetch_add(1);
					taskNode.task();
					activeCount_.fetch_sub(1);
					cond_.notify_all();
				}
			}
		}

		void ThreadPool::Submit(Task task) {
			Submit(std::move(task), 0);
		}

		void ThreadPool::Submit(Task task, int priority) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				tasks_.push({ std::move(task), priority });
			}
			cond_.notify_one();
		}

		void ThreadPool::Shutdown() {
			shutdown_.store(true);
			cond_.notify_all();
			for (auto& thread : threads_) {
				if (thread.joinable()) {
					thread.join();
				}
			}
			threads_.clear();
		}

		void ThreadPool::Wait() {
			std::unique_lock<std::mutex> lock(mutex_);
			cond_.wait(lock, [this] {
				return tasks_.empty() && activeCount_.load() == 0;
			});
		}

		size_t ThreadPool::GetThreadCount() const {
			return threads_.size();
		}

		size_t ThreadPool::GetPendingTaskCount() const {
			std::lock_guard<std::mutex> lock(mutex_);
			return tasks_.size();
		}

		size_t ThreadPool::GetActiveTaskCount() const {
			return activeCount_.load();
		}

	}
} // namespace vcs::base
