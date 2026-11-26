#pragma once

#include "gateway_export.h"
#include "IRtpTransport.h"
#include "MediaStreamKind.h"
#include "NegotiatedCodec.h"
#include <string>
#include <memory>
#include <cstdint>
#include <vector>

namespace vcs {
	namespace gateway {

		/**
		 * @brief Session state enum
		 *
		 * Represents the lifecycle state of a call session
		 */
		enum class SessionState {
			IncomingPending,   // Incoming call pending accept
			OutgoingPending,   // Outgoing call pending response
			Connecting,        // Connection establishing (media negotiation in progress)
			Active,           // Call established
			Disconnecting,    // Disconnecting
			Disconnected      // Disconnected
		};

		/**
		 * @brief Protocol type
		 *
		 * Added in v2 architecture refactoring: identifies which protocol adapter
		 * the session came from. When SessionManager serves multiple protocols,
		 * this field distinguishes session sources.
		 */
		enum class ProtocolType {
			UNKNOWN = 0,  // Unspecified (backward compatibility)
			SIP,          // SIP protocol
			H323,         // H.323 protocol
			WEBRTC        // WebRTC protocol
		};

		/**
		 * @brief [V7.0 Phase-2 D] Stream-level transport metadata
		 *
		 * Wraps an IRtpTransport together with the per-stream metadata that
		 * the old single-transport CallSession exposed as bare fields
		 * (remoteAddress/remotePort). One StreamTransport is created per media
		 * stream; the SessionManager / SignalingHandler routes RTP/RTCP packets
		 * to the right transport by looking at the kind.
		 *
		 * Design notes:
		 * - transportObj is the actual network object (shared_ptr so the
		 *   underlying socket can be shared with the media layer).
		 * - isPrimary marks the stream that should be used for "single transport"
		 *   compatibility reads (GetPrimaryTransport).
		 * - kind disambiguates which media stream this entry represents.
		 */
		struct StreamTransport {
			MediaStreamKind kind = MediaStreamKind::Audio;
			std::shared_ptr<IRtpTransport> transportObj;
			std::string remoteAddress;          // Remote address of this stream
			uint16_t    remotePort = 0;         // Remote port of this stream
			bool        isPrimary = false;      // Marks the legacy-compat "primary" stream
		};

		/**
		 * @brief Call session data container
		 *
		 * Only stores session-related data, no business logic.
		 * Used as data structure for saving context information.
		 *
		 * Design Principles:
		 * 1. No business logic, only data container
		 * 2. Managed internally by SessionManager
		 * 3. Interacts with Conference layer via CommandDispatcher
		 *
		 * [V7.0 Phase-2 D/E] Multi-stream media refactoring:
		 * - The legacy single-transport fields (mediaTransport / remoteAddress /
		 *   remotePort) have been replaced by a vector of StreamTransport entries.
		 * - One CallSession can now carry N independent media streams (audio +
		 *   video + secondary video + data).
		 * - For legacy single-stream compatibility, the first Audio entry is
		 *   flagged isPrimary and exposed by GetPrimaryTransport().
		 *
		 * [Single write entry] The transport table is READ-ONLY to everyone
		 * outside SessionManager: the vector and its mutators are private, and
		 * SessionManager::AttachTransport is the only way to change it. Callers
		 * hold a shared_ptr<CallSession> without any lock, so letting them write
		 * the vector (or mutate StreamTransport in place) raced with teardown
		 * and with each other. Readers get the const accessors below, or a
		 * locked snapshot via SessionManager::GetTransportSnapshot.
		 */
		struct CallSession {
			int callId;                        // Protocol stack call ID (e.g., PJSIP)
			std::string sessionId;             // Session unique ID (UUID format)
			std::string caller;                // Caller
			std::string callee;                // Callee
			std::string conferenceId;          // Associated conference ID
			SessionState state;                // Current state
			int64_t startTime;                // Start time (millisecond timestamp)
			int64_t connectTime;              // Connect time (millisecond timestamp)
			// [V5.8] Last-active timestamp used for inactivity-based timeout cleanup.
			// Updated whenever the session receives a media packet or state change.
			int64_t lastActiveTime;           // Last active time (millisecond timestamp)

