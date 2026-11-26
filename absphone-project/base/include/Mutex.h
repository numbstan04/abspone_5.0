#pragma once

#include "base_export.h"
#include "IMutex.h"
#include <mutex>

namespace vcs { namespace base {

// 互斥锁实现 - 基于 std::mutex
class Mutex : public IMutex {
public:
    void Lock() override {
        mutex_.lock();
    }

    void Unlock() override {
        mutex_.unlock();
    }

    bool TryLock() override {
        return mutex_.try_lock();
    }

private:
    std::mutex mutex_;
};

} // namespace base
} // namespace vcs
