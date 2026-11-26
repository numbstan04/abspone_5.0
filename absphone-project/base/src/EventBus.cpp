#include "EventBus.h"
#include "ILogger.h"

#include <chrono>
#include <cstdio>
#include <string>

namespace vcs {

namespace {

// A handler that runs longer than this has stopped being "a decision" and
// started being "an execution". The dispatch thread is global, so the cost is
// paid by every other event.
const int64_t kSlowHandlerWarnMs  = 50;
const int64_t kSlowHandlerErrorMs = 500;

int64_t NowUtcMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

int64_t NowSteadyMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// State events (see EventTypes.h). Two consequences, and they must stay
// together or the bus becomes unsafe:
//   1. Sticky: the map is keyed by type, so a state type may only ever describe
//      ONE subject, and a late subscriber gets the latest value replayed.
//   2. Droppable: a state event only carries "the current value", so losing one
//      is recoverable - the next update carries the truth again.
// Fact events (CallIncoming/CallConnected/CallTerminated/SystemError...) are the
// opposite on both counts: never replayed, never dropped.
bool IsStateEvent(EventType type) {
    switch (type) {
    case EventType::RegistrationStateChanged:
    case EventType::MediaStateChanged:
    case EventType::LayoutChanged:
    case EventType::RecordingStateChanged:
    case EventType::H239ChannelChanged:
        return true;
    default:
        return false;
    }
}

} // namespace

bool EventFilter::Matches(const Event& ev) const {
    if (!allowList.empty()) {
        bool allowed = false;
        for (size_t i = 0; i < allowList.size(); ++i) {
            if (allowList[i] == ev.type) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return false;
        }
    }
    if (static_cast<int>(ev.severity) < static_cast<int>(minSeverity)) {
        return false;
    }
    if (!sessionId.empty() && ev.sessionId != sessionId) {
        return false;
    }
    if (!memberId.empty() && ev.memberId != memberId) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- Subscription

Subscription::Subscription(SubscriptionId id, std::weak_ptr<EventBus> bus)
    : id_(id), bus_(bus) {}

Subscription::~Subscription() {
    Reset();
}

Subscription::Subscription(Subscription&& other) noexcept
    : id_(other.id_), bus_(other.bus_) {
    other.id_ = 0;
}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        Reset();
        id_ = other.id_;
        bus_ = other.bus_;
        other.id_ = 0;
    }
    return *this;
}

void Subscription::Reset() {
    if (id_ != 0) {
        std::shared_ptr<EventBus> bus = bus_.lock();
        if (bus) {
            bus->Unsubscribe(id_);
        }
        id_ = 0;
    }
}

// ---------------------------------------------------------------- EventBus

EventBus::EventBus(size_t maxQueueSize)
    : maxQueueSize_(maxQueueSize == 0 ? 4096 : maxQueueSize),
      nextId_(1),
      nextEventId_(1),
      dropped_(0),
      slowHandlerCount_(0),
      lastSlowWarnMs_(0),
      running_(false),
      stopping_(false) {}

EventBus::~EventBus() {
    Stop();
}

void EventBus::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_.load()) {
        return;
    }
    stopping_ = false;
    running_.store(true);
    worker_ = std::thread(&EventBus::DispatchLoop, this);
}

void EventBus::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.load()) {
            return;
        }
        stopping_ = true;
        running_.store(false);
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

Subscription EventBus::Subscribe(const EventFilter& filter, EventHandler handler) {
    if (!handler) {
        return Subscription();
    }

    SubscriptionId id = nextId_++;
    std::shared_ptr<Entry> entry(new Entry());
    entry->id = id;
    entry->filter = filter;
    entry->handler = handler;

    std::vector<Event> replay;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[id] = entry;
        for (std::unordered_map<int, Event>::const_iterator it = sticky_.begin();
             it != sticky_.end(); ++it) {
            if (filter.Matches(it->second)) {
                replay.push_back(it->second);
            }
        }
    }

    // Sticky replay also goes through async dispatch so a subscriber is only
    // ever invoked on the single dispatch thread.
    for (size_t i = 0; i < replay.size(); ++i) {
        Enqueue(replay[i]);
    }

    return Subscription(id, shared_from_this());
}

void EventBus::Unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(id);
}

void EventBus::Publish(Event ev) {
    Enqueue(ev);
}

void EventBus::Enqueue(Event ev) {
    if (ev.timestampMs == 0) {
        ev.timestampMs = NowUtcMs();
    }
    if (ev.steadyMs == 0) {
        ev.steadyMs = NowSteadyMs();
    }
    ev.id = nextEventId_++;
    UpdateSticky(ev);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        if (queue_.size() >= maxQueueSize_) {
            MakeRoomLocked();
        }
        queue_.push_back(ev);
    }
    cv_.notify_one();
}

