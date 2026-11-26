#pragma once

#include "vcs_export.h"

// =====================================================================
// MCU global unified error code space
//
// [Design principles]
// 1. Single source of truth: the entire project (common / media / conference /
//    gateway / ...) shares this single error code space. No more per-module
//    error code files (absorbed the original MediaErrorCode and
//    BackendErrorCode; deleted the two standalone enums. Modeled after
//    PJSIP's pj_status_t design.)
//
// 2. Grouped by semantic range to enable log filtering, alert routing, and
//    canary releases:
//      0          : Success
//      1000-1999  : Generic errors (params, state, memory, business-agnostic failures)
//      2000-2999  : Conference-related errors
//      3000-3999  : Member-related errors
//      4000-4999  : Media-related errors (transport, codec, backend, SSRC, etc.)
//      5000-5999  : Recording-related errors
//      6000-6999  : Configuration-related errors
//      7000-7999  : Failure handling / circuit breaker / resource lifecycle errors
//      8000-8999  : Persistence / state recovery errors
//      9000+      : Unknown / unsupported
//
// 3. Open enumeration: new error codes must be appended at the end of the
//    range they belong to, to avoid breaking the ABI.
//
// 4. Naming convention: PascalCase, no prefix (the ErrorCode namespace
//    already provides scope).
//
// [History]
// - 2026-08-07: merged the original media::MediaErrorCode (39 entries) and
//   media::BackendErrorCode (13 entries) into this file; deleted
//   MediaErrorCode.h / BackendErrorCode.h. The previous
//   ToUpperErrorCode(MediaErrorCode) translation layer was retired (no
//   longer needed).
// =====================================================================

namespace vcs {

// Error code enumeration
enum class ErrorCode : int {
    Success = 0,

    // ============================================================
    // 1000-1999: Generic errors
    // ============================================================
    InvalidParams      = 1001,  // Invalid parameters (NULL, out of range, illegal enum value)
    OperationTimeout   = 1002,  // Generic operation timeout
    ResourceNotFound   = 1003,  // Generic resource not found
    ResourceConflict   = 1004,  // Generic resource conflict
    Unauthorized       = 1005,  // Unauthorized
    Forbidden          = 1006,  // Forbidden
    InternalError      = 1007,  // Internal error / invariant violation
    NotImplemented     = 1008,  // Feature not implemented
    Unknown            = 1009,  // Unknown error
    NotInitialized     = 1010,  // Not initialized
    AlreadyInitialized = 1011,  // Already initialized
    ShuttingDown       = 1012,  // Process is shutting down gracefully
    Unsupported        = 1013,  // Unsupported operation
    OutOfMemory        = 1014,  // Out of memory

    // ============================================================
    // 2000-2999: Conference-related errors
    // ============================================================
    ConferenceNotFound         = 2001,
    ConferenceFull             = 2002,
    ConferenceLocked           = 2003,
    ConferencePasswordRequired = 2004,
    ConferencePasswordMismatch = 2005,
    ConferenceAlreadyExists    = 2006,

    // ============================================================
    // 3000-3999: Member-related errors
    // ============================================================
    MemberNotFound        = 3001,
    MemberAlreadyExists   = 3002,
    MemberBanned          = 3003,
    MemberRoleNotAllowed  = 3004,
    MemberMuteNotAllowed  = 3005,

    // ============================================================
    // 4000-4999: Media-related errors
    // (merged from the original media::MediaErrorCode + media::BackendErrorCode)
    // ============================================================

    // Media transport (4001-4020)
    MediaTransportError      = 4001,  // Media transport error (generic)
    MediaCodecNotSupported   = 4002,  // Codec not supported
    MediaBandwidthInsufficient = 4003, // Insufficient bandwidth
    MediaConnectionLost      = 4004,  // Media connection lost
    MediaNetworkError        = 4005,  // Network error
    MediaSendFailed          = 4006,  // Send failed (replaces the original BackendErrorCode::kSendFailed)
    MediaReceiveFailed       = 4007,  // Receive failed (replaces the original BackendErrorCode::kReceiveFailed)
    MediaTransportNotReady   = 4008,  // Transport layer not ready (replaces the original BackendErrorCode::kTransportNotReady)

