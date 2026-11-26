#pragma once

/**
 * @file VideoFrameQueue.h
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
 * 7. Clear safety issue: Wait for refCount to zero before cleanup, abandon dynamic data with references
 * 8. PeekSafe/Peek dangerous interface: Removed
 * 9. Thread-local cache dangling pointer: Removed, use read-write lock instead
 *
 * Design principles (SPSC):
 * - Producer (RTP thread) single-threaded write
 * - Consumer (decode thread) single-threaded read
 * - head advanced by consumer, tail advanced by producer
 * - Reference count ensures data not overwritten during consumer usage
 *
 * Reference count lifecycle (critical!):
 * Pop order: AddRef -> head++ -> count-- -> return frame -> ReleaseRef on frame destruction
 *
 * Dynamic data ownership transfer:
 * If node uses dynamic allocation during Pop, immediately set dynamicData to nullptr,
 * ownership transferred to frame object, memory released on frame destruction.
 */

#include "media_export.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <utility>

namespace vcs {
	namespace media {

		// ============================================================
		// Encoded video frame
		// ============================================================
		/**
		 * @brief Encoded video frame
		 *
		 * Stores complete H.264/VP8/AV1 encoded data after RTP FU-A reassembly.
		 *  Destructor correctly releases reference count
		 */
		struct EncodedVideoFrame {
			uint8_t* data = nullptr;           // Encoded data pointer
			size_t size = 0;                    // Data size
			uint32_t width = 0;                 // Original width
			uint32_t height = 0;                // Original height
			uint64_t timestamp = 0;             // RTP timestamp
			uint32_t ssrc = 0;                  // SSRC identifier
			uint16_t sequenceNumber = 0;        // RTP sequence number
			uint8_t payloadType = 0;            // Payload type
			bool isKeyFrame = false;            // Is keyframe
			bool isH239 = false;                // Is H.239 secondary stream
			int64_t receiveTime = 0;           // Receive timestamp (ms)

			// Encoding info
			enum class CodecType {
				H264, VP8, VP9, AV1, H265,
			} codecType = CodecType::H264;

			//  Reference count pointer (points to atomic in node)
			std::atomic<int32_t>* refCount = nullptr;

			// Frame data source flag
			enum class DataSource {
				PoolBuffer,    // From pool pre-allocated buffer
				DynamicAlloc,  // Dynamic allocation
			} dataSource = DataSource::PoolBuffer;

			//  Reference count management
			inline void AddRef() {
				if (refCount) {
					refCount->fetch_add(1, std::memory_order_acq_rel);
				}
			}

			inline void ReleaseRef() {
				if (refCount) {
					refCount->fetch_sub(1, std::memory_order_acq_rel);
				}
			}

			/**
			 * @brief Create frame from pool buffer
			 * @param buffer Pre-allocated buffer pointer (or dynamic data pointer if using dynamic allocation)
			 * @param isDynamic Whether using dynamic allocation (if true, don't free buffer on destruction)
			 * @note [Important] Calling this function automatically increments node refCount, decrements on destruction.
			 *       Caller **must NOT** call AddRef() externally.
			 */
			static std::unique_ptr<EncodedVideoFrame> CreateFromPool(
				uint8_t* buffer,
				size_t dataSize,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				uint32_t ssrc,  // [Key Fix] Added SSRC parameter
				uint32_t width,
				uint32_t height,
				bool isKeyFrame,
				bool isH239,
				EncodedVideoFrame::CodecType codecType,
				std::atomic<int32_t>* nodeRefCount,
				bool isDynamic = false) {

				auto frame = std::make_unique<EncodedVideoFrame>();
				frame->data = buffer;
				frame->size = dataSize;
				frame->timestamp = timestamp;
				frame->sequenceNumber = sequenceNumber;
				frame->ssrc = ssrc;  // [Key Fix] Restore SSRC
				frame->width = width;
				frame->height = height;
				frame->isKeyFrame = isKeyFrame;
				frame->isH239 = isH239;
				frame->codecType = codecType;
				frame->dataSource = isDynamic ? DataSource::DynamicAlloc : DataSource::PoolBuffer;
				frame->refCount = nodeRefCount;
				frame->AddRef();  // Increment reference count
				return frame;
			}

