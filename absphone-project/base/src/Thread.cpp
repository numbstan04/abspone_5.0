#include "base_export.h"
#include "IThread.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>

namespace vcs { namespace base {

// 简化版本：直接使用 std::thread
class Thread : public IThread {
public:
    explicit Thread(const std::function<void()>& runnable)
        : runnable_(runnable), running_(false) {}

    ~Thread() override {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Start() override {
        if (!running_.load()) {
            running_.store(true);
            thread_ = std::thread([this] {
                if (runnable_) {
                    runnable_();
                }
                running_.store(false);
            });
        }
    }

    void Stop() override {
        // Note: 简单实现，不支持强制停止
        // 实际应用中应在runnable中检查running_标志
    }

    void Join() override {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool IsRunning() const override {
        return running_.load();
    }

    uint64_t GetThreadId() const override {
        if (thread_.joinable()) {
            return std::hash<std::thread::id>{}(thread_.get_id());
        }
        return 0;
    }

private:
    std::function<void()> runnable_;
    std::thread thread_;
    std::atomic<bool> running_;
};

} // namespace base
} // namespace vcs