			// Bumped only when the installed transport OBJECT changes (first
			// install or replacement). Address-only refreshes and re-registration
			// of the same object do not bump it. Upper layers compare this with
			// the generation their media path is wired to; a difference means a
			// hot-swap is required even though no event reports it.
			uint32_t transportGeneration = 0;

			// [V5.7] Protocol type (added in v2 architecture)
			ProtocolType protocol = ProtocolType::UNKNOWN;

			// Codec negotiated from the remote SDP, committed by MarkConnected.
			// Facts live here (single source of truth); CallConnected only
			// notifies with the codec names.
			vcs::NegotiatedCodec audioCodec;
			vcs::NegotiatedCodec videoCodec;

			// callId -1 = not yet bound to a protocol-stack call; pjsua ids
			// start at 0, so 0 is a valid id and must not double as "unbound".
			CallSession()
				: callId(-1)
				, state(SessionState::Disconnected)
				, startTime(0)
				, connectTime(0)
				, lastActiveTime(0) {
			}

			// ----------------------------------------------------------------
			// Read-only transport views (safe for any holder of the session)
			// ----------------------------------------------------------------

			/**
			 * @brief Get the primary (legacy-compat) transport.
			 * @return Pointer to the primary StreamTransport, or nullptr if no
			 *         transport has been added yet.
			 */
			const StreamTransport* GetPrimaryTransport() const {
				for (const auto& t : transports) {
					if (t.isPrimary) {
						return &t;
					}
				}
				if (!transports.empty()) {
					return &transports.front();
				}
				return nullptr;
			}

			/**
			 * @brief Find the transport entry for a given media stream kind.
			 * @return Pointer to the first matching entry, or nullptr if absent.
			 */
			const StreamTransport* GetTransportByKind(MediaStreamKind kind) const {
				for (const auto& t : transports) {
					if (t.kind == kind) {
						return &t;
					}
				}
				return nullptr;
			}

			/**
			 * @brief Get the IRtpTransport for a given kind (convenience over GetTransportByKind).
			 */
			std::shared_ptr<IRtpTransport> GetTransportObjectByKind(MediaStreamKind kind) const {
				const auto* t = GetTransportByKind(kind);
				return t ? t->transportObj : nullptr;
			}

			/**
			 * @brief Get the primary transport object (legacy-compat shortcut).
			 */
			std::shared_ptr<IRtpTransport> GetPrimaryTransportObject() const {
				const auto* t = GetPrimaryTransport();
				return t ? t->transportObj : nullptr;
			}

			/**
			 * @brief Get the remote address of the primary transport.
			 *
			 * Returns empty string if no transport exists. Provided as a
			 * drop-in replacement for the old session->remoteAddress field.
			 */
			const std::string& GetPrimaryRemoteAddress() const {
				static const std::string kEmpty;
				const auto* t = GetPrimaryTransport();
				return t ? t->remoteAddress : kEmpty;
			}

			/**
			 * @brief Get the remote port of the primary transport.
			 */
			uint16_t GetPrimaryRemotePort() const {
				const auto* t = GetPrimaryTransport();
				return t ? t->remotePort : 0;
			}

		private:
			// Only SessionManager mutates the transport table (single write
			// entry: AttachTransport / DetachAllTransports under the shard lock).
			friend class SessionManager;

			std::vector<StreamTransport> transports;

			/**
			 * @brief Add a media transport to this session.
			 *
			 * Appends a new StreamTransport to the transports vector. If this is
			 * the first entry and the caller has not yet marked any other entry
			 * primary, the new entry is also marked isPrimary (legacy single-SM
			 * compatibility).
			 *
			 * @param kind       The media stream kind (Audio / Video / ...).
			 * @param transport  The transport object (may be nullptr for streams
			 *                   whose network plane is not yet ready).
			 * @param remoteAddr Remote address string (may be empty).
			 * @param remotePort Remote port (0 if not yet known).
			 * @return Reference to the newly inserted entry. The reference is
			 *         invalidated by any subsequent mutation of `transports`.
			 */
			StreamTransport& AddTransport(MediaStreamKind kind,
				std::shared_ptr<IRtpTransport> transport,
				const std::string& remoteAddr = "",
				uint16_t remotePort = 0) {
				StreamTransport entry;
				entry.kind = kind;
				entry.transportObj = std::move(transport);
				entry.remoteAddress = remoteAddr;
				entry.remotePort = remotePort;
				// First-added entry becomes the primary by default. This preserves
				// legacy single-transport semantics for code paths that have not
				// been ported to per-kind access.
				if (transports.empty()) {
					entry.isPrimary = true;
				}
				transports.push_back(std::move(entry));
				return transports.back();
			}