			// Factory method (dynamic allocation, for large frame scenarios)
			static std::unique_ptr<EncodedVideoFrame> CreateDynamic(
				uint8_t* data,
				size_t size,
				uint64_t timestamp,
				uint16_t sequenceNumber,
				uint32_t ssrc,  // [Key Fix] Added SSRC parameter
				uint32_t width,
				uint32_t height,
				bool isKeyFrame,
				bool isH239,
				EncodedVideoFrame::CodecType codecType) {

				auto frame = std::make_unique<EncodedVideoFrame>();
				frame->data = data;
				frame->size = size;
				frame->timestamp = timestamp;
				frame->sequenceNumber = sequenceNumber;
				frame->ssrc = ssrc;  // [Key Fix] Save SSRC
				frame->width = width;
				frame->height = height;
				frame->isKeyFrame = isKeyFrame;
				frame->isH239 = isH239;
				frame->codecType = codecType;
				frame->dataSource = DataSource::DynamicAlloc;
				frame->refCount = nullptr;  // Dynamic allocation doesn't need reference count
				return frame;
			}

			//  Destructor correctly releases reference count
			~EncodedVideoFrame() {
				//  Must call ReleaseRef to release reference
				ReleaseRef();

				// Dynamic allocated data needs to be freed
				if (dataSource == DataSource::DynamicAlloc && data) {
					free(data);
					data = nullptr;
				}
			}

		private:
			EncodedVideoFrame(const EncodedVideoFrame&) = delete;
			EncodedVideoFrame& operator=(const EncodedVideoFrame&) = delete;

		public:
			EncodedVideoFrame() = default;
			EncodedVideoFrame(EncodedVideoFrame&&) = default;
			EncodedVideoFrame& operator=(EncodedVideoFrame&&) = default;
		};

		// ============================================================
		// Pre-allocated frame node
		// ============================================================
		/**
		 * @brief Pre-allocated frame node
		 *
		 *
		 * - Simplified state management: Only one occupied flag
		 * - Reference count held by consumer frame object
		 * - Producer only writes, doesn't manipulate reference count
		 * - Supports large frame dynamic allocation (when exceeding pre-allocated buffer)
		 */
		struct PreAllocatedFrameNode {
			std::vector<uint8_t> buffer;     // Pre-allocated buffer
			size_t dataSize = 0;             // Actual data size
			uint64_t timestamp = 0;
			uint16_t sequenceNumber = 0;
			uint32_t ssrc = 0;              // [Key Fix] Save SSRC, for FIR request
			uint32_t width = 0;
			uint32_t height = 0;
			bool isKeyFrame = false;
			bool isH239 = false;
			EncodedVideoFrame::CodecType codecType = EncodedVideoFrame::CodecType::H264;

			//  Dynamic allocated data (used when exceeding pre-allocated buffer)
			uint8_t* dynamicData = nullptr;  // Dynamic allocated data pointer

			//  Reference count (held by consumer frame object)
			std::atomic<int32_t> refCount{ 0 };

			//  Node state: true=has valid data to read, false=idle writable
			std::atomic<bool> occupied{ false };

			PreAllocatedFrameNode() = default;

			explicit PreAllocatedFrameNode(size_t capacity) : buffer(capacity) {}

			// Disable copy
			PreAllocatedFrameNode(const PreAllocatedFrameNode&) = delete;
			PreAllocatedFrameNode& operator=(const PreAllocatedFrameNode&) = delete;

			// Allow move
			PreAllocatedFrameNode(PreAllocatedFrameNode&&) = default;
			PreAllocatedFrameNode& operator=(PreAllocatedFrameNode&&) = default;

			// Safety check: Has valid data
			bool HasData() const {
				return occupied.load(std::memory_order_acquire);
			}

			// [Large frame fix] Get data pointer
			// Note: Returns non-const pointer, because frame object needs to hold modifiable data pointer
			uint8_t* GetDataPtr() {
				return dynamicData ? dynamicData : buffer.data();
			}

			// const version (read-only access)
			const uint8_t* GetDataPtr() const {
				return dynamicData ? dynamicData : buffer.data();
			}

			// [Large frame fix] Check if using dynamic allocation
			bool IsDynamicAlloc() const {
				return dynamicData != nullptr;
			}

			// Get reference count
			int32_t GetRefCount() const {
				return refCount.load(std::memory_order_acquire);
			}

			// Mark as having data (called by producer)
			void SetHasData(size_t actualSize, const EncodedVideoFrame& frame) {
				dataSize = actualSize;
				timestamp = frame.timestamp;
				sequenceNumber = frame.sequenceNumber;
				ssrc = frame.ssrc;  // [Key Fix] Save SSRC
				width = frame.width;
				height = frame.height;
				isKeyFrame = frame.isKeyFrame;
				isH239 = frame.isH239;
				codecType = frame.codecType;
				// Don't modify refCount, keep it at 0
				occupied.store(true, std::memory_order_release);
			}

