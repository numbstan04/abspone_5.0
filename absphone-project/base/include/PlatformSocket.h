/**
 * @file PlatformSocket.h
 * @brief Cross-platform Socket wrapper using Boost.Asio
 *
 * True cross-platform implementation - one codebase for Windows, Linux, macOS
 * No #ifdef conditional compilation for platform differences
 */
#pragma once

#include "base_export.h"
#include <string>
#include <cstdint>
#include <vector>
#include <memory>
#include <functional>

 // Boost.Asio (header-only)
#include <boost/asio.hpp>

namespace vcs {
	namespace base {

		// ============================================================
		// Error codes
		// ============================================================
		enum class SocketError {
			Success = 0,
			WouldBlock,
			ConnectionRefused,
			ConnectionReset,
			TimedOut,
			HostUnreachable,
			NetworkUnreachable,
			AddressInUse,
			InvalidSocket,
			Unknown
		};

		// ============================================================
		// IP Address
		// ============================================================
		class VCS_BASE_API IPAddress {
		public:
			IPAddress() : family_(AF_UNSPEC), port_(0) {}
			explicit IPAddress(const std::string& ip, uint16_t port, int family = AF_INET);

			bool isV4() const { return family_ == AF_INET; }
			bool isV6() const { return family_ == AF_INET6; }
			uint16_t port() const { return port_; }
			int family() const { return family_; }
			std::string toString() const;
			const void* rawData() const;
			socklen_t rawLength() const;

		private:
			int family_;
			uint16_t port_;
			std::vector<uint8_t> addressData_;
		};

		// ============================================================
		// Socket Options
		// ============================================================
		struct SocketOptions {
			bool reuseAddress = true;
			bool nonBlocking = false;
			int receiveTimeout = 30000;    // ms
			int sendTimeout = 30000;       // ms
			int receiveBufferSize = 65536;
			int sendBufferSize = 65536;
			bool tcpNoDelay = true;        // Disable Nagle algorithm
		};

		// ============================================================
		// TCP Socket
		// ============================================================
		class VCS_BASE_API TcpSocket {
		public:
			TcpSocket() = default;
			explicit TcpSocket(boost::asio::io_context& ioContext);
			explicit TcpSocket(boost::asio::ip::tcp::socket&& socket);
			~TcpSocket();

			TcpSocket(const TcpSocket&) = delete;
			TcpSocket& operator=(const TcpSocket&) = delete;

			TcpSocket(TcpSocket&& other) noexcept = default;
			TcpSocket& operator=(TcpSocket&& other) noexcept = default;

			bool isOpen() const { return socket_ && socket_->is_open(); }
			boost::asio::ip::tcp::socket& native() { return *socket_; }
			const boost::asio::ip::tcp::socket& native() const { return *socket_; }

			// Connect
			SocketError connect(boost::asio::io_context& ioContext, const IPAddress& address);
			SocketError connect(boost::asio::io_context& ioContext, const std::string& host, uint16_t port);

			// Send/Receive
			SocketError send(const void* data, size_t len, size_t& sent);
			SocketError receive(void* buffer, size_t len, size_t& received);

			// Async send/receive
			template<typename Handler>
			void asyncSend(const void* data, size_t len, Handler&& handler);

			template<typename Handler>
			void asyncReceive(void* buffer, size_t len, Handler&& handler);

			// Set options
			SocketError setOptions(const SocketOptions& options);

			// Get local/remote address
			IPAddress localAddress() const;
			IPAddress remoteAddress() const;

			void close();
			void shutdown();

		private:
			std::unique_ptr<boost::asio::ip::tcp::socket> socket_;
		};

		// ============================================================
		// TCP Acceptor (Server side)
		// ============================================================
		class VCS_BASE_API TcpAcceptor {
		public:
			TcpAcceptor() = default;
			explicit TcpAcceptor(boost::asio::io_context& ioContext);
			~TcpAcceptor();

			TcpAcceptor(const TcpAcceptor&) = delete;
			TcpAcceptor& operator=(const TcpAcceptor&) = delete;

			TcpAcceptor(TcpAcceptor&& other) noexcept = default;
			TcpAcceptor& operator=(TcpAcceptor&& other) noexcept = default;

			bool isOpen() const { return acceptor_ && acceptor_->is_open(); }

			// Listen
			SocketError listen(boost::asio::io_context& ioContext, uint16_t port, const std::string& host = "", int backlog = 128);

			// Accept connection
			SocketError accept(TcpSocket& client, IPAddress* clientAddr = nullptr);

			template<typename Handler>
			void asyncAccept(TcpSocket& client, Handler&& handler);

			void close();

		private:
			std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
			IPAddress boundAddress_;
		};

		// ============================================================
		// UDP Socket
		// ============================================================
		class VCS_BASE_API UdpSocket {
		public:
			UdpSocket() = default;
			explicit UdpSocket(boost::asio::io_context& ioContext);
			explicit UdpSocket(boost::asio::ip::udp::socket&& socket);
			~UdpSocket();

			UdpSocket(const UdpSocket&) = delete;
			UdpSocket& operator=(const UdpSocket&) = delete;

			UdpSocket(UdpSocket&& other) noexcept = default;
			UdpSocket& operator=(UdpSocket&& other) noexcept = default;

			bool isOpen() const { return socket_ && socket_->is_open(); }

    // Access to the underlying Boost.Asio socket. Required by callers that
    // need to drive async I/O or call Boost-native operations such as
    // cancel() (the cross-platform equivalent of SO_RCVTIMEO).
    boost::asio::ip::udp::socket& native() { return *socket_; }
    const boost::asio::ip::udp::socket& native() const { return *socket_; }

    // Bind
    SocketError bind(boost::asio::io_context& ioContext, uint16_t port, const std::string& host = "");

			// Send/Receive
			SocketError sendTo(const void* data, size_t len, const IPAddress& dest, size_t& sent);
			SocketError receiveFrom(void* buffer, size_t len, IPAddress& sender, size_t& received);

			// Set options
			SocketError setOptions(const SocketOptions& options);
			SocketError setBroadcast(bool enable);

    // Cancel any in-flight async operation. Cross-platform: routes
    // through CancelIoEx on Windows and through epoll/kqueue on Linux.
    // After cancel() returns, the next async handler completes with
    // boost::asio::error::operation_aborted.
    void cancel();

    void close();

		private:
			std::unique_ptr<boost::asio::ip::udp::socket> socket_;
		};

		// ============================================================
		// DNS Resolver
		// ============================================================
		class VCS_BASE_API Resolver {
		public:
			using Results = std::vector<IPAddress>;

			// Synchronous resolution
			SocketError resolve(boost::asio::io_context& ioContext, Results& results,
				const std::string& host, uint16_t port, int family = AF_UNSPEC);
		};

		// ============================================================
		// Socket Exception
		// ============================================================
		class VCS_BASE_API SocketException : public std::exception {
		public:
			explicit SocketException(SocketError err, const std::string& msg = "")
				: error_(err), message_(msg.empty() ? errorToString(err) : msg) {}

			const char* what() const noexcept override { return message_.c_str(); }
			SocketError error() const { return error_; }

			static std::string errorToString(SocketError err);

		private:
			SocketError error_;
			std::string message_;
		};

		// ============================================================
		// Utility functions
		// ============================================================
		namespace detail {
			VCS_BASE_API void initNetwork();
			VCS_BASE_API void cleanupNetwork();
		}

	}
} // namespace vcs::base
