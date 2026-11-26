/**
 * @file BackendFactory.cpp
 * @brief Factory implementation for IRtpBackend (Q5)
 *
 * The .cpp is the *only* place that links against concrete backends.
 * BackendFactory.h stays free of concrete-type includes so other
 * translation units (RTPEngine, MediaControl, ...) can be compiled
 * without pulling in NativeRtpBackend's definition.
 *
 * The static registry is protected by a one-shot init flag rather
 * than a mutex: register at program startup, before concurrent
 * traffic begins, and treat the table as immutable thereafter.
 */

#include "BackendFactory.h"

#include "NativeRtpBackend.h"   // concrete backend
#ifdef VCS_WITH_WEBRTC
#include "webrtc/WebRtcRtpBackend.h"   // standard artifact, provided by media/src/backend/webrtc
#endif

#include <algorithm>
#include <cctype>
#include <mutex>
#include <unordered_map>

namespace vcs { namespace media {

namespace {

// One-shot init guard. `std::call_once` is overkill for a static
// initializer that runs at program startup; we use a plain bool.
bool g_initialized = false;

// Registry of custom (non-built-in) backends, keyed by enum value.
struct RegistryEntry {
    std::string  name;
    BackendFactory::Creator creator;
};
std::unordered_map<BackendType, RegistryEntry>& Registry() {
    static std::unordered_map<BackendType, RegistryEntry> r;
    return r;
}

// Case-insensitive ASCII equality. Used for ParseType.
bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Built-in creators. Each is a thin lambda so the table can be
// populated without an extra layer of free functions.
std::unique_ptr<IRtpBackend> CreateNativeRtp() {
    return std::unique_ptr<IRtpBackend>(new NativeRtpBackend());
}

#ifdef VCS_WITH_WEBRTC
std::unique_ptr<IRtpBackend> CreateWebRtc() {
    return std::unique_ptr<IRtpBackend>(new WebRtcRtpBackend());
}
#endif

// One-shot table of built-in (BackendType -> creator + name). Built
// lazily on first access so the order of static initialization
// across translation units does not matter.
const std::unordered_map<BackendType, RegistryEntry>& Builtins() {
    static const std::unordered_map<BackendType, RegistryEntry> table = {
        // The name MUST match the value returned by the concrete
        // GetBackendType() so that callers can do Create(backend->GetBackendType()).
        { BackendType::kNativeRtp, { "NativeRtp", &CreateNativeRtp } },
#ifdef VCS_WITH_WEBRTC
        { BackendType::kWebRtc, { "WebRtc", &CreateWebRtc } },
#endif
    };
    return table;
}

// "default" means "whatever this artifact ships as its backend"; it is
// resolved by ResolveDefaultType(), not by the Builtins() table.
constexpr const char* kDefaultName = "default";

} // namespace

// ----------------------------------------------------------------
// Free function
// ----------------------------------------------------------------

std::unique_ptr<IRtpBackend> CreateDefaultBackend() {
    return BackendFactory::Create(BackendType::kDefault);
}

// kDefault is resolved here, not at the call site: this is the single
// assembly point for the artifact decision (docs/12 §9/§15 P0-2).
static BackendType ResolveDefaultType() {
#ifdef VCS_WITH_WEBRTC
    return BackendType::kWebRtc;
#else
    return BackendType::kNativeRtp;
#endif
}

// ----------------------------------------------------------------
// BackendFactory
// ----------------------------------------------------------------

std::unique_ptr<IRtpBackend> BackendFactory::Create(BackendType type) {
    if (type == BackendType::kDefault) {
        type = ResolveDefaultType();
    }
    // 1. Built-in creators win. This guarantees that a misbehaving
    //    Register() call cannot shadow a shipped backend.
    auto bit = Builtins().find(type);
    if (bit != Builtins().end() && bit->second.creator) {
        return bit->second.creator();
    }
    // 2. Plugin registry.
    auto rit = Registry().find(type);
    if (rit != Registry().end() && rit->second.creator) {
        return rit->second.creator();
    }
    return nullptr;
}

std::unique_ptr<IRtpBackend> BackendFactory::Create(const std::string& name) {
    // "default" follows the artifact; "native" / "NativeRtp" always mean the
    // NativeRtp backend, including in the standard artifact (diagnostic path).
    if (IEquals(name, kDefaultName)) {
        return Create(BackendType::kDefault);
    }
    if (IEquals(name, "native") || IEquals(name, "NativeRtp")) {
        return Create(BackendType::kNativeRtp);
    }
    for (const auto& kv : Builtins()) {
        if (IEquals(kv.second.name, name)) return Create(kv.first);
    }
    for (const auto& kv : Registry()) {
        if (IEquals(kv.second.name, name)) return Create(kv.first);
    }
    return nullptr;
}

std::vector<std::string> BackendFactory::SupportedTypes() {
    std::vector<std::string> out;
    out.reserve(Builtins().size() + Registry().size() + 1);
    out.emplace_back(kDefaultName);
    for (const auto& kv : Builtins()) {
        out.push_back(kv.second.name);
    }
    for (const auto& kv : Registry()) {
        out.push_back(kv.second.name);
    }
    return out;
}

std::string BackendFactory::TypeName(BackendType type) {
    if (type == BackendType::kDefault) return kDefaultName;
    auto bit = Builtins().find(type);
    if (bit != Builtins().end()) return bit->second.name;
    auto rit = Registry().find(type);
    if (rit != Registry().end()) return rit->second.name;
    return "unknown";
}

BackendType BackendFactory::ParseType(const std::string& name) {
    // "default" follows the artifact; "native" is an alias for the
    // NativeRtp backend and must NOT resolve to kDefault in the standard
    // artifact. "NativeRtp" round-trips through the Builtins() loop below.
    if (IEquals(name, kDefaultName)) {
        return BackendType::kDefault;
    }
    if (IEquals(name, "native")) {
        return BackendType::kNativeRtp;
    }
    for (const auto& kv : Builtins()) {
        if (IEquals(kv.second.name, name)) return kv.first;
    }
    for (const auto& kv : Registry()) {
        if (IEquals(kv.second.name, name)) return kv.first;
    }
    return BackendType::kDefault;   // unknown -> safe default
}

bool BackendFactory::Register(const std::string& name,
                              BackendType type,
                              Creator creator) {
    if (name.empty() || !creator)              return false;
    if (IEquals(name, kDefaultName))           return false;   // reserved
    if (IEquals(name, "native"))               return false;
    if (IEquals(name, "NativeRtp"))            return false;
    if (Builtins().count(type))                return false;   // reserved
    if (Registry().count(type))                return false;   // already taken
    Registry().emplace(type, RegistryEntry{ name, std::move(creator) });
    return true;
}

bool BackendFactory::Unregister(BackendType type) {
    auto bit = Builtins().find(type);
    if (bit != Builtins().end()) return false;   // cannot unregister builtins
    return Registry().erase(type) > 0;
}

} // namespace media
} // namespace vcs
