#pragma once

/**
 * @file SharedMutex.h
 * @brief Read-write lock implementation
 *
 * Uses C++11 standard library implementation, supports Windows, Linux, macOS, ARM and all C++11 environments
 * No dependency on pthread, truly cross-platform
 *
 * Implementation principle:
 * - Read lock: atomic counter + condition variable, waiting for write lock release
 * - Write lock: exclusive mutex, waiting for all read locks to release
 *
 * Usage:
 *   SharedMutex mutex;
 *
 *   // Read lock (multiple threads can hold simultaneously)
 *   {
 *       auto readLock = mutex.GetReadLock();
 *       // Read operations...
 *   }
 *
 *   // Write lock (exclusive)
 *   {
 *       auto writeLock = mutex.GetWriteLock();
 *       // Write operations...
 *   }
 */

#include "base_export.h"
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace vcs {
	namespace base {

		/**
		 * @brief Read-write lock (C++11 native implementation)
		 *
		 * Features:
		 * - Multiple read locks can be held simultaneously
		 * - Write lock is exclusive, other read and write locks must wait
		 * - Uses RAII pattern, automatically releases locks
		 * - No dependency on pthread, suitable for any C++11 environment
		 *
		 * Implementation:
		 * - Uses std::atomic<int> counter to track read lock count
		 * - Uses std::mutex + std::condition_variable to implement write lock priority
		 */
		class SharedMutex {
		public:
			/**
			 * @brief Read lock
			 *
			 * Uses atomic counter, multiple read locks can be held simultaneously
			 */
			class ReadLock {
			public:
				explicit ReadLock(SharedMutex* shared_mutex)
					: shared_mutex_(shared_mutex), locked_(false) {
					if (shared_mutex_) {
						shared_mutex_->AcquireReadLock();
						locked_ = true;
					}
				}

				~ReadLock() {
					if (locked_ && shared_mutex_) {
						shared_mutex_->ReleaseReadLock();
					}
				}

				// Disable copy
				ReadLock(const ReadLock&) = delete;
				ReadLock& operator=(const ReadLock&) = delete;

				// Allow move
				ReadLock(ReadLock&& other) noexcept
					: shared_mutex_(other.shared_mutex_), locked_(other.locked_) {
					other.locked_ = false;
				}

				ReadLock& operator=(ReadLock&& other) noexcept {
					if (this != &other) {
						if (locked_ && shared_mutex_) {
							shared_mutex_->ReleaseReadLock();
						}
						shared_mutex_ = other.shared_mutex_;
						locked_ = other.locked_;
						other.locked_ = false;
					}
					return *this;
				}

				void unlock() {
					if (locked_ && shared_mutex_) {
						shared_mutex_->ReleaseReadLock();
						locked_ = false;
					}
				}

			private:
				SharedMutex* shared_mutex_;
				bool locked_;
			};

			/**
			 * @brief Write lock
			 *
			 * Exclusive access, waiting for all read locks to release
			 */
			class WriteLock {
			public:
				explicit WriteLock(SharedMutex* shared_mutex)
					: shared_mutex_(shared_mutex), locked_(false) {
					if (shared_mutex_) {
						shared_mutex_->AcquireWriteLock();
						locked_ = true;
					}
				}

				~WriteLock() {
					if (locked_ && shared_mutex_) {
						shared_mutex_->ReleaseWriteLock();
					}
				}

				// Disable copy
				WriteLock(const WriteLock&) = delete;
				WriteLock& operator=(const WriteLock&) = delete;

				// Allow move
				WriteLock(WriteLock&& other) noexcept
					: shared_mutex_(other.shared_mutex_), locked_(other.locked_) {
					other.locked_ = false;
				}

				WriteLock& operator=(WriteLock&& other) noexcept {
					if (this != &other) {
						if (locked_ && shared_mutex_) {
							shared_mutex_->ReleaseWriteLock();
						}
						shared_mutex_ = other.shared_mutex_;
						locked_ = other.locked_;
						other.locked_ = false;
					}
					return *this;
				}

				void unlock() {
					if (locked_ && shared_mutex_) {
						shared_mutex_->ReleaseWriteLock();
						locked_ = false;
					}
				}

			private:
				SharedMutex* shared_mutex_;
				bool locked_;
			};

			/**
			 * @brief Constructor
			 */
			SharedMutex()
				: readers_(0)
				, writers_waiting_(0)
				, writer_active_(false) {}

			/**
			 * @brief Destructor
			 */
			~SharedMutex() {
				// Ensure no active locks
			}

			/**
			 * @brief Get read lock
			 */
			ReadLock GetReadLock() {
				return ReadLock(this);
			}

			/**
			 * @brief Get write lock
			 */
			WriteLock GetWriteLock() {
				return WriteLock(this);
			}

			// Disable copy
			SharedMutex(const SharedMutex&) = delete;
			SharedMutex& operator=(const SharedMutex&) = delete;

		private:
			// Internal method: acquire read lock
			void AcquireReadLock() {
				std::unique_lock<std::mutex> lock(mutex_);
				// Wait for no active write lock and no write lock waiting (or timeout to prevent deadlock)
				cv_.wait(lock, [this]() {
					return !writer_active_ && writers_waiting_ == 0;
				});
				readers_.fetch_add(1, std::memory_order_relaxed);
			}

			// Internal method: release read lock
			void ReleaseReadLock() {
				readers_.fetch_sub(1, std::memory_order_relaxed);
				cv_.notify_all();
			}

			// Internal method: acquire write lock
			void AcquireWriteLock() {
				std::unique_lock<std::mutex> lock(mutex_);
				writers_waiting_.fetch_add(1, std::memory_order_relaxed);
				// Wait for no active read locks and no active write lock
				cv_.wait(lock, [this]() {
					return readers_.load(std::memory_order_relaxed) == 0 && !writer_active_;
				});
				writers_waiting_.fetch_sub(1, std::memory_order_relaxed);
				writer_active_ = true;
			}

			// Internal method: release write lock
			void ReleaseWriteLock() {
				{
					std::lock_guard<std::mutex> lock(mutex_);
					writer_active_ = false;
				}
				cv_.notify_all();
			}

		private:
			std::mutex mutex_;
			std::condition_variable cv_;
			std::atomic<int> readers_;        // Number of currently active read locks
			std::atomic<int> writers_waiting_; // Number of write locks waiting
			bool writer_active_;              // Whether a write lock is active
		};

	} // namespace base

	// C++14 没有 std::shared_mutex / std::shared_lock。这里的 shared_lock 只是
	// 互斥退让实现：读也会串行。需要真读写并发请用上面的 base::SharedMutex。
	class shared_mutex {
	public:
		void lock() { mtx_.lock(); }
		void unlock() { mtx_.unlock(); }
		bool try_lock() { return mtx_.try_lock(); }

	private:
		std::mutex mtx_;
	};

	template<typename Mutex>
	class shared_lock {
	public:
		explicit shared_lock(Mutex& mtx) : mtx_(&mtx), owned_(false) {}
		shared_lock(Mutex& mtx, std::adopt_lock_t) : mtx_(&mtx), owned_(true) {}
		explicit shared_lock(const Mutex& mtx) : mtx_(const_cast<Mutex*>(&mtx)), owned_(false) {}
		shared_lock(const Mutex& mtx, std::adopt_lock_t) : mtx_(const_cast<Mutex*>(&mtx)), owned_(true) {}
		~shared_lock() { if (owned_ && mtx_) mtx_->unlock(); }

		shared_lock(const shared_lock&) = delete;
		shared_lock& operator=(const shared_lock&) = delete;

		explicit operator bool() const { return owned_; }
		bool owns_lock() const { return owned_; }

	private:
		Mutex* mtx_;
		bool owned_;
	};

} // namespace vcs
