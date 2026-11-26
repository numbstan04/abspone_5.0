/**
 * @file VideoFrameQueue.cpp
 * @brief Video frame queue implementation
 *
 * :
 *
 * 1. Ref count leak: Call ReleaseRef() in EncodedVideoFrame destructor
 * 2. Pop race condition (P0): AddRef first to protect node, then advance head
 * 3. Producer write position offset: Return currentTail when queue not full
 * 4. Overwrite old frame without advancing head: Sync head pointer during overwrite
 * 5. Large frame dynamic allocation: Use malloc when exceeding pre-allocated buffer
 * 6. Large frame double free: Transfer dynamic data ownership to frame object during Pop
 * 7. Clear safety issue: Wait for refCount to zero before cleanup, abandon dynamic data with refs
 * 8. PeekSafe/Peek dangerous interface: Removed from Header
 * 9. Thread-local cache dangling pointer: Removed, use pure read-write lock instead
 *
 * Design principles (SPSC):
 * - Producer (RTP thread) single-threaded write
 * - Consumer (decode thread) single-threaded read
 * - head advanced by consumer, tail advanced by producer
 * - Reference count ensures data not overwritten during consumer usage
 *
 * [P0 Race Window Fix Principle]:
 * Pop order must be: AddRef -> head++ -> count--
 * During AddRef and head++, producer sees refCount>0 and won't overwrite node.
 *
 * [Large Frame Double Free Fix Principle]:
 * During Pop, set node.dynamicData to nullptr, transfer ownership completely to frame object,
 * avoid double free of same memory during frame destruction and node overwrite.
 */

#include "VideoFrameQueue.h"
#include "ILogger.h"
#include <cstring>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <climits>
#include <thread>
#include <chrono>
#include <mutex>

namespace vcs {
	namespace media {

		// ============================================================
		// VideoFrameQueue::Impl definition
		// ============================================================
		struct VideoFrameQueue::Impl {
			// Pre-allocated frame node array (using unique_ptr to avoid atomic move issues)
			std::vector<std::unique_ptr<PreAllocatedFrameNode>> nodes_;

			// Queue properties
			size_t capacity_;
			size_t preAllocSize_;

			//  Lock-free SPSC indices
			// Producer (RTP thread) only writes tail_
			// Consumer (decode thread) only reads/writes head_
			std::atomic<size_t> head_{ 0 };      // Consumer read position
			std::atomic<size_t> tail_{ 0 };      // Producer write position

			//  Frame count (atomic operation)
			std::atomic<size_t> count_{ 0 };

			// Key frame cache (last key frame, for quick request)
			struct KeyFrameCache {
				std::vector<uint8_t> data;  // Key frame data stored independently
				size_t size = 0;
				uint64_t timestamp = 0;
				uint32_t width = 0;
				uint32_t height = 0;
				bool isH239 = false;
				EncodedVideoFrame::CodecType codecType = EncodedVideoFrame::CodecType::H264;
			};
			std::unique_ptr<KeyFrameCache> keyFrameCache_;
			std::mutex keyFrameMutex_;  // Exclusive access to key frame cache

			// Statistics
			std::atomic<uint64_t> droppedFrames_{ 0 };
			std::atomic<uint64_t> largeFrameRejects_{ 0 };
			std::atomic<uint64_t> dynamicAllocCount_{ 0 };

			// Member ID
			std::string memberId_;

			std::atomic<bool> deprecated_{ false };  // Queue deprecated flag

			Impl(size_t capacity, size_t preAllocSize)
				: capacity_(capacity),
				preAllocSize_(preAllocSize) {
				nodes_.reserve(capacity);
				for (size_t i = 0; i < capacity; ++i) {
					nodes_.push_back(std::unique_ptr<PreAllocatedFrameNode>(
						new PreAllocatedFrameNode(preAllocSize)));
				}
			}

