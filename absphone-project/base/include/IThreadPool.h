#pragma once

#include "base_export.h"
#include <functional>
#include <memory>

namespace vcs { namespace base {

// 线程任务类型
using Task = std::function<void()>;

		// 线程池接口
		class VCS_BASE_API IThreadPool {
		public:
			virtual ~IThreadPool() = default;

    // 提交任务
    virtual void Submit(Task task) = 0;

    // 提交任务（带优先级）
    virtual void Submit(Task task, int priority) = 0;

    // 关闭线程池
    virtual void Shutdown() = 0;

    // 等待所有任务完成
    virtual void Wait() = 0;

    // 获取线程数
    virtual size_t GetThreadCount() const = 0;

    // 获取等待任务数
    virtual size_t GetPendingTaskCount() const = 0;

    // 获取活跃任务数
    virtual size_t GetActiveTaskCount() const = 0;
};

} // namespace base
} // namespace vcs