    // Media codec (4021-4030)
    MediaCodecInitFailed     = 4021,
    MediaCodecDecodeFailed   = 4022,
    MediaCodecEncodeFailed   = 4023,

    // Media backend / RTP (4031-4050)
    // (merged from the original media::BackendErrorCode 1000-1999 range)
    MediaBackendNotAvailable = 4031,  // Backend not available
    MediaBackendFatalError   = 4032,  // Backend fatal error
    MediaBackendInitFailed   = 4033,  // Backend initialization failed
    MediaBackendSpecificError = 4034, // Backend-specific error (catch-all; see backend logs for details)
    MediaSsrcPoolExhausted   = 4035,  // SSRC pool exhausted
    MediaSsrcConflict        = 4036,  // SSRC conflict

    // Media signal handler (4051-4060)
    MediaHandlerNotFound     = 4051,
    MediaHandlerAlreadyExists = 4052,
    MediaHandlerChainBroken  = 4053,

    // Media member errors (4061-4070)
    MediaMemberDecodeFailure = 4061,
    MediaMemberTimeout       = 4062,
    MediaMemberDisconnected  = 4063,

    // ============================================================
    // 5000-5999: Recording-related errors
    // ============================================================
    RecordingNotStarted    = 5001,
    RecordingAlreadyStarted = 5002,
    RecordingStorageError  = 5003,

    // ============================================================
    // 6000-6999: Configuration-related errors
    // ============================================================
    ConfigInvalid = 6001,
    ConfigNotFound = 6002,

    // ============================================================
    // 7000-7999: Failure handling / circuit breaker / resource lifecycle
    // (merged from the original media::MediaErrorCode 2000-2999 range, Q6 failure handling)
    // ============================================================
    CircuitBreakerOpen       = 7001,  // Circuit breaker is open
    GracefulShutdownTimeout  = 7002,  // Graceful shutdown timed out
    ResourceLeakDetected     = 7003,  // Resource leak detected