			//  Lock-free check
			bool IsFull() const {
				return count_.load(std::memory_order_acquire) >= capacity_;
			}

			bool IsEmpty() const {
				return count_.load(std::memory_order_acquire) == 0;
			}

			size_t NextPos(size_t pos) const {
				return (pos + 1) % capacity_;
			}

			//  Safely get writable node index
			// Returns writable node index, -1 if no available node
			//
			// [Bug 1 Fix] When queue not full, should return currentTail, not NextPos(currentTail)
			// tail_ itself points to next writable position, producer should write directly to that position
			//
			// [Bug 2 Fix] When overwriting old frame, must advance head_
			// Otherwise head_ is inconsistent with actual read position, queue state is corrupted
			//
			// [Fix] Don't increase count_ when overwriting, because it's replacement not addition
			int GetWritableNodeIndex(bool& isOverwrite) {
				isOverwrite = false;

				if (deprecated_.load(std::memory_order_acquire)) {
					return -1;  // Deprecated, reject enqueue
				}

				size_t currentTail = tail_.load(std::memory_order_acquire);
				size_t currentCount = count_.load(std::memory_order_acquire);

				// Queue full: check if oldest node can be overwritten
				if (currentCount >= capacity_) {
					size_t oldestPos = head_.load(std::memory_order_acquire);
					PreAllocatedFrameNode& oldestNode = *nodes_[oldestPos];

					//  Only overwrite when no references
					if (oldestNode.GetRefCount() > 0) {
						return -1;  // Oldest frame in use, cannot overwrite
					}

					// Mark as dropped
					droppedFrames_.fetch_add(1, std::memory_order_relaxed);
					VCS_MEDIA_LOG_TRACE("[Queue] Queue full, dropping frame in use, total dropped=%llu",
						(unsigned long long)droppedFrames_.load());

					// [Bug 2 Fix] Must advance head_ when overwriting old frame
					// This way head_ always points to next readable position
					size_t newHead = NextPos(oldestPos);
					head_.store(newHead, std::memory_order_release);
					VCS_MEDIA_LOG_TRACE("[Queue] Overwrite oldest frame, advanced head from %zu to %zu",
						oldestPos, newHead);

					// [Fix] Mark as overwrite, so WriteToNodeInternal won't increase count_
					isOverwrite = true;

					// Can overwrite oldest node
					return static_cast<int>(oldestPos);
				}

				// [Bug 1 Fix] When queue not full, should return currentTail
				// tail_ itself points to next writable position, use directly
				return static_cast<int>(currentTail);
			}

			//  Write node data
			//
			// [Large frame fix] Support dynamic allocation:
			// - <= preAllocSize_: use pre-allocated buffer
			// - > preAllocSize_: use dynamic allocation
			//
			// [Fix] Don't increase count_ when isOverwrite=true, because it's replacement not addition
			bool WriteToNodeInternal(size_t nodeIndex, const EncodedVideoFrame& frame, bool isOverwrite) {
				PreAllocatedFrameNode& node = *nodes_[nodeIndex];

				// [Large frame fix] Clean old data (if dynamic allocation or overwrite scenario)
				if (node.IsDynamicAlloc()) {
					node.FreeDynamicData();
				}

				// [Large frame fix] Select write method based on frame size
				if (frame.size > preAllocSize_) {
					// Dynamic allocation
					uint8_t* dynData = static_cast<uint8_t*>(malloc(frame.size));
					if (!dynData) {
						largeFrameRejects_.fetch_add(1, std::memory_order_relaxed);
						VCS_MEDIA_LOG_ERROR("[Queue] Failed to allocate %zu bytes for large frame",
							frame.size);
						return false;
					}
					std::memcpy(dynData, frame.data, frame.size);
					node.SetHasDataWithDynamic(frame.size, frame, dynData);
					dynamicAllocCount_.fetch_add(1, std::memory_order_relaxed);
					VCS_MEDIA_LOG_TRACE("[Queue] Large frame %zu bytes using dynamic allocation",
						frame.size);
				}
				else {
					// Use pre-allocated buffer
					std::memcpy(node.buffer.data(), frame.data, frame.size);
					node.SetHasData(frame.size, frame);
				}
				// Note: Don't reset refCount, keep as 0

				// Update tail (producer advances)
				tail_.store(NextPos(nodeIndex), std::memory_order_release);

				// [Fix] Don't increase count_ when overwriting, because it's replacement not addition
				if (!isOverwrite) {
					count_.fetch_add(1, std::memory_order_release);
				}

				return true;
			}

