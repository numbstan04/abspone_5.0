/**
 * @file Network.cpp
 * @brief 网络管理器实现（跨平台）
 *
 * 使用 PlatformSocket 提供跨平台网络支持
 */

#include "Network.h"
#include "PlatformSocket.h"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <ctime>

namespace vcs {
	namespace base {

		// Connection context
		struct ConnectionContext {
			std::string connId;
			std::unique_ptr<TcpSocket> socket;  // Use pointer to avoid default construction issues
			INetworkCallback* callback = nullptr;
		};

		// Network manager implementation using Boost.Asio
		class Network::Impl {
		public:
			boost::asio::io_context ioContext;
			std::unordered_map<std::string, ConnectionContext> connections;
			std::unique_ptr<TcpAcceptor> acceptor;
			std::string nextConnId;
			mutable std::mutex mutex;
		};

		Network::Network()
			: impl_(std::make_unique<Impl>()) {
			detail::initNetwork();
		}

		Network::~Network() {
			Stop();
			detail::cleanupNetwork();
		}

		Response Network::Listen(uint16_t port) {
			std::lock_guard<std::mutex> lock(impl_->mutex);

			impl_->acceptor = std::make_unique<TcpAcceptor>();
			SocketError err = impl_->acceptor->listen(impl_->ioContext, port, "0.0.0.0");

			if (err != SocketError::Success) {
				impl_->acceptor.reset();
				return Response::Error(ResponseCode::InternalError,
					"listen failed: " + std::string(SocketException(err).what()));
			}

			localAddr_.port = port;
			localAddr_.ip = "0.0.0.0";

			return Response::Success("Listening on port " + std::to_string(port));
		}

		Response Network::Connect(const std::string& ip, uint16_t port, INetworkCallback* callback) {
			std::lock_guard<std::mutex> lock(impl_->mutex);

			auto socket = std::make_unique<TcpSocket>();
			SocketError err = socket->connect(impl_->ioContext, ip, port);

			if (err != SocketError::Success) {
				return Response::Error(ResponseCode::InternalError,
					"connect failed: " + std::string(SocketException(err).what()));
			}

			// Generate connection ID
			impl_->nextConnId = "conn_" + std::to_string(
				std::hash<std::string>{}(ip + std::to_string(port) + std::to_string(time(nullptr))));
			std::string connId = impl_->nextConnId;

			// Save connection context
			ConnectionContext ctx;
			ctx.connId = connId;
			ctx.socket = std::move(socket);
			ctx.callback = callback;
			impl_->connections[connId] = std::move(ctx);

			return Response::Success("Connected to " + ip + ":" + std::to_string(port));
		}

		void Network::Disconnect(const std::string& connId) {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->connections.erase(connId);
		}

		Response Network::Send(const std::string& connId, const uint8_t* data, size_t len) {
			std::lock_guard<std::mutex> lock(impl_->mutex);

			auto it = impl_->connections.find(connId);
			if (it == impl_->connections.end()) {
				return Response::Error(ResponseCode::NotFound, "Connection not found");
			}

			if (!it->second.socket) {
				return Response::Error(ResponseCode::InternalError, "Socket is null");
			}

			size_t sent = 0;
			SocketError err = it->second.socket->send(data, len, sent);

			if (err != SocketError::Success) {
				return Response::Error(ResponseCode::InternalError,
					"send failed: " + std::string(SocketException(err).what()));
			}

			return Response::Success("Sent " + std::to_string(sent) + " bytes");
		}

		Address Network::GetLocalAddress() const {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			return localAddr_;
		}

		void Network::Stop() {
			std::lock_guard<std::mutex> lock(impl_->mutex);
			impl_->connections.clear();
			impl_->acceptor.reset();
		}

	}
} // namespace vcs::base
