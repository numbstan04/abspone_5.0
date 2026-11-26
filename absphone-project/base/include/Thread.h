#pragma once

#include "base_export.h"
#include "IThread.h"
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace vcs { namespace base {

// 线程实现类
class ThreadImpl : public IThread {
public:
    ThreadImpl();
    explicit ThreadImpl(const std::function<void()>& runnable);
    ~ThreadImpl() override;

    void Start() override;
    void Stop() override;
    void Join() override;
    bool IsRunning() const override;
    uint64_t GetThreadId() const override;

    void SetRunnable(const std::function<void()>& runnable);

private:
    void Run();

private:
    std::function<void()> runnable_;
    std::thread thread_;
    std::atomic<bool> running_;
    std::mutex mutex_;
    std::condition_variable cond_;
};

// 简化版本：直接使用 std::thread
class Thread : public IThread {
public:
    explicit Thread(const std::function<void()>& runnable);
    ~Thread() override;

    void Start() override;
    void Stop() override;
    void Join() override;
    bool IsRunning() const override;
    uint64_t GetThreadId() const override;

private:
    std::function<void()> runnable_;
    std::thread thread_;
    std::atomic<bool> running_;
};

} // namespace base
} // namespace vcs