void EventBus::MakeRoomLocked() {
    // Prefer evicting a state event: it is only a "latest value" carrier, and
    // the next update restores the truth. Scanning from the front keeps the
    // remaining order intact and costs O(depth) - only on overflow.
    for (std::deque<Event>::iterator it = queue_.begin(); it != queue_.end(); ++it) {
        if (!IsStateEvent(it->type)) {
            continue;
        }
        droppedByType_[static_cast<int>(it->type)] += 1;
        dropped_.fetch_add(1);
        queue_.erase(it);
        return;
    }

    // Every queued event is a fact: the pipeline is already pathological.
    // Dropping one breaks the call state machine downstream, so make it loud
    // instead of silently losing a transition.
    const EventType victim = queue_.front().type;
    droppedByType_[static_cast<int>(victim)] += 1;
    dropped_.fetch_add(1);
    queue_.pop_front();

    const int64_t now = NowSteadyMs();
    if (now - lastFactDropWarnMs_ >= 1000) {
        lastFactDropWarnMs_ = now;
        VCS_LOG_ERROR_L(vcs::base::LogModule::Base,
            "[EventBus] fact event dropped under pressure: type=%s depth=%zu "
            "totalDropped=%llu - call state machine may be corrupted",
            EventTypeHelper::ToString(victim),
            queue_.size(),
            static_cast<unsigned long long>(dropped_.load()));
    }
}

uint64_t EventBus::DroppedCount(EventType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<int, uint64_t>::const_iterator it =
        droppedByType_.find(static_cast<int>(type));
    return it == droppedByType_.end() ? 0 : it->second;
}

void EventBus::UpdateSticky(const Event& ev) {
    if (!IsStateEvent(ev.type)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    sticky_[static_cast<int>(ev.type)] = ev;
}

Event EventBus::GetLastSticky(EventType type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<int, Event>::const_iterator it = sticky_.find(static_cast<int>(type));
    return (it == sticky_.end()) ? Event() : it->second;
}

size_t EventBus::QueueDepth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool EventBus::MatchesFilter(const EventFilter& filter, const Event& ev) const {
    return filter.Matches(ev);
}

void EventBus::DispatchLoop() {
    for (;;) {
        Event ev;
        std::vector<std::shared_ptr<Entry> > handlers;
        bool hasEvent = false;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });

            if (queue_.empty()) {
                if (stopping_) {
                    break;
                }
                continue;
            }

            ev = queue_.front();
            queue_.pop_front();
            hasEvent = true;

            handlers.reserve(entries_.size());
            for (std::unordered_map<SubscriptionId, std::shared_ptr<Entry> >::const_iterator it = entries_.begin();
                 it != entries_.end(); ++it) {
                handlers.push_back(it->second);
            }
        }

        if (!hasEvent) {
            continue;
        }

        // Dispatch outside the lock: subscriber code never runs under the bus lock.
        for (size_t i = 0; i < handlers.size(); ++i) {
            const std::shared_ptr<Entry>& entry = handlers[i];
            if (!MatchesFilter(entry->filter, ev)) {
                continue;
            }
            const std::chrono::steady_clock::time_point began =
                std::chrono::steady_clock::now();
            try {
                entry->handler(ev);
            } catch (...) {
                Event err;
                err.type = EventType::SystemError;
                err.severity = EventSeverity::Error;
                err.source = EventSource::Base;
                err.errorCode = 0;
                err.sessionId = ev.sessionId;
                err.traceId = ev.traceId;
                err.payload = "{\"reason\":\"subscriber threw\"}";
                Enqueue(err);
            }
            const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began).count();
            ReportHandlerDuration(ev, elapsedMs);
        }
    }
}

void EventBus::ReportHandlerDuration(const Event& ev, int64_t elapsedMs) {
    if (elapsedMs < kSlowHandlerWarnMs) {
        return;
    }
    slowHandlerCount_.fetch_add(1);

    // Rate-limit the log line only; the counter above is exact.
    const int64_t now = NowSteadyMs();
    int64_t last = lastSlowWarnMs_.load();
    if (now - last >= 1000 && lastSlowWarnMs_.compare_exchange_strong(last, now)) {
        VCS_LOG_WARNING_L(vcs::base::LogModule::Base,
            "[EventBus] slow handler on the dispatch thread: type=%s took %lld ms "
            "(budget %lld ms) - work of this size belongs on an executor",
            EventTypeHelper::ToString(ev.type),
            static_cast<long long>(elapsedMs),
            static_cast<long long>(kSlowHandlerWarnMs));
    }

    // A handler that blows through the hard budget is a design violation, not a
    // slow log line: surface it where operators already look.
    if (elapsedMs < kSlowHandlerErrorMs || ev.type == EventType::SystemError) {
        return;
    }
    char payload[256];
    std::snprintf(payload, sizeof(payload),
        "{\"reason\":\"slow subscriber\",\"eventType\":\"%s\",\"elapsedMs\":%lld}",
        EventTypeHelper::ToString(ev.type),
        static_cast<long long>(elapsedMs));
    Event err;
    err.type = EventType::SystemError;
    err.severity = EventSeverity::Error;
    err.source = EventSource::Base;
    err.errorCode = 0;
    err.sessionId = ev.sessionId;
    err.conferenceId = ev.conferenceId;
    err.traceId = ev.traceId;
    err.payload = payload;
    Enqueue(err);
}

} // namespace vcs
