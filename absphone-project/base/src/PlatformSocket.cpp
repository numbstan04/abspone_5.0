/**
 * @file PlatformSocket.cpp
 * @brief Cross-platform Socket implementation using Boost.Asio
 *
 * True cross-platform implementation - no #ifdef conditional compilation
 * Works on Windows, Linux, and macOS with the same code
 */

#include "PlatformSocket.h"
#include "ILogger.h"
#include <cstring>
#include <memory>

namespace vcs {
	namespace base {

		// ============================================================
		// Network initialization
		// ============================================================
		namespace detail {
			void initNetwork() {
				// Boost.Asio handles platform-specific initialization internally
			}

			void cleanupNetwork() {
				// Nothing needed - Boost.Asio handles cleanup
			}
		}

// ============================================================
// SocketException
// ============================================================
std::string SocketException::errorToString(SocketError err) {
    switch (err) {
        case SocketError::Success: return "Success";
        case SocketError::WouldBlock: return "Operation would block (EAGAIN/WSAEWOULDBLOCK)";
        case SocketError::ConnectionRefused: return "Connection refused";
        case SocketError::ConnectionReset: return "Connection reset";
        case SocketError::TimedOut: return "Operation timed out";
        case SocketError::HostUnreachable: return "Host unreachable";
        case SocketError::NetworkUnreachable: return "Network unreachable";
        case SocketError::AddressInUse: return "Address in use";
        case SocketError::InvalidSocket: return "Invalid socket";
        case SocketError::Unknown: return "Unknown error";
        default: {
            // [V5.5 Fix] Return more detailed message for numeric error codes
            int err_val = static_cast<int>(err);
            char buf[128];
            snprintf(buf, sizeof(buf), "Unknown error (code=%d)", err_val);
            return std::string(buf);
        }
    }
}

		// ============================================================
		// IPAddress
		// ============================================================
		IPAddress::IPAddress(const std::string& ip, uint16_t port, int family)
			: family_(family), port_(port) {
			if (family == AF_INET) {
				addressData_.resize(sizeof(sockaddr_in));
				sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(addressData_.data());
				std::memset(addr, 0, sizeof(sockaddr_in));
				addr->sin_family = AF_INET;
				addr->sin_port = htons(port);
				inet_pton(AF_INET, ip.c_str(), &addr->sin_addr);
			}
			else if (family == AF_INET6) {
				addressData_.resize(sizeof(sockaddr_in6));
				sockaddr_in6* addr = reinterpret_cast<sockaddr_in6*>(addressData_.data());
				std::memset(addr, 0, sizeof(sockaddr_in6));
				addr->sin6_family = AF_INET6;
				addr->sin6_port = htons(port);
				inet_pton(AF_INET6, ip.c_str(), &addr->sin6_addr);
			}
		}

		std::string IPAddress::toString() const {
			char buf[INET6_ADDRSTRLEN] = { 0 };
			if (family_ == AF_INET) {
				const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(addressData_.data());
				inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
			}
			else if (family_ == AF_INET6) {
				const sockaddr_in6* addr = reinterpret_cast<const sockaddr_in6*>(addressData_.data());
				inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf));
			}
			return std::string(buf);
		}

		const void* IPAddress::rawData() const {
			return addressData_.data();
		}

		socklen_t IPAddress::rawLength() const {
			return static_cast<socklen_t>(addressData_.size());
		}

// ============================================================
// TcpSocket
// ============================================================
TcpSocket::TcpSocket(boost::asio::io_context& ioContext)
    : socket_(std::make_unique<boost::asio::ip::tcp::socket>(ioContext)) {
}

