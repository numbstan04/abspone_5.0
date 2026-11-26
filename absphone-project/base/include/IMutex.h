#pragma once

#include "base_export.h"

namespace vcs { namespace base {

		class VCS_BASE_API IMutex {
		public:
			virtual ~IMutex() = default;
			virtual void Lock() = 0;
			virtual void Unlock() = 0;
			virtual bool TryLock() = 0;
		};

// 互斥锁RAII封装
class MutexLock {
public:
    explicit MutexLock(IMutex& mutex) : mutex_(mutex) {
        mutex_.Lock();
    }
    ~MutexLock() {
        mutex_.Unlock();
    }

    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;

private:
    IMutex& mutex_;
};

} // namespace base
} // namespace vcs
