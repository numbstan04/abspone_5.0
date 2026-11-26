#include "base_export.h"
#include "ICondition.h"
#include <condition_variable>
#include <chrono>
#include <mutex>

namespace vcs { namespace base {

// 条件变量实现
class Condition : public ICondition {
public:
    void Wait() override {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock);
    }

    bool WaitFor(int milliseconds) override {
        std::unique_lock<std::mutex> lock(mutex_);
        auto duration = std::chrono::milliseconds(milliseconds);
        return cond_.wait_for(lock, duration) == std::cv_status::no_timeout;
    }

    void NotifyOne() override {
        cond_.notify_one();
    }

    void NotifyAll() override {
        cond_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
};

} // namespace base
} // namespace vcs
