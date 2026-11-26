/**
 * @file BackendFactory.h
 * @brief Factory for IRtpBackend implementations (Q5)
 *
 * [Role]
 * BackendFactory is the *only* supported way to construct an
 * IRtpBackend. RTPEngine takes a `unique_ptr<IRtpBackend>` by injection
 *  -  it never calls `new NativeRtpBackend()` itself. This keeps the
 * protocol choice at the top of the dependency graph (the application
 * binary that wires MediaControl together), where it belongs.
 *
 * [Two access patterns]
 * 1. Free function `CreateDefaultBackend()`  -  the 90% case. Returns the
 *    compile-time default backend (NativeRtp in the lite artifact,
 *    WebRtc in the standard artifact) wrapped in a unique_ptr. Used in
 *    tests and in production code that has no reason to think about
 *    which backend it gets.
 * 2. `BackendFactory` class with static methods  -  used when the caller
 *    needs to pick a specific backend type. The class also exposes a
 *    tiny registration hook so future backends can be plugged in
 *    without modifying this header.
 *
 * [Single construction point]
 * This factory is the ONLY supported way to construct an IRtpBackend in
 * production. Direct `new NativeRtpBackend()` / `new WebRtcRtpBackend()`
 * outside this translation unit is a contract violation (docs/12 §15
 * P0-5): it would bypass the artifact switch and make the running
 * backend impossible to identify from the startup log.
 *
 * [Extending the factory]
 * To add a new backend type:
 *   1. Implement `IRtpBackend` in a separate build target.
 *   2. Add a value to `BackendType`.
 *   3. Add an entry to the built-in table in BackendFactory.cpp.
 * That's it. No changes to IRtpBackend, no changes to RTPEngine, no
 * changes to MediaControl.
 *
 * [References]
 * - Section 20.4  (Q5 architecture: factory inverts the dependency)
 * - Section 20.6  (Q5 RTPEngine constructor takes injected backend)
 * - Section 20.7  (Q5 NativeRtpBackend is the default)
 */

#pragma once

#include "IRtpBackend.h"
#include "media_export.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vcs { namespace media {

// ============================================================
// BackendType enum
// ============================================================

/**
 * @brief Identifies a concrete IRtpBackend implementation.
 *
 * New values are appended at the end to preserve ABI.
 */
enum class BackendType : uint8_t {
    kDefault   = 0,   // the compile-time default (VCS_WITH_WEBRTC decides)
    kNativeRtp = 1,   // RFC 3550 RTP over UDP
    kWebRtc    = 10,  // libwebrtc backend; only built in the standard artifact
    // --- reserved for future backends ---
    // kSrt    = 20,   // Secure Reliable Transport
    // kQuic   = 30,   // QUIC-based media
};

// ============================================================
// Free function
// ============================================================

/**
 * @brief Create the compile-time default backend.
 *
 * Equivalent to `BackendFactory::Create(BackendType::kDefault)`: NativeRtp
 * in the lite artifact (`VCS_WITH_WEBRTC=OFF`, the default) and WebRtc in
 * the standard artifact (`VCS_WITH_WEBRTC=ON`). The most commonly used
 * factory entry point. Always returns a valid pointer  -  failure modes
 * today are OOM, which terminates the process; once the factory is
 * exposed across module boundaries we may revisit this and return
 * nullptr instead.
 */
VCS_MEDIA_API std::unique_ptr<IRtpBackend> CreateDefaultBackend();

// ============================================================
// BackendFactory class
// ============================================================

/**
 * @brief Static factory + lightweight plugin registry for IRtpBackend.
 *
 * The static methods are thread-safe. The optional registration hook
 * is *not* thread-safe  -  register backends at program startup, before
 * any concurrent traffic.
 */
class VCS_MEDIA_API BackendFactory {
public:
    /// Signature of a backend constructor. Implementations may capture
    /// configuration from the surrounding environment; today none do.
    using Creator = std::function<std::unique_ptr<IRtpBackend>()>;

    // ---- Type-based creation ------------------------------------------

    /// Create a backend of `type`. Returns nullptr for unknown types.
    static std::unique_ptr<IRtpBackend> Create(BackendType type);

    /// Create a backend by string name (case-insensitive). "default"
    /// follows the artifact; "native" / "NativeRtp" always mean
    /// NativeRtp; other names resolve through the built-in table and
    /// the plugin registry. Returns nullptr for unknown names.
    static std::unique_ptr<IRtpBackend> Create(const std::string& name);

    // ---- Introspection ------------------------------------------------

    /// Names of every backend type Create() currently understands. The
    /// order matches the enum definition (stable).
    static std::vector<std::string> SupportedTypes();

    /// Map a BackendType to its canonical name. Returns "unknown" for
    /// values the factory does not recognize.
    static std::string TypeName(BackendType type);

    /// Inverse of TypeName. Returns BackendType::kDefault for unknown
    /// names (callers that care can check SupportedTypes() first).
    static BackendType ParseType(const std::string& name);

    // ---- Plugin registration (future backends) -----------------------

    /**
     * @brief Register a custom backend type.
     *
     * @param name      Stable identifier. The reserved names
     *                  ("default", "native", "nativerpt") cannot be
     *                  overwritten  -  attempting to do so returns false
     *                  and leaves the factory unchanged.
     * @param type      Enum value to bind this creator to. Must not
     *                  collide with an already-known type.
     * @param creator   Factory function.
     * @return true on success, false if any argument is invalid.
     *
     * Intended for shared-library backend plugins loaded at startup.
     * Not thread-safe.
     */
    static bool Register(const std::string& name,
                         BackendType type,
                         Creator creator);

    /// Drop a previously-registered custom backend. The reserved names
    /// cannot be removed. Returns true if something was removed.
    static bool Unregister(BackendType type);
};

// ============================================================
// Note: CreateDefaultBackend() is declared above and defined in the
// matching .cpp. Do NOT inline it here  -  the .cpp pulls in concrete
// backend headers (NativeRtpBackend.h, ...) which the .h must stay
// free of.
// ============================================================

} // namespace media
} // namespace vcs