			/**
			 * @brief Mutable view for SessionManager's own writers.
			 */
			StreamTransport* FindTransportMutable(MediaStreamKind kind) {
				for (auto& t : transports) {
					if (t.kind == kind) {
						return &t;
					}
				}
				return nullptr;
			}

			/**
			 * @brief Detach every transport from the session, returning them so
			 *        the caller can call StopReceiving outside any lock.
			 *
			 * [Deadlock fix] This method intentionally does NOT call
			 * StopReceiving. StopReceiving joins the transport's receive
			 * thread; if that thread is currently in its callback path and
			 * the callback needs the same lock the caller is holding, the two
			 * threads will deadlock. Detaching under the lock and stopping
			 * after the lock is released breaks the cycle.
			 *
			 * Each detached transport has its callback cleared before being
			 * returned, so any in-flight packet delivery becomes a no-op once
			 * the session no longer references the transport.
			 *
			 * @return Vector of detached transports. Caller is responsible
			 *         for calling StopReceiving on each, preferably after
			 *         releasing any lock that the transport callback might
			 *         need.
			 */
			std::vector<std::shared_ptr<IRtpTransport>> DetachAllTransports() {
				std::vector<std::shared_ptr<IRtpTransport>> result;
				result.reserve(transports.size());
				for (auto& t : transports) {
					if (t.transportObj) {
						// Clear the callback first so any in-flight packet
						// delivery becomes a no-op. SetCallback is internally
						// synchronized (callbackMutex_ in SimpleRtpTransport)
						// and safe to call under any lock.
						t.transportObj->SetCallback(nullptr);
						result.push_back(std::move(t.transportObj));
					}
				}
				transports.clear();
				return result;
			}
		};

		/**
		 * @brief Session read-only value snapshot (07 施工图 §2.4)
		 *
		 * The ONLY way for upper layers (conference) to read session facts.
		 * shared_ptr<CallSession> never leaves gateway; this struct is a
		 * value copy taken under the SessionManager shard lock.
		 */
		struct SessionView {
			bool hasSession = false;
			std::string sessionId;
			std::string conferenceId;
			std::string memberId;
			std::string caller;
			std::string callee;
			SessionState state = SessionState::Disconnected;
			int64_t startTimeMs = 0;
			int64_t connectTimeMs = 0;
			std::string primaryRemoteAddress;
			bool hasAudio = false;
			bool hasVideo = false;
			ProtocolType protocol = ProtocolType::UNKNOWN;
			vcs::NegotiatedCodec audioCodec;
			vcs::NegotiatedCodec videoCodec;
			// Transport object generation: changes only when the installed
			// transport object is replaced (see CallSession).
			uint32_t transportGeneration = 0;
		};

		/**
		 * @brief Call session info structure
		 *
		 * Read-only copy for external session info queries
		 */
		struct CallSessionInfo {
			std::string sessionId;
			std::string caller;
			std::string callee;
			std::string conferenceId;
			std::string protocol;
			SessionState state;
			int64_t startTime;
			int64_t connectTime;
			int64_t lastActiveTime;
		};

		// Note: v5.7 refactoring - removed the ISessionManager abstract interface that
		// was previously declared here.
		//
		// Reasons for removal:
		//   1. The interface does not match the v2 SessionManager's actual API
		//      (only 6 methods in the interface vs. 30+ methods in v2 SessionManager).
		//   2. No external consumer exists in the codebase (grep returned zero references).
		//   3. The v2 design document adopts a "concrete class + DI" pattern,
		//      where SessionManager itself is the sole implementation.
		// Callers requiring an abstraction layer should perform dependency injection
		// on the SessionManager concrete class.

	} // namespace gateway
} // namespace vcs
