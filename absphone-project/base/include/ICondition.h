#pragma once

#include "base_export.h"

namespace vcs { namespace base {

		// Condition variable interface
		class VCS_BASE_API ICondition {
		public:
			virtual ~ICondition() = default;

    // 等待
    virtual void Wait() = 0;

    // 等待（带超时）
    virtual bool WaitFor(int milliseconds) = 0;

    // 通知一个线程
    virtual void NotifyOne() = 0;

    // 通知所有线程
    virtual void NotifyAll() = 0;
};

} // namespace base
} // namespace vcs
