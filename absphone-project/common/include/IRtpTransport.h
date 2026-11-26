#pragma once

// RTP transport contract (defined in common, implemented by gateway,
// consumed by media).
//
// Layering (docs/06 §1.4.1): this interface lives in common (L1) so that
// gateway and media never include each other. gateway implements it for
// SIP / H.323 / WebRTC and hands instances to media::StartMedia once per
// call; media only ever talks to the interface.
//
// Design rules:
//   1. Gateway is a pure datagram passthrough: it never parses RTP headers
//      (payload type / SSRC / timestamp / sequence are parsed by media).
//   2. Every callback carries the full SessionContext, so the receiver never
//      needs a side lookup.
//   3. The callback object must outlive the transport. SetCallback(nullptr)
//      detaches it and, as with StopReceiving(), returns only once no dispatch
//      is in flight - so the callback object may be destroyed right after it
//      returns. SetCallback must be called before StartReceiving().
//   4. One transport instance per media stream (MediaStreamKind).
//
// Receive-gate semantics (docs/09 §2.3 / §2.5):
//   - Packets arriving before StartReceiving() are silently dropped: no
//     callback invocation, no warning spam. The missed-keyframe gap is
//     closed by the receiver's FIR request (media side), not by buffering.
//   - StopReceiving() returning guarantees no in-flight callback afterwards.
//     UDP transports satisfy this by cancel + join; pj-backed transports
//     uninstall the stack's external receive callback and barrier in-flight
//     dispatch before returning (SipMediaTransport::DetachReceivers).
//   - SendRtp() after StopReceiving() returns false; it must not crash.
//   - StartReceiving() / StopReceiving() / SetCallback(nullptr) are idempotent.
//   - A callback must not synchronously detach its own transport (dispatch is
//     serialised with detach); tear down from another thread instead.
//
// Lifecycle (docs/09 §3):
//   - Gateway creates transports at media negotiation and destroys them at
//     session teardown. Media holds the shared_ptr only between StartMedia
//     and StopMedia. A pj-backed transport's native handle is dereferenceable
//     only while the protocol stack's call media is alive.

#include "MediaStreamKind.h"
#include "SessionContext.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace vcs {

// Raw RTP datagram, including the RTP header.
struct RtpPacket {
    const uint8_t* data = nullptr;
    size_t length = 0;
};

// Raw RTCP datagram (possibly compound), not parsed by the gateway.
struct RtcpPacket {
    const uint8_t* data = nullptr;
    size_t length = 0;
};

class IRtpTransportCallback {
public:
    virtual ~IRtpTransportCallback() = default;

    // A complete RTP datagram was received. The gateway does not distinguish
    // audio from video; media parses the header to decide.
    virtual void OnRtpReceived(const RtpPacket& packet,
                               const SessionContext& context) = 0;

    virtual void OnRtcpReceived(const RtcpPacket& packet,
                                const SessionContext& context) = 0;

    virtual void OnTransportConnected(const SessionContext& context) = 0;

    virtual void OnTransportDisconnected(const SessionContext& context,
                                         const std::string& reason) = 0;

    virtual void OnTransportError(const SessionContext& context,
                                  int errorCode,
                                  const std::string& errorMessage) = 0;
};

// One RTP transport instance per session media stream.
class IRtpTransport {
public:
    virtual ~IRtpTransport() = default;

    virtual const std::string& GetType() const = 0;

    // Connection id: equals the sessionId that owns this transport.
    virtual const std::string& GetConnectionId() const = 0;

    virtual uint32_t GetSsrc() const = 0;
    virtual void SetSsrc(uint32_t ssrc) = 0;

    virtual bool SendRtp(const RtpPacket& packet) = 0;
    virtual bool SendRtcp(const RtcpPacket& packet) = 0;

    // Request a keyframe from the remote sender via RTCP FIR / PLI.
    virtual bool SendFir(uint32_t remoteSsrc, uint32_t localSsrc) = 0;

    virtual bool IsConnected() const = 0;
    virtual std::string GetRemoteAddress() const = 0;

    // Local endpoint of this transport, for the media-information snapshot
    // only ("which port did I actually bind"). Diagnostic, never routing:
    // defaulted so an implementation that cannot report it stays compilable.
    // Empty string / 0 means "unknown".
    virtual std::string GetLocalAddress() const { return std::string(); }
    virtual uint16_t GetLocalPort() const { return 0; }

    // The callback object is owned by the caller and must outlive the
    // transport (or be detached with nullptr before destruction).
    virtual bool SetCallback(IRtpTransportCallback* callback) = 0;
    virtual IRtpTransportCallback* GetCallback() const = 0;

    virtual void SetContext(const SessionContext& context) = 0;
    virtual const SessionContext& GetContext() const = 0;

    // Requires SetCallback() and SetContext() to have been called.
    virtual bool StartReceiving() = 0;
    virtual void StopReceiving() = 0;
    virtual bool IsReceiving() const = 0;
};

// One session's per-kind transport set: gateway builds it, media consumes it
// (docs/09 §1.2). Canonical definition lives here; media headers re-export
// the name as vcs::media::MemberMediaTransports via using-declarations.
using MemberMediaTransports =
    std::map<MediaStreamKind, std::shared_ptr<IRtpTransport>>;

} // namespace vcs
