#pragma once

#include "base_export.h"
#include "INetwork.h"
#include "Response.h"
#include <memory>
#include <string>

namespace vcs { namespace base {

// 前向声明
class TcpAcceptor;

/**
 * @brief 网络管理器（跨平台实现）
 *
 * 使用PlatformSocket提供跨平台网络支持。
 * 支持Windows、Linux、macOS。
 *
 * 设计原则：
 * 1. 使用条件编译处理平台差异
 * 2. 提供简洁的同步API
 * 3. 连接管理使用Pimpl模式
 */
class VCS_BASE_API Network : public INetwork {
public:
    Network();
    ~Network() override;

    // INetwork 接口
    Response Listen(uint16_t port) override;
    Response Connect(const std::string& ip, uint16_t port, INetworkCallback* callback) override;
    void Disconnect(const std::string& connId) override;
    Response Send(const std::string& connId, const uint8_t* data, size_t len) override;
    Address GetLocalAddress() const override;

    // 额外方法
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Address localAddr_;
};

} // namespace base
} // namespace vcs
