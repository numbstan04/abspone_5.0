#pragma once

// Unified event bus (vcs::IEventBus / EventBus).
// Contract (docs/02-event-and-callback-model.md):
//   1. Publish only enqueues; subscribers never run on the publisher thread.
//   2. Single dispatch thread; order equals enqueue order.
//   3. Bounded queue. When full the bus evicts the OLDEST STATE event; a FACT
//      event is never evicted while any state event is still queued. State is
//      a "latest value" carrier and is safe to lose; a fact (CallConnected,
//      CallTerminated, SystemError...) is a one-shot occurrence, and losing it
//      corrupts the call state machine. Only when the whole queue consists of
//      facts does the bus drop the oldest one, and it reports that as ERROR.
//      Drops are counted per type (DroppedCount(type)) - never silently.
//   4. Dispatch happens outside the lock; subscriber exceptions are isolated
//      and converted into a SystemError event.
//   5. Sticky events keep a last value per type; new subscribers get a replay.
//   6. Subscription is an RAII handle; destruction unsubscribes automatically.
//   7. A handler runs ON the single dispatch thread, so it must be short and
//      non-blocking: no cross-module locks, no join, no file IO, no waiting on
//      conditions. The bus measures every handler and reports a violation
//      (SystemError + SlowHandlerCount) instead of trusting reviewers to catch
//      it. Long work belongs on an executor, not here.

#include "vcs_export.h"
#include "EventTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vcs {

typedef uint64_t SubscriptionId;
typedef std::function<void(const Event&)> EventHandler;

struct EventFilter {
    std::vector<EventType> allowList;             // empty = all types
    EventSeverity minSeverity = EventSeverity::Info;
    std::string sessionId;                        // empty = any session
    std::string memberId;                         // empty = any member

    bool Matches(const Event& ev) const;
};

class EventBus;

// RAII subscription handle: destruction unsubscribes. Movable, not copyable.
class VCS_BASE_API Subscription {
public:
    Subscription() = default;
    Subscription(SubscriptionId id, std::weak_ptr<EventBus> bus);
    ~Subscription();

    Subscription(Subscription&& other) noexcept;
    Subscription& operator=(Subscription&& other) noexcept;

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void Reset();
    SubscriptionId Id() const { return id_; }
    bool Valid() const { return id_ != 0; }

private:
    SubscriptionId id_ = 0;
    std::weak_ptr<EventBus> bus_;
};

class VCS_BASE_API IEventBus {
public:
    virtual ~IEventBus() = default;

    virtual void Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;

    virtual Subscription Subscribe(const EventFilter& filter, EventHandler handler) = 0;
    virtual void Unsubscribe(SubscriptionId id) = 0;

    virtual void Publish(Event ev) = 0;

    virtual Event GetLastSticky(EventType type) const = 0;
    virtual size_t QueueDepth() const = 0;
    virtual uint64_t DroppedCount() const = 0;

    // Drops attributed to one event type. A non-zero value for a fact type is
    // an incident: the state machine below may have missed a transition.
    virtual uint64_t DroppedCount(EventType type) const = 0;

    // Number of handler calls that exceeded the slow-handler budget. Kept as a
    // permanent, queryable signal so "someone did long work on the dispatch
    // thread" cannot disappear into the logs.
    virtual uint64_t SlowHandlerCount() const = 0;
};

class VCS_BASE_API EventBus : public IEventBus,
                              public std::enable_shared_from_this<EventBus> {
public:
    explicit EventBus(size_t maxQueueSize = 4096);
    ~EventBus() override;

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    void Start() override;
    void Stop() override;
    bool IsRunning() const override { return running_.load(); }

    Subscription Subscribe(const EventFilter& filter, EventHandler handler) override;
    void Unsubscribe(SubscriptionId id) override;

    void Publish(Event ev) override;

    Event GetLastSticky(EventType type) const override;
    size_t QueueDepth() const override;
    uint64_t DroppedCount() const override { return dropped_.load(); }
    uint64_t DroppedCount(EventType type) const override;
    uint64_t SlowHandlerCount() const override { return slowHandlerCount_.load(); }

private:
    struct Entry {
        SubscriptionId id;
        EventFilter filter;
        EventHandler handler;
    };

    void DispatchLoop();
    void Enqueue(Event ev);
    bool MatchesFilter(const EventFilter& filter, const Event& ev) const;
    void UpdateSticky(const Event& ev);

    // Caller holds mutex_. Frees exactly one slot according to the drop policy.
    void MakeRoomLocked();
    // Called on the dispatch thread after each handler returns.
    void ReportHandlerDuration(const Event& ev, int64_t elapsedMs);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<SubscriptionId, std::shared_ptr<Entry> > entries_;
    std::unordered_map<int, Event> sticky_;
    std::deque<Event> queue_;
    size_t maxQueueSize_;
    std::atomic<SubscriptionId> nextId_;
    std::atomic<uint64_t> nextEventId_;
    std::atomic<uint64_t> dropped_;
    std::unordered_map<int, uint64_t> droppedByType_;  // guarded by mutex_
    int64_t lastFactDropWarnMs_ = 0;                   // guarded by mutex_
    std::atomic<uint64_t> slowHandlerCount_;
    std::atomic<int64_t> lastSlowWarnMs_;
    std::atomic<bool> running_;
    bool stopping_;
    std::thread worker_;
};

} // namespace vcs