			// [Large frame fix] Mark as having data and hold dynamic allocated memory
			void SetHasDataWithDynamic(size_t actualSize, const EncodedVideoFrame& frame, uint8_t* dynData) {
				dataSize = actualSize;
				timestamp = frame.timestamp;
				sequenceNumber = frame.sequenceNumber;
				ssrc = frame.ssrc;  // [Key Fix] Save SSRC
				width = frame.width;
				height = frame.height;
				isKeyFrame = frame.isKeyFrame;
				isH239 = frame.isH239;
				codecType = frame.codecType;
				dynamicData = dynData;  // Save dynamic allocated data pointer
				occupied.store(true, std::memory_order_release);
			}

			// Mark as consumed (called after consumer takes it, after head moved)
			// Note: This is optional, because new Pop won't use this node again
			void MarkConsumed() {
				occupied.store(false, std::memory_order_release);
				refCount.store(0, std::memory_order_release);
				// [Large frame fix] Free dynamic allocated data
				if (dynamicData) {
					free(dynamicData);
					dynamicData = nullptr;
				}
			}

			// [Large frame fix] Free dynamic data (don't reset occupied state)
			void FreeDynamicData() {
				if (dynamicData) {
					free(dynamicData);
					dynamicData = nullptr;
				}
			}
		};

		// ============================================================
		// Video frame queue
		// ============================================================
		/**
		 * @brief Single-member video frame queue (SPSC lock-free)
		 *
		 *
		 *
		 * Reference count lifecycle (critical!):
		 * 1. Producer Push: Write data, occupied=true, don't modify refCount
		 * 2. Consumer Pop:
		 *    a. Check occupied=true
		 *    b. Create EncodedVideoFrame, AddRef(), refCount=1 ([P0 Fix] Protect node first)
		 *    c. Advance head (consumer has taken data)
		 *    d. Decrement count
		 *    e. Return frame to consumer
		 * 3. Consumer finishes using frame and destroys it:
		 *    a. EncodedVideoFrame destructor
		 *    b. ReleaseRef(), refCount=0
		 * 4. Producer can safely overwrite this node
		 *
		 * [P0 Race Window Fix]:
		 * Must AddRef before advancing head, prevent producer from overwriting during this window.
		 *
		 * [Large Frame Support]:
		 * Use malloc for dynamic allocation when exceeding pre-allocated buffer, node holds lifecycle.
		 */
		class VideoFrameQueue {
		public:
			/**
			 * @brief Constructor
			 * @param capacity Queue capacity (default 30 frames)
			 * @param preAllocSize Pre-allocated frame size (default 50KB)
			 */
			VCS_MEDIA_API VideoFrameQueue(size_t capacity = 30,
				size_t preAllocSize = 50 * 1024);
			VCS_MEDIA_API ~VideoFrameQueue();

			// Disable copy
			VideoFrameQueue(const VideoFrameQueue&) = delete;
			VideoFrameQueue& operator=(const VideoFrameQueue&) = delete;

			// ============================================================
			// Producer interface (called by RTPEngine)
			// ============================================================

			/**
			 * @brief Enqueue (producer)
			 * @return true on success, false if queue full or frame too large
			 */
			VCS_MEDIA_API bool Push(const EncodedVideoFrame& frame);

			/**
			 * @brief Try enqueue (non-blocking)
			 */
			VCS_MEDIA_API bool TryPush(const EncodedVideoFrame& frame);

			// ============================================================
			// Consumer interface (called by VideoProcessor)
			// ============================================================

			/**
			 * @brief Dequeue (consumer) - zero-copy version
			 * @param timeoutMs Timeout (ms), 0 means non-blocking
			 * @return Frame data (empty unique_ptr means queue empty or timeout)
			 *
			 *
			 * - AddRef first to protect node (prevent producer overwrite)
			 * - Then advance head and decrement count
			 * - Finally return frame (ReleaseRef on frame destruction)
			 *
			 * Order: AddRef -> head++ -> count-- -> return frame
			 */
			VCS_MEDIA_API std::unique_ptr<EncodedVideoFrame> Pop(int timeoutMs = 0);

			/**
			 * @brief Try dequeue (non-blocking)
			 */
			VCS_MEDIA_API std::unique_ptr<EncodedVideoFrame> TryPop();