			//  Update key frame cache (with lock protection)
			void UpdateKeyFrameCache(const EncodedVideoFrame& frame) {
				if (!frame.isKeyFrame) {
					return;
				}

				std::lock_guard<std::mutex> lock(keyFrameMutex_);

				if (!keyFrameCache_) {
					keyFrameCache_ = std::make_unique<KeyFrameCache>();
				}

				// Release old data and copy new data
				keyFrameCache_->data.clear();
				keyFrameCache_->data.resize(frame.size);
				std::memcpy(keyFrameCache_->data.data(), frame.data, frame.size);
				keyFrameCache_->size = frame.size;
				keyFrameCache_->timestamp = frame.timestamp;
				keyFrameCache_->width = frame.width;
				keyFrameCache_->height = frame.height;
				keyFrameCache_->isH239 = frame.isH239;
				keyFrameCache_->codecType = frame.codecType;
			}
		};

		// ============================================================
		// Constructor/Destructor
		// ============================================================

		VideoFrameQueue::VideoFrameQueue(size_t capacity, size_t preAllocSize)
			: impl_(std::make_unique<Impl>(capacity, preAllocSize)) {
		}

		VideoFrameQueue::~VideoFrameQueue() = default;

		// ============================================================
		// Producer interface (lock-free)
		// ============================================================

		bool VideoFrameQueue::Push(const EncodedVideoFrame& frame) {
			// New check
			if (impl_->deprecated_.load(std::memory_order_acquire)) {
				return false;  // Deprecated, reject enqueue
			}

			if (!frame.data || frame.size == 0) {
				return false;
			}

			// Get writable position
			bool isOverwrite = false;
			int nodeIndex = impl_->GetWritableNodeIndex(isOverwrite);
			if (nodeIndex < 0) {
				return false;
			}

			// Write data
			if (!impl_->WriteToNodeInternal(static_cast<size_t>(nodeIndex), frame, isOverwrite)) {
				return false;
			}

			// Update key frame cache
			impl_->UpdateKeyFrameCache(frame);

			// [V7.0] ֪ͨ pool:�Ǹ���ʱ֡�� +1(����ʱ count_ ����,Ҳ���ص�)
			if (!isOverwrite && frameCountCallback_) {
				frameCountCallback_(1);
			}

			return true;
		}

		bool VideoFrameQueue::TryPush(const EncodedVideoFrame& frame) {
			return Push(frame);
		}

		// ============================================================
		// Consumer interface
		// ============================================================

