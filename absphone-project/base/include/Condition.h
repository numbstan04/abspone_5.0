#pragma once

/**
 * @file Condition.h
 * @brief 跨平台条件变量封装
 *
 * 使用C++11标准库实现，支持Windows、Linux、macOS
 * C++11/14/17/20全兼容
 * 如需更高级特性，可切换到Boost版本
 */

#include "base_export.h"
#include "ICondition.h"
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace vcs { namespace base {

// ============================================================
// 条件变量实现 - 基于 std::condition_variable（C++11原生）
// ============================================================
class Condition : public ICondition {
public:
    void Wait() override {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock);
    }

    bool WaitFor(int milliseconds) override {
        std::unique_lock<std::mutex> lock(mutex_);
        return cond_.wait_for(lock, std::chrono::milliseconds(milliseconds)) == std::cv_status::no_timeout;
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
