#include "EventTypes.h"

#include <algorithm>

namespace vcs {

bool EventTypeHelper::initialized_ = false;
std::unordered_map<std::string, EventType> EventTypeHelper::stringToEnum_;
std::unordered_map<EventType, std::string> EventTypeHelper::enumToString_;
int EventTypeHelper::nextCustomValue_ = static_cast<int>(EventType::CustomEventBase);
std::unordered_map<std::string, int> EventTypeHelper::customTypes_;
std::mutex EventTypeHelper::mutex_;

namespace {

struct TypeEntry {
    EventType type;
    const char* name;
};

const TypeEntry kPredefined[] = {
    { EventType::Unknown,                 "Unknown" },
    { EventType::TerminalInitialized,     "TerminalInitialized" },
    { EventType::TerminalReleased,        "TerminalReleased" },
    { EventType::SystemError,             "SystemError" },
    { EventType::SystemWarning,           "SystemWarning" },
    { EventType::RegistrationStateChanged,"RegistrationStateChanged" },
    { EventType::NetworkChanged,          "NetworkChanged" },
    { EventType::CallIncoming,            "CallIncoming" },
    { EventType::CallConnected,           "CallConnected" },
    { EventType::CallTerminated,          "CallTerminated" },
    { EventType::CallOutgoing,            "CallOutgoing" },
    { EventType::MediaStateChanged,       "MediaStateChanged" },
    { EventType::MediaStreamStateChanged, "MediaStreamStateChanged" },
    { EventType::LayoutChanged,           "LayoutChanged" },
    { EventType::RecordingStateChanged,   "RecordingStateChanged" },
    { EventType::H239ChannelChanged,      "H239ChannelChanged" },
    { EventType::CustomEventBase,         "CustomEventBase" },
};

const size_t kPredefinedCount = sizeof(kPredefined) / sizeof(kPredefined[0]);

} // namespace

void EventTypeHelper::EnsureInitialized() {
    if (initialized_) {
        return;
    }
    for (size_t i = 0; i < kPredefinedCount; ++i) {
        stringToEnum_[kPredefined[i].name] = kPredefined[i].type;
        enumToString_[kPredefined[i].type] = kPredefined[i].name;
    }
    initialized_ = true;
}

const char* EventTypeHelper::ToString(EventType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitialized();
    std::unordered_map<EventType, std::string>::const_iterator it = enumToString_.find(type);
    return (it == enumToString_.end()) ? "Unknown" : it->second.c_str();
}

EventType EventTypeHelper::FromString(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitialized();
    std::unordered_map<std::string, EventType>::const_iterator it = stringToEnum_.find(name);
    return (it == stringToEnum_.end()) ? EventType::Unknown : it->second;
}

bool EventTypeHelper::IsPredefined(EventType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitialized();
    std::unordered_map<EventType, std::string>::const_iterator it = enumToString_.find(type);
    return it != enumToString_.end() && static_cast<int>(type) < static_cast<int>(EventType::CustomEventBase);
}

bool EventTypeHelper::IsCustom(EventType type) {
    return static_cast<int>(type) >= static_cast<int>(EventType::CustomEventBase);
}

int EventTypeHelper::ToInt(EventType type) {
    return static_cast<int>(type);
}

EventType EventTypeHelper::FromInt(int value) {
    return static_cast<EventType>(value);
}

EventType EventTypeHelper::RegisterCustom(const std::string& name) {
    if (name.empty()) {
        return EventType::Unknown;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitialized();

    std::unordered_map<std::string, int>::const_iterator existing = customTypes_.find(name);
    if (existing != customTypes_.end()) {
        return static_cast<EventType>(existing->second);
    }
    if (stringToEnum_.find(name) != stringToEnum_.end()) {
        return stringToEnum_[name];
    }

    const int value = nextCustomValue_++;
    const EventType type = static_cast<EventType>(value);
    customTypes_[name] = value;
    stringToEnum_[name] = type;
    enumToString_[type] = name;
    return type;
}

void EventTypeHelper::GetRegisteredTypes(std::unordered_map<std::string, int>& types) {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureInitialized();
    types.clear();
    for (std::unordered_map<std::string, EventType>::const_iterator it = stringToEnum_.begin();
         it != stringToEnum_.end(); ++it) {
        types[it->first] = static_cast<int>(it->second);
    }
}

} // namespace vcs