		/**
		 * @brief Dequeue (consumer) - zero-copy version
		 *
		 *  Correct reference count lifecycle (critical!):
		 * 1. Check node has data (occupied=true)
		 * 2. [P0 Fix] Create EncodedVideoFrame and AddRef(), refCount=1 (node protected now)
		 * 3. Advance head (consumer has taken data, but node still referenced)
		 * 4. Decrement count
		 * 5. Return frame to consumer
		 * 6. Consumer finishes using frame and destroys unique_ptr
		 * 7. EncodedVideoFrame destructor calls ReleaseRef(), refCount=0
		 * 8. Producer can safely overwrite this node
		 *
		 * [P0 Race Window Fix]:
		 * Must AddRef before advancing head, prevent producer from overwriting during this window.
		 * Otherwise causes data corruption and count inconsistency.
		 */
		std::unique_ptr<EncodedVideoFrame> VideoFrameQueue::Pop(int timeoutMs) {
			auto startTime = std::chrono::steady_clock::now();

			while (true) {
				// Check if queue has data
				if (impl_->IsEmpty()) {
					if (timeoutMs <= 0) {
						return nullptr;
					}
				}
				else {
					// Get head position
					size_t currentHead = impl_->head_.load(std::memory_order_acquire);
					PreAllocatedFrameNode& node = *impl_->nodes_[currentHead];

					// Check if node has valid data
					if (node.HasData()) {
						// [P0 Fix] Must AddRef first, then advance head
						// Order: AddRef -> head++ -> count--
						// During AddRef and head++, producer sees refCount>0 and won't overwrite

						// [Large frame fix] Get correct data pointer (pre-allocated or dynamic)
						bool isDynamic = node.IsDynamicAlloc();
						uint8_t* dataPtr = node.GetDataPtr();  // Use non-const version

						// 1. Create frame object (CreateFromPool internally calls AddRef)
						auto result = EncodedVideoFrame::CreateFromPool(
							dataPtr,
							node.dataSize,
							node.timestamp,
							node.sequenceNumber,
							node.ssrc,  // [Key Fix] Pass SSRC
							node.width,
							node.height,
							node.isKeyFrame,
							node.isH239,
							node.codecType,
							&node.refCount,
							isDynamic);  // Pass whether dynamic allocation

						// [Double free fix] Transfer dynamic data ownership to frame object
						// Frame destructor will release this memory, node no longer holds, avoid double free
						if (isDynamic) {
							// Key: use const_cast + mutable or directly access mutable member
							// Since node is protected by AddRef, no concurrent write at this time
							const_cast<PreAllocatedFrameNode&>(node).dynamicData = nullptr;
							VCS_MEDIA_LOG_TRACE("[Queue] Transferred dynamic data ownership to frame");
						}

						// 2. Advance head (node is now protected by reference)
						impl_->head_.store(impl_->NextPos(currentHead), std::memory_order_release);

						// 3. Decrement count
						impl_->count_.fetch_sub(1, std::memory_order_release);
						// [V7.0] ֪ͨ pool ֡������
						if (frameCountCallback_) frameCountCallback_(-1);

						// 4. Return frame (frame destructor will ReleaseRef and free dynamic data)
						return result;
					}
				}

				// Check timeout
				if (timeoutMs <= 0) {
					break;
				}

				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - startTime).count();
				if (elapsed >= timeoutMs) {
					break;
				}

				// Brief wait before retry (avoid CPU spinning)
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}

			return nullptr;
		}

		std::unique_ptr<EncodedVideoFrame> VideoFrameQueue::TryPop() {
			return Pop(0);
		}

		void VideoFrameQueue::MarkDeprecated() {
			impl_->deprecated_.store(true, std::memory_order_release);
		}

		size_t VideoFrameQueue::GetTotalRefCount() const {
			size_t total = 0;
			for (const auto& node_ptr : impl_->nodes_) {
				total += static_cast<size_t>(node_ptr->refCount.load(std::memory_order_acquire));
			}
			return total;
		}

		// ============================================================
		// State query (atomic operations, no locking needed)
		// ============================================================

		size_t VideoFrameQueue::Size() const {
			return impl_->count_.load(std::memory_order_acquire);
		}

		size_t VideoFrameQueue::Capacity() const {
			return impl_->capacity_;
		}

		bool VideoFrameQueue::Empty() const {
			return impl_->IsEmpty();
		}

		bool VideoFrameQueue::Full() const {
			return impl_->IsFull();
		}

