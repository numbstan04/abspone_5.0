#pragma once

#include "base_export.h"
#include "Response.h"
#include <string>
#include <functional>
#include <cstdint>

namespace vcs { namespace base {

// 网络地址
struct Address {
    std::string ip;
    uint16_t port;
};

// 连接回调接口
class INetworkCallback {
public:
    virtual ~INetworkCallback() = default;
    virtual void OnConnected(const std::string& connId) = 0;
    virtual void OnDisconnected(const std::string& connId) = 0;
    virtual void OnDataReceived(const std::string& connId, const uint8_t* data, size_t len) = 0;
    virtual void OnError(const std::string& connId, int errorCode, const std::string& errorMsg) = 0;
};

		class VCS_BASE_API INetwork {
		public:
			virtual ~INetwork() = default;

    // 监听端口
    virtual Response Listen(uint16_t port) = 0;

    // 连接远程地址
    virtual Response Connect(const std::string& ip, uint16_t port, INetworkCallback* callback) = 0;

    // 断开连接
    virtual void Disconnect(const std::string& connId) = 0;

    // 发送数据
    virtual Response Send(const std::string& connId, const uint8_t* data, size_t len) = 0;

    // 获取本地地址
    virtual Address GetLocalAddress() const = 0;
};

} // namespace base
} // namespace vcs