			// Mark queue as deprecated (prevent new Push)
			void MarkDeprecated();

			// Get sum of reference counts for all current nodes (used during destruction)
			size_t GetTotalRefCount() const;

			// ============================================================
			// State query
			// ============================================================

			VCS_MEDIA_API size_t Size() const;
			VCS_MEDIA_API size_t Capacity() const;
			VCS_MEDIA_API bool Empty() const;
			VCS_MEDIA_API bool Full() const;
			VCS_MEDIA_API void Clear();
			VCS_MEDIA_API const std::string& GetMemberId() const;
			VCS_MEDIA_API void SetMemberId(const std::string& id);
			VCS_MEDIA_API float GetUsage() const;
			VCS_MEDIA_API uint64_t GetDroppedFrameCount() const;
			VCS_MEDIA_API uint64_t GetLargeFrameRejectCount() const;
			VCS_MEDIA_API uint64_t GetDynamicAllocCount() const;

			/**
			 * @brief Get latest key frame (zero-copy)
			 */
			VCS_MEDIA_API std::unique_ptr<EncodedVideoFrame> GetLatestKeyFrame();

			// ============================================================
			// [V7.0] Frame count change notification
			// ============================================================
			/**
			 * @brief ����֡���仯�ص�
			 *
			 * �� Push �ɹ� / Pop �ɹ� / Clear ʱ,����ô˻ص�,������֡���仯��
			 * (����=����,����=����,���ʱ�����ĵ�ǰ��С).
			 * VideoFrameQueuePool ������ά�� O(1) ��"�Ƿ�������"�ж�.
			 */
			using FrameCountCallback = std::function<void(int delta)>;
			void SetFrameCountCallback(FrameCountCallback cb) { frameCountCallback_ = std::move(cb); }

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;

			// [V7.0] ֡���仯�ص�(�� VideoFrameQueuePool ����,��ѡ)
			FrameCountCallback frameCountCallback_;
		};

		// ============================================================
		// Video frame queue pool
		// ============================================================
		/**
		 * @brief Video frame queue pool
		 *
		 *
		 * - Removed dangerous thread-local cache
		 * - Only uses read-write lock, sufficient performance for read-heavy scenarios
		 */
		class VideoFrameQueuePool {
		public:
			/**
			 * @brief Constructor
			 * @param queueCapacity Single queue capacity (default 30 frames)
			 */
			VCS_MEDIA_API explicit VideoFrameQueuePool(size_t queueCapacity = 30);
			VCS_MEDIA_API ~VideoFrameQueuePool();

			// Disable copy
			VideoFrameQueuePool(const VideoFrameQueuePool&) = delete;
			VideoFrameQueuePool& operator=(const VideoFrameQueuePool&) = delete;

			// Queue management
			VCS_MEDIA_API bool CreateQueue(const std::string& memberId);
			VCS_MEDIA_API bool DestroyQueue(const std::string& memberId);

			/**
			 * @brief Get member queue
			 * @return Queue pointer (returns nullptr if not exists)
			 *
			 * Uses read-write lock to protect read operations
			 */
			VCS_MEDIA_API VideoFrameQueue* GetQueue(const std::string& memberId);

			VCS_MEDIA_API bool HasQueue(const std::string& memberId) const;
			VCS_MEDIA_API size_t Size() const;
			VCS_MEDIA_API void Clear();
			VCS_MEDIA_API std::vector<std::string> GetAllMemberIds() const;
			VCS_MEDIA_API std::vector<std::string> GetActiveMemberIds() const;
			VCS_MEDIA_API std::vector<VideoFrameQueue*> GetQueues(const std::vector<std::string>& memberIds);
			VCS_MEDIA_API void SetPreAllocSize(size_t size);
			VCS_MEDIA_API size_t GetPreAllocSize() const;

			// ============================================================
			// [V7.0] O(1) ��ѯ
			// ============================================================
			/**
			 * @brief ������������Ƿ��д�����֡
			 *
			 * O(1) ʵ��:�ڲ�ά��һ��ԭ�Ӽ�����,������ Push/Pop ʱ����.
			 * ���� WorkerLoop �� ProcessOnce() ֮ǰ�����ж�,�����ת.
			 */
			VCS_MEDIA_API bool HasAnyData() const;

		private:
			struct Impl;
			std::unique_ptr<Impl> impl_;
		};

	} // namespace media
} // namespace vcs