TcpSocket::TcpSocket(boost::asio::ip::tcp::socket&& socket)
    : socket_(std::make_unique<boost::asio::ip::tcp::socket>(std::move(socket))) {
}

		TcpSocket::~TcpSocket() {
			close();
		}

		SocketError TcpSocket::connect(boost::asio::io_context& ioContext, const IPAddress& address) {
			boost::system::error_code ec;

    if (!socket_ || !socket_->is_open()) {
        socket_ = std::make_unique<boost::asio::ip::tcp::socket>(ioContext);
        socket_->open(boost::asio::ip::tcp::v4(), ec);
        if (ec) return SocketError::InvalidSocket;
    }

			boost::asio::ip::tcp::endpoint endpoint;
			if (address.isV4()) {
				const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(address.rawData());
				boost::asio::ip::address_v4::bytes_type bytes;
				std::memcpy(bytes.data(), &addr->sin_addr, 4);
				endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v4(bytes), address.port());
			}
			else if (address.isV6()) {
				const sockaddr_in6* addr = reinterpret_cast<const sockaddr_in6*>(address.rawData());
				boost::asio::ip::address_v6::bytes_type bytes;
				std::memcpy(bytes.data(), addr->sin6_addr.s6_addr, 16);
				endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::address_v6(bytes), address.port());
			}

			socket_->connect(endpoint, ec);

			if (ec == boost::asio::error::connection_refused) return SocketError::ConnectionRefused;
			if (ec == boost::asio::error::timed_out) return SocketError::TimedOut;
			if (ec == boost::asio::error::host_not_found) return SocketError::HostUnreachable;
			if (ec) return SocketError::Unknown;
			return SocketError::Success;
		}

		SocketError TcpSocket::connect(boost::asio::io_context& ioContext, const std::string& host, uint16_t port) {
			Resolver resolver;
			Resolver::Results results;
			SocketError err = resolver.resolve(ioContext, results, host, port);
			if (err != SocketError::Success) {
				return err;
			}
			return connect(ioContext, results[0]);
		}

		SocketError TcpSocket::send(const void* data, size_t len, size_t& sent) {
			if (!socket_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			sent = boost::asio::write(*socket_, boost::asio::buffer(data, len), ec);

			if (ec == boost::asio::error::would_block) return SocketError::WouldBlock;
			if (ec == boost::asio::error::connection_reset) return SocketError::ConnectionReset;
			if (ec) return SocketError::Unknown;
			return SocketError::Success;
		}

		SocketError TcpSocket::receive(void* buffer, size_t len, size_t& received) {
			if (!socket_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			received = socket_->read_some(boost::asio::buffer(buffer, len), ec);

			if (ec == boost::asio::error::would_block) return SocketError::WouldBlock;
			if (ec == boost::asio::error::connection_reset) return SocketError::ConnectionReset;
			if (ec == boost::asio::error::eof) return SocketError::ConnectionReset;
			if (ec) return SocketError::Unknown;
			return SocketError::Success;
		}

		template<typename Handler>
		void TcpSocket::asyncSend(const void* data, size_t len, Handler&& handler) {
			if (!socket_) {
				handler(SocketError::InvalidSocket, 0);
				return;
			}
			boost::asio::async_write(*socket_, boost::asio::buffer(data, len),
				[handler](const boost::system::error_code& ec, size_t bytes) {
				if (ec) handler(SocketError::Unknown, 0);
				else handler(SocketError::Success, bytes);
			});
		}

		template<typename Handler>
		void TcpSocket::asyncReceive(void* buffer, size_t len, Handler&& handler) {
			if (!socket_) {
				handler(SocketError::InvalidSocket, 0);
				return;
			}
			socket_->async_read_some(boost::asio::buffer(buffer, len),
				[handler](const boost::system::error_code& ec, size_t bytes) {
				if (ec == boost::asio::error::would_block) handler(SocketError::WouldBlock, 0);
				else if (ec == boost::asio::error::connection_reset) handler(SocketError::ConnectionReset, 0);
				else if (ec == boost::asio::error::eof) handler(SocketError::ConnectionReset, 0);
				else if (ec) handler(SocketError::Unknown, 0);
				else handler(SocketError::Success, bytes);
			});
		}

		SocketError TcpSocket::setOptions(const SocketOptions& options) {
			if (!socket_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			if (options.reuseAddress) {
				socket_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
			}
			if (options.tcpNoDelay) {
				socket_->set_option(boost::asio::ip::tcp::no_delay(true), ec);
			}
			return SocketError::Success;
		}

		IPAddress TcpSocket::localAddress() const {
			if (!socket_) return IPAddress();
			boost::system::error_code ec;
			auto ep = socket_->local_endpoint(ec);
			return IPAddress(ep.address().to_string(), ep.port(), ep.address().is_v4() ? AF_INET : AF_INET6);
		}

		IPAddress TcpSocket::remoteAddress() const {
			if (!socket_) return IPAddress();
			boost::system::error_code ec;
			auto ep = socket_->remote_endpoint(ec);
			return IPAddress(ep.address().to_string(), ep.port(), ep.address().is_v4() ? AF_INET : AF_INET6);
		}

		void TcpSocket::close() {
			if (socket_ && socket_->is_open()) {
				boost::system::error_code ec;
				socket_->close(ec);
			}
		}

		void TcpSocket::shutdown() {
			if (socket_ && socket_->is_open()) {
				boost::system::error_code ec;
				socket_->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
			}
		}

// ============================================================
// TcpAcceptor
// ============================================================
TcpAcceptor::TcpAcceptor(boost::asio::io_context& ioContext)
    : acceptor_(std::make_unique<boost::asio::ip::tcp::acceptor>(ioContext)) {
}

		TcpAcceptor::~TcpAcceptor() {
			close();
		}

SocketError TcpAcceptor::listen(boost::asio::io_context& ioContext, uint16_t port, const std::string& host, int backlog) {
    if (!acceptor_) {
        acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(ioContext);
    }
    boost::system::error_code ec;

			acceptor_->open(boost::asio::ip::tcp::v4(), ec);
			if (ec) return SocketError::InvalidSocket;

			acceptor_->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);

			boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), port);
			acceptor_->bind(endpoint, ec);
			if (ec) return SocketError::AddressInUse;

			acceptor_->listen(backlog, ec);
			if (ec) return SocketError::Unknown;

			boundAddress_ = IPAddress(host.empty() ? "0.0.0.0" : host, port, AF_INET);
			return SocketError::Success;
		}

		SocketError TcpAcceptor::accept(TcpSocket& client, IPAddress* clientAddr) {
			if (!acceptor_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			auto socket = acceptor_->accept(ec);

			if (ec == boost::asio::error::would_block) return SocketError::WouldBlock;
			if (ec) return SocketError::InvalidSocket;

			client = TcpSocket(std::move(socket));

			if (clientAddr) {
				*clientAddr = client.remoteAddress();
			}

			return SocketError::Success;
		}

		template<typename Handler>
		void TcpAcceptor::asyncAccept(TcpSocket& client, Handler&& handler) {
			if (!acceptor_) {
				handler(SocketError::InvalidSocket);
				return;
			}
			acceptor_->async_accept(
				[this, &client, handler](const boost::system::error_code& ec, boost::asio::ip::tcp::socket socket) {
				if (ec) {
					handler(SocketError::Unknown);
				}
				else {
					client = TcpSocket(std::move(socket));
					handler(SocketError::Success);
				}
			});
		}

		void TcpAcceptor::close() {
			if (acceptor_ && acceptor_->is_open()) {
				boost::system::error_code ec;
				acceptor_->close(ec);
			}
		}

// ============================================================
// UdpSocket
// ============================================================
UdpSocket::UdpSocket(boost::asio::io_context& ioContext)
    : socket_(std::make_unique<boost::asio::ip::udp::socket>(ioContext)) {
}

UdpSocket::UdpSocket(boost::asio::ip::udp::socket&& socket)
    : socket_(std::make_unique<boost::asio::ip::udp::socket>(std::move(socket))) {
}

		UdpSocket::~UdpSocket() {
			close();
		}

SocketError UdpSocket::bind(boost::asio::io_context& ioContext, uint16_t port, const std::string& host) {
    if (!socket_) {
        socket_ = std::make_unique<boost::asio::ip::udp::socket>(ioContext);
    }
    boost::system::error_code ec;
    socket_->open(boost::asio::ip::udp::v4(), ec);
    if (ec) return SocketError::InvalidSocket;

			boost::asio::ip::udp::endpoint endpoint(boost::asio::ip::udp::v4(), port);
			socket_->bind(endpoint, ec);
			if (ec) return SocketError::AddressInUse;
			return SocketError::Success;
		}

		SocketError UdpSocket::sendTo(const void* data, size_t len, const IPAddress& dest, size_t& sent) {
			if (!socket_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			boost::asio::ip::udp::endpoint endpoint;

			if (dest.isV4()) {
				const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(dest.rawData());
				boost::asio::ip::address_v4::bytes_type bytes;
				std::memcpy(bytes.data(), &addr->sin_addr, 4);
				endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4(bytes), dest.port());
			}
			else {
				const sockaddr_in6* addr = reinterpret_cast<const sockaddr_in6*>(dest.rawData());
				boost::asio::ip::address_v6::bytes_type bytes;
				std::memcpy(bytes.data(), addr->sin6_addr.s6_addr, 16);
				endpoint = boost::asio::ip::udp::endpoint(boost::asio::ip::address_v6(bytes), dest.port());
			}

			sent = socket_->send_to(boost::asio::buffer(data, len), endpoint, 0, ec);
			if (ec) return SocketError::Unknown;
			return SocketError::Success;
		}

		SocketError UdpSocket::receiveFrom(void* buffer, size_t len, IPAddress& sender, size_t& received) {
			if (!socket_) return SocketError::InvalidSocket;
			boost::system::error_code ec;
			boost::asio::ip::udp::endpoint senderEndpoint;

			received = socket_->receive_from(boost::asio::buffer(buffer, len), senderEndpoint, 0, ec);

    if (ec == boost::asio::error::would_block) return SocketError::WouldBlock;

    // An ICMP "port unreachable" from the peer surfaces on the next receive as a
    // reset-style error on every platform (Windows WSAECONNRESET / WSAENETRESET,
    // POSIX ECONNRESET), and a non-blocking socket reports EAGAIN / EWOULDBLOCK.
    // None of these means "the socket is broken": there is simply no datagram to
    // read, so report WouldBlock and let the caller retry. Compare Asio error
    // *conditions* instead of numeric errno values — the numbers differ between
    // Windows (10054/10035) and POSIX (104/11).
    if (ec == boost::asio::error::try_again ||
        ec == boost::asio::error::connection_reset ||
        ec == boost::asio::error::connection_refused ||
        ec == boost::asio::error::network_reset) {
        return SocketError::WouldBlock;
    }

    // [V5.5 Fix] Map more specific errors instead of returning Unknown for all
    // These are the most common errors that should be retried
    if (ec == boost::asio::error::operation_aborted) return SocketError::InvalidSocket;  // Socket closed
    if (ec == boost::asio::error::message_size) return SocketError::Unknown;  // Message too large
    if (ec == boost::asio::error::network_reset) return SocketError::NetworkUnreachable;
    if (ec == boost::asio::error::host_unreachable) return SocketError::HostUnreachable;
    if (ec == boost::asio::error::connection_reset) return SocketError::ConnectionReset;
    if (ec == boost::asio::error::timed_out) return SocketError::TimedOut;

    // Log unknown errors for debugging
    if (ec) {
        // Log to stderr for now - in production this should use proper logging
        // The error message contains the actual system error description
        return SocketError::Unknown;
    }

			sender = IPAddress(senderEndpoint.address().to_string(),
				senderEndpoint.port(),
				senderEndpoint.address().is_v4() ? AF_INET : AF_INET6);
			return SocketError::Success;
		}

SocketError UdpSocket::setOptions(const SocketOptions& options) {
    if (!socket_) return SocketError::InvalidSocket;
    boost::system::error_code ec;
    if (options.reuseAddress) {
        socket_->set_option(boost::asio::ip::udp::socket::reuse_address(true), ec);
    }

    // 收发缓冲：用 Boost.Asio 的可移植选项，不再对 native_handle 直接
    // setsockopt。RTP 需要 4MB 级别才能吸收视频突发，调用方（SimpleRtpTransport）
    // 显式设置；这里负责真正落到 socket 上。
    if (options.sendBufferSize > 0) {
        socket_->set_option(boost::asio::socket_base::send_buffer_size(
            options.sendBufferSize), ec);
    }
    if (options.receiveBufferSize > 0) {
        socket_->set_option(boost::asio::socket_base::receive_buffer_size(
            options.receiveBufferSize), ec);
    }
    {
        // 实际值可能被内核截断（Linux net.core.*mem_max）或翻倍（SO_RCVBUF），
        // 记录实测值，避免"以为设了 4MB"。
        boost::system::error_code getEc;
        boost::asio::socket_base::send_buffer_size actualSend(0);
        boost::asio::socket_base::receive_buffer_size actualRecv(0);
        socket_->get_option(actualSend, getEc);
        socket_->get_option(actualRecv, getEc);
        // 低层封装可能早于日志初始化被使用，取不到 logger 就不记。
        if (ILogManager* logManager = GetLogManager()) {
            if (ILogger* logger = logManager->GetLogger(LogModule::Base)) {
                logger->DebugF(
                    "[SocketBuf] requested send=%d recv=%d | actual send=%d recv=%d",
                    options.sendBufferSize, options.receiveBufferSize,
                    static_cast<int>(actualSend.value()),
                    static_cast<int>(actualRecv.value()));
            }
        }
    }

    // [V7.1 Note 2026-08-13] receiveTimeout / sendTimeout used to be applied
    // here via raw setsockopt(SO_RCVTIMEO / SO_SNDTIMEO). That implementation
    // was removed because:
    //   1. It required a #ifdef _WIN32 block (POSIX timeval vs Win32 DWORD)
    //      which violates the "single source code, all platforms" principle
    //      of this wrapper.
    //   2. Boost.Asio provides a portable alternative: a steady_timer can call
    //      socket_->cancel() to wake up a stuck synchronous receive_from() on
    //      every platform with identical semantics. That mechanism now lives
    //      in SimpleRtpTransport (see watchdog_ / cancelWatchdog()).
    //
    // The fields are intentionally kept in the SocketOptions struct so legacy
    // callers continue to compile; they are simply ignored here.
    (void)options.receiveTimeout;
    (void)options.sendTimeout;

    return SocketError::Success;
}

SocketError UdpSocket::setBroadcast(bool enable) {
    if (!socket_) return SocketError::InvalidSocket;
    boost::system::error_code ec;
    socket_->set_option(boost::asio::ip::udp::socket::broadcast(enable), ec);
    return ec ? SocketError::Unknown : SocketError::Success;
}

void UdpSocket::cancel() {
    if (socket_ && socket_->is_open()) {
        // Boost::Asio's cancel() is fully portable: CancelIoEx on
        // Windows, epoll/kqueue wakeup on Linux. Either way the
        // currently pending async op completes with operation_aborted.
        boost::system::error_code ec;
        socket_->cancel(ec);
    }
}

void UdpSocket::close() {
    if (socket_ && socket_->is_open()) {
        boost::system::error_code ec;
        socket_->close(ec);
    }
}

		// ============================================================
		// Resolver
		// ============================================================
		SocketError Resolver::resolve(boost::asio::io_context& ioContext, Results& results,
			const std::string& host, uint16_t port, int /*family*/) {
			results.clear();

			boost::system::error_code ec;
			boost::asio::ip::tcp::resolver resolver(ioContext);

			std::string portStr = std::to_string(port);
			boost::asio::ip::tcp::resolver::query query(host, portStr);

			auto endpoints = resolver.resolve(query, ec);
			if (ec) return SocketError::HostUnreachable;

			for (const auto& ep : endpoints) {
				int family = ep.endpoint().address().is_v4() ? AF_INET : AF_INET6;
				results.emplace_back(ep.endpoint().address().to_string(), ep.endpoint().port(), family);
			}

			return results.empty() ? SocketError::HostUnreachable : SocketError::Success;
		}

		// Explicit template instantiations (needed for header-only with templates)
		template void TcpSocket::asyncSend<>(const void* data, size_t len, std::function<void(SocketError, size_t)>&& handler);
		template void TcpSocket::asyncReceive<>(void* buffer, size_t len, std::function<void(SocketError, size_t)>&& handler);
		template void TcpAcceptor::asyncAccept<>(TcpSocket& client, std::function<void(SocketError)>&& handler);

	}
} // namespace vcs::base