		void VideoFrameQueue::Clear() {
			//  Safe cleanup:
			// 1. Mark deprecated first, block new Push
			// 2. Wait for all external references to release
			// 3. Then reset node state
			// 4. Only free dynamic data without external references

			impl_->deprecated_.store(true, std::memory_order_release);

			// Wait for all external references to release (max 500ms)
			const int kMaxWaitMs = 500;
			const int kStepMs = 10;
			int elapsed = 0;
			while (elapsed < kMaxWaitMs) {
				if (GetTotalRefCount() == 0) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
				elapsed += kStepMs;
			}

			// If still has external references, log warning
			if (GetTotalRefCount() > 0) {
				VCS_MEDIA_LOG_WARNING("[Queue] Clear with %zu pending refs, data may leak",
					GetTotalRefCount());
			}

			// Cleanup all nodes: only free dynamic data without external references
			for (auto& node_ptr : impl_->nodes_) {
				// [Safety fix] Only free dynamic data when no external references
				// Otherwise let external reference holder release (avoid dangling pointer)
				if (node_ptr->GetRefCount() == 0) {
					node_ptr->FreeDynamicData();
				}
				else {
					// [Last fix] When still has external references, node must abandon ownership
					// Otherwise subsequent overwrite triggers double free (external frame destruction + WriteToNodeInternal free)
					if (node_ptr->IsDynamicAlloc()) {
						VCS_MEDIA_LOG_TRACE("[Queue] Clear releasing ownership of dynamic data with %d refs",
							node_ptr->GetRefCount());
						// Node abandons ownership, set to nullptr, external holder responsible for free
						node_ptr->dynamicData = nullptr;
					}
				}
				node_ptr->occupied.store(false, std::memory_order_release);
			}

			impl_->head_.store(0, std::memory_order_release);
			impl_->tail_.store(0, std::memory_order_release);
			// [V7.0] Clear ʱ������֡���� pool ��������۵�
			size_t oldCount = impl_->count_.exchange(0, std::memory_order_release);
			if (oldCount > 0 && frameCountCallback_) {
				frameCountCallback_(-static_cast<int>(oldCount));
			}

			// Reset deprecated flag, allow subsequent reuse
			impl_->deprecated_.store(false, std::memory_order_release);
		}

		const std::string& VideoFrameQueue::GetMemberId() const {
			return impl_->memberId_;
		}

		void VideoFrameQueue::SetMemberId(const std::string& id) {
			impl_->memberId_ = id;
		}

		float VideoFrameQueue::GetUsage() const {
			return static_cast<float>(impl_->count_.load(std::memory_order_acquire)) / impl_->capacity_;
		}

		uint64_t VideoFrameQueue::GetDroppedFrameCount() const {
			return impl_->droppedFrames_.load(std::memory_order_relaxed);
		}

		uint64_t VideoFrameQueue::GetLargeFrameRejectCount() const {
			return impl_->largeFrameRejects_.load(std::memory_order_relaxed);
		}

		uint64_t VideoFrameQueue::GetDynamicAllocCount() const {
			return impl_->dynamicAllocCount_.load(std::memory_order_relaxed);
		}

		// ============================================================
		// Key frame retrieval (zero-copy)
		// ============================================================

		/**
		 * @brief Get latest key frame (zero-copy)
		 *
		 *  Key frame stored independently, not dependent on queue nodes,
		 * so no reference count protection needed.
		 */
		std::unique_ptr<EncodedVideoFrame> VideoFrameQueue::GetLatestKeyFrame() {
			std::lock_guard<std::mutex> lock(impl_->keyFrameMutex_);

			if (!impl_->keyFrameCache_ || impl_->keyFrameCache_->data.empty()) {
				return nullptr;
			}

			auto* cache = impl_->keyFrameCache_.get();

			// [Fix] Deep copy: allocate independent memory
			uint8_t* copyData = static_cast<uint8_t*>(malloc(cache->size));
			if (!copyData) return nullptr;
			std::memcpy(copyData, cache->data.data(), cache->size);

			//  Key frame uses independent buffer, no reference count needed
			// Use CreateDynamic, because key frame cache data may be overwritten anytime
			auto result = EncodedVideoFrame::CreateDynamic(
				copyData,  // Now perfectly matches the free() passed to the destructor!
				cache->size,
				cache->timestamp,
				0,  // sequenceNumber
				0,  // ssrc (key frame cache doesn't care about SSRC)
				cache->width,
				cache->height,
				true,  // isKeyFrame
				cache->isH239,
				cache->codecType);

			return result;
		}

