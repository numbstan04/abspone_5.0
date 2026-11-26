#pragma once

#include "base_export.h"
#include "IThreadPool.h"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <vector>

namespace vcs {
	namespace base {

		struct TaskNode {
			Task task;
			int priority;
			bool operator<(const TaskNode& other) const {
				return priority < other.priority;
			}
		};

		class VCS_BASE_API ThreadPool : public IThreadPool {
		public:
			explicit ThreadPool(size_t threadCount = 4);
			~ThreadPool() override;

			void Submit(Task task) override;
			void Submit(Task task, int priority) override;
			void Shutdown() override;
			void Wait() override;
			size_t GetThreadCount() const override;
			size_t GetPendingTaskCount() const override;
			size_t GetActiveTaskCount() const override;

		private:
			void WorkerThread();

		private:
			std::vector<std::thread> threads_;
			std::priority_queue<TaskNode> tasks_;
			mutable std::mutex mutex_;
			std::condition_variable cond_;
			std::atomic<bool> shutdown_;
			std::atomic<size_t> activeCount_;
		};

	}
} // namespace vcs::base
