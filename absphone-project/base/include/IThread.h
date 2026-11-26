#pragma once

#include "base_export.h"
#include <cstdint>
#include <string>

namespace vcs { namespace base {

		// Thread interface
		class VCS_BASE_API IThread {
		public:
			virtual ~IThread() = default;

    // 启动线程
    virtual void Start() = 0;

    // 停止线程
    virtual void Stop() = 0;

    // 等待线程结束
    virtual void Join() = 0;

    // 检查是否运行
    virtual bool IsRunning() const = 0;

    // 获取线程ID
    virtual uint64_t GetThreadId() const = 0;
};

} // namespace base
} // namespace vcs