		// ============================================================
		// VideoFrameQueuePool implementation
		// ============================================================

		/**
		 * @brief Video frame queue pool
		 *
		 *
		 * - Removed dangerous thread-local cache (dangling pointer risk)
		 * - Only uses read-write lock, sufficient performance for read-heavy scenarios
		 */
		struct VideoFrameQueuePool::Impl {
			size_t queueCapacity_;
			size_t preAllocSize_;
			std::unordered_map<std::string, std::unique_ptr<VideoFrameQueue>> queues_;

			//  Read-write lock instead of mutex (read-heavy scenarios)
			mutable std::mutex mutex_;

			// [V7.0] ���ж��е�֡���ܺ�(ԭ��),���� O(1) �� HasAnyData() �ж�
			// �ɸ����е� frameCountCallback_ �ص�ά��
			std::atomic<int64_t> totalPendingFrames_{ 0 };

			Impl(size_t capacity, size_t preAllocSize)
				: queueCapacity_(capacity), preAllocSize_(preAllocSize) {}

			//  Pure read-write lock lookup, no thread-local cache
			VideoFrameQueue* Lookup(const std::string& memberId) {
				std::lock_guard<std::mutex> lock(mutex_);
				auto it = queues_.find(memberId);
				if (it != queues_.end()) {
					return it->second.get();
				}
				return nullptr;
			}
		};

		VideoFrameQueuePool::VideoFrameQueuePool(size_t queueCapacity)
			: impl_(std::make_unique<Impl>(queueCapacity, 50 * 1024)) {
		}

		VideoFrameQueuePool::~VideoFrameQueuePool() = default;

		bool VideoFrameQueuePool::CreateQueue(const std::string& memberId) {
			std::lock_guard<std::mutex> lock(impl_->mutex_);

			if (impl_->queues_.find(memberId) != impl_->queues_.end()) {
				return false;  // Already exists
			}

			auto queue = std::make_unique<VideoFrameQueue>(
				impl_->queueCapacity_, impl_->preAllocSize_);
			queue->SetMemberId(memberId);

			// [V7.0] ע��֡���仯�ص�,�� pool ά�� O(1) ����֡��
			queue->SetFrameCountCallback([this](int delta) {
				if (delta > 0) {
					impl_->totalPendingFrames_.fetch_add(delta, std::memory_order_relaxed);
				}
				else if (delta < 0) {
					impl_->totalPendingFrames_.fetch_sub(-delta, std::memory_order_relaxed);
				}
			});

			impl_->queues_[memberId] = std::move(queue);

			return true;
		}