    // ============================================================
    // 8000-8999: Persistence / state recovery
    // (merged from the original media::MediaErrorCode 3000-3999 range, Q6 resource lifecycle)
    // ============================================================
    ConferenceStateLost      = 8001,  // Crash recovery failed to restore conference state
    PersistenceFailed        = 8002,  // Persistence failed
};

/**
 * @brief Convert vcs::ErrorCode to a human-readable string.
 *
 * The returned pointer is to a static literal; do not free.
 * Used by log statements across all modules; replaces the former
 * per-module ToString helpers (media::ToString(MediaErrorCode), etc.).
 */
inline const char* ToString(ErrorCode c) {
    switch (c) {
        case ErrorCode::Success:                  return "Success";
        case ErrorCode::InvalidParams:            return "InvalidParams";
        case ErrorCode::OperationTimeout:         return "OperationTimeout";
        case ErrorCode::ResourceNotFound:         return "ResourceNotFound";
        case ErrorCode::ResourceConflict:         return "ResourceConflict";
        case ErrorCode::Unauthorized:             return "Unauthorized";
        case ErrorCode::Forbidden:                return "Forbidden";
        case ErrorCode::InternalError:            return "InternalError";
        case ErrorCode::NotImplemented:           return "NotImplemented";
        case ErrorCode::Unknown:                  return "Unknown";
        case ErrorCode::NotInitialized:           return "NotInitialized";
        case ErrorCode::AlreadyInitialized:       return "AlreadyInitialized";
        case ErrorCode::ShuttingDown:             return "ShuttingDown";
        case ErrorCode::Unsupported:              return "Unsupported";
        case ErrorCode::OutOfMemory:              return "OutOfMemory";
        case ErrorCode::ConferenceNotFound:       return "ConferenceNotFound";
        case ErrorCode::ConferenceFull:           return "ConferenceFull";
        case ErrorCode::ConferenceLocked:         return "ConferenceLocked";
        case ErrorCode::ConferencePasswordRequired: return "ConferencePasswordRequired";
        case ErrorCode::ConferencePasswordMismatch: return "ConferencePasswordMismatch";
        case ErrorCode::ConferenceAlreadyExists:  return "ConferenceAlreadyExists";
        case ErrorCode::MemberNotFound:           return "MemberNotFound";
        case ErrorCode::MemberAlreadyExists:      return "MemberAlreadyExists";
        case ErrorCode::MemberBanned:             return "MemberBanned";
        case ErrorCode::MemberRoleNotAllowed:     return "MemberRoleNotAllowed";
        case ErrorCode::MemberMuteNotAllowed:     return "MemberMuteNotAllowed";
        case ErrorCode::MediaTransportError:      return "MediaTransportError";
        case ErrorCode::MediaCodecNotSupported:   return "MediaCodecNotSupported";
        case ErrorCode::MediaBandwidthInsufficient: return "MediaBandwidthInsufficient";
        case ErrorCode::MediaConnectionLost:      return "MediaConnectionLost";
        case ErrorCode::MediaNetworkError:        return "MediaNetworkError";
        case ErrorCode::MediaSendFailed:          return "MediaSendFailed";
        case ErrorCode::MediaReceiveFailed:       return "MediaReceiveFailed";
        case ErrorCode::MediaTransportNotReady:   return "MediaTransportNotReady";
        case ErrorCode::MediaCodecInitFailed:     return "MediaCodecInitFailed";
        case ErrorCode::MediaCodecDecodeFailed:   return "MediaCodecDecodeFailed";
        case ErrorCode::MediaCodecEncodeFailed:   return "MediaCodecEncodeFailed";
        case ErrorCode::MediaBackendNotAvailable: return "MediaBackendNotAvailable";
        case ErrorCode::MediaBackendFatalError:   return "MediaBackendFatalError";
        case ErrorCode::MediaBackendInitFailed:   return "MediaBackendInitFailed";
        case ErrorCode::MediaBackendSpecificError: return "MediaBackendSpecificError";
        case ErrorCode::MediaSsrcPoolExhausted:   return "MediaSsrcPoolExhausted";
        case ErrorCode::MediaSsrcConflict:        return "MediaSsrcConflict";
        case ErrorCode::MediaHandlerNotFound:     return "MediaHandlerNotFound";
        case ErrorCode::MediaHandlerAlreadyExists: return "MediaHandlerAlreadyExists";
        case ErrorCode::MediaHandlerChainBroken:  return "MediaHandlerChainBroken";
        case ErrorCode::MediaMemberDecodeFailure: return "MediaMemberDecodeFailure";
        case ErrorCode::MediaMemberTimeout:       return "MediaMemberTimeout";
        case ErrorCode::MediaMemberDisconnected:  return "MediaMemberDisconnected";
        case ErrorCode::RecordingNotStarted:      return "RecordingNotStarted";
        case ErrorCode::RecordingAlreadyStarted:  return "RecordingAlreadyStarted";
        case ErrorCode::RecordingStorageError:    return "RecordingStorageError";
        case ErrorCode::ConfigInvalid:            return "ConfigInvalid";
        case ErrorCode::ConfigNotFound:           return "ConfigNotFound";
        case ErrorCode::CircuitBreakerOpen:       return "CircuitBreakerOpen";
        case ErrorCode::GracefulShutdownTimeout:  return "GracefulShutdownTimeout";
        case ErrorCode::ResourceLeakDetected:     return "ResourceLeakDetected";
        case ErrorCode::ConferenceStateLost:      return "ConferenceStateLost";
        case ErrorCode::PersistenceFailed:        return "PersistenceFailed";
    }
    return "Unknown";
}

} // namespace vcs
