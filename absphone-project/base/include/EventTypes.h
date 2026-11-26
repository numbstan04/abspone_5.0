#pragma once

// Platform events: the single source of event types for the whole framework.
// See docs/02-event-and-callback-model.md.
// Rules:
//   1. Events describe facts that already happened; queries/commands never use events.
//   2. Values are segmented; business values must not enter the 1000+ custom range.
//   3. payload schema is fixed per type (JSON object).
//   4. Register new events in the catalog first.

#include "vcs_export.h"

#include <string>
#include <cstdint>
#include <unordered_map>
#include <mutex>

namespace vcs {

// Event catalog (see docs/06-设计基线.md §5.2 / §9.1).
//
// Two kinds of events, and the distinction decides stickiness:
//   - State event (状态事件): describes the current value of ONE subject
//     (registration, media, layout, ...). Sticky: a late subscriber gets the
//     latest value replayed. Every subject must have at most one state event
//     type, because the sticky map is keyed by type only.
//   - Fact event (事实事件): a one-shot occurrence that must never be replayed
//     to a late subscriber. Not sticky.
//
// Events travel upward / across layers only. Commands and queries are not
// events: they are interface calls.
enum class EventType : int {
    Unknown = 0,

    // Terminal / system (1-99) - facts
    TerminalInitialized = 1,
    TerminalReleased    = 2,
    SystemError         = 50,
    SystemWarning       = 51,

    // Registration / link (100-199)
    RegistrationStateChanged = 100,   // state, sticky (gateway)
    NetworkChanged           = 110,   // fact

    // Call / session (200-299) - facts, single producer: SessionManager
    CallIncoming   = 200,
    CallConnected  = 201,
    CallTerminated = 202,
    CallOutgoing   = 203,   // 呼出已由协议栈受理（发起期状态回调，非"每个迁移"）

    // Media / device (400-499)
    MediaStateChanged       = 400,    // state, sticky (media)
    MediaStreamStateChanged = 402,    // fact (per-stream up/down)
    LayoutChanged           = 420,    // state, sticky (conference)

    // Recording / dual stream (500-599)
    RecordingStateChanged = 500,      // state, sticky
    H239ChannelChanged    = 510,      // state, sticky

    // Start of the custom range (>=1000, registered by EventTypeHelper)
    CustomEventBase = 1000
};

enum class EventSeverity : int {
    Info = 0,
    Warning = 1,
    Error = 2,
    Fatal = 3
};

enum class EventSource : int {
    Unknown = 0,
    Base,
    Common,
    Media,
    Gateway,
    Conference,
    AI,
    AbsPhone
};

inline const char* EventSeverityToString(EventSeverity v) {
    switch (v) {
    case EventSeverity::Info:    return "info";
    case EventSeverity::Warning: return "warning";
    case EventSeverity::Error:   return "error";
    case EventSeverity::Fatal:   return "fatal";
    default:                     return "unknown";
    }
}

inline const char* EventSourceToString(EventSource v) {
    switch (v) {
    case EventSource::Base:       return "base";
    case EventSource::Common:     return "common";
    case EventSource::Media:      return "media";
    case EventSource::Gateway:    return "gateway";
    case EventSource::Conference: return "conference";
    case EventSource::AI:         return "ai";
    case EventSource::AbsPhone:   return "absphone";
    default:                      return "unknown";
    }
}

// Platform event. Immutable after publication; subscribers read by value or const&.
struct Event {
    uint64_t      id = 0;          // assigned by the bus, globally monotonic
    EventType     type = EventType::Unknown;
    EventSeverity severity = EventSeverity::Info;
    EventSource   source = EventSource::Unknown;
    std::string   sessionId;       // terminal session key (calls and media share it)
    std::string   conferenceId;
    std::string   memberId;
    std::string   traceId;         // spans one request and its follow-up events
    int           errorCode = 0;   // vcs::ErrorCode; 0 means no error
    std::string   payload;         // JSON object, schema fixed per type
    int64_t       timestampMs = 0; // UTC milliseconds
    int64_t       steadyMs = 0;    // monotonic clock milliseconds
};

// Event type helpers: stringify, reverse lookup, custom registration.
class VCS_BASE_API EventTypeHelper {
public:
    static const char* ToString(EventType type);
    static EventType FromString(const std::string& name);
    static bool IsPredefined(EventType type);
    static bool IsCustom(EventType type);
    static int ToInt(EventType type);
    static EventType FromInt(int value);

    // Register a custom event (thread safe). Re-registering a name returns the
    // existing value; an invalid name or an exhausted value range returns Unknown.
    static EventType RegisterCustom(const std::string& name);

    static void GetRegisteredTypes(std::unordered_map<std::string, int>& types);

private:
    static void EnsureInitialized();
    static bool initialized_;
    static std::unordered_map<std::string, EventType> stringToEnum_;
    static std::unordered_map<EventType, std::string> enumToString_;
    static int nextCustomValue_;
    static std::unordered_map<std::string, int> customTypes_;
    static std::mutex mutex_;
};

} // namespace vcs