		bool VideoFrameQueuePool::DestroyQueue(const std::string& memberId) {
			std::unique_ptr<VideoFrameQueue> queue_ptr;
			{
				std::lock_guard<std::mutex> lock(impl_->mutex_);
				auto it = impl_->queues_.find(memberId);
				if (it == impl_->queues_.end()) {
					return false;
				}
				// Take ownership of queue object, remove from pool (subsequent GetQueue returns nullptr)
				queue_ptr = std::move(it->second);
				impl_->queues_.erase(it);
			}

			VideoFrameQueue* queue = queue_ptr.get();

			// 1. Mark deprecated (block new frames from enqueueing)
			queue->MarkDeprecated();

			// 2. Brief wait until all external references released or timeout
			// ע��:wait �ڼ� consumer ���ܻ��� Pop,�ûص���������,���ּ�����ͬ��
			const int kMaxWaitMs = 200;
			const int kStepMs = 20;
			int elapsed = 0;
			size_t lastRefCount = 0;
			size_t lastSize = 0;
			while (elapsed < kMaxWaitMs) {
				lastRefCount = queue->GetTotalRefCount();
				lastSize = queue->Size();
				// Check condition: no external references (all refCount=0) and queue logically empty
				if (lastRefCount == 0 && lastSize == 0) {
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
				elapsed += kStepMs;
			}

			// 3. Handle result
			if (elapsed >= kMaxWaitMs) {
				// Timeout: frames are being held for long time
				//  No longer leak memory, but force cleanup
				VCS_MEDIA_LOG_WARNING("[Pool] DestroyQueue timeout for %s (refCount=%zu, size=%zu), forcing cleanup",
					memberId.c_str(), lastRefCount, lastSize);

				// Call Clear for forced cleanup (internally handles still-referenced situation)
				// Clear �ᴥ���ص���ʣ��֡���Ӽ������۵�
				queue->Clear();

				// queue_ptr leaving scope automatically deletes queue object
				return true;
			}

			// Normal case: all references released, safely clear and destroy
			// ��ʱ count_ �Ѿ��� 0,Clear ���ᴥ���ص�(��Ϊ oldCount == 0)
			queue->Clear();
			// queue_ptr leaving scope automatically deletes queue object
			return true;
		}

		VideoFrameQueue* VideoFrameQueuePool::GetQueue(const std::string& memberId) {
			//  Direct read-write lock lookup, no thread-local cache
			return impl_->Lookup(memberId);
		}

		bool VideoFrameQueuePool::HasQueue(const std::string& memberId) const {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			return impl_->queues_.find(memberId) != impl_->queues_.end();
		}

		size_t VideoFrameQueuePool::Size() const {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			return impl_->queues_.size();
		}

		void VideoFrameQueuePool::Clear() {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			// [V7.0] ������ж���ǰ,�Ȱ���֡������(�ص������ٴ���,�Լ��ֶ�����)
			for (auto& kv : impl_->queues_) {
				kv.second->SetFrameCountCallback(nullptr);
			}
			impl_->queues_.clear();
			impl_->totalPendingFrames_.store(0, std::memory_order_relaxed);
		}

		std::vector<std::string> VideoFrameQueuePool::GetAllMemberIds() const {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			std::vector<std::string> ids;
			ids.reserve(impl_->queues_.size());
			for (const auto& kv : impl_->queues_) {
				ids.push_back(kv.first);
			}
			return ids;
		}

		std::vector<std::string> VideoFrameQueuePool::GetActiveMemberIds() const {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			std::vector<std::string> activeIds;

			for (const auto& kv : impl_->queues_) {
				if (kv.second->Size() > 0) {
					activeIds.push_back(kv.first);
				}
			}

			return activeIds;
		}

		std::vector<VideoFrameQueue*> VideoFrameQueuePool::GetQueues(
			const std::vector<std::string>& memberIds) {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			std::vector<VideoFrameQueue*> queues;

			for (const auto& memberId : memberIds) {
				auto it = impl_->queues_.find(memberId);
				if (it != impl_->queues_.end()) {
					queues.push_back(it->second.get());
				}
			}

			return queues;
		}

		void VideoFrameQueuePool::SetPreAllocSize(size_t size) {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			impl_->preAllocSize_ = size;
		}

		size_t VideoFrameQueuePool::GetPreAllocSize() const {
			std::lock_guard<std::mutex> lock(impl_->mutex_);
			return impl_->preAllocSize_;
		}

		bool VideoFrameQueuePool::HasAnyData() const {
			// [V7.0] O(1) ʵ��:һ�� atomic load
			return impl_->totalPendingFrames_.load(std::memory_order_acquire) > 0;
		}

	} // namespace media
} // namespace vcs
