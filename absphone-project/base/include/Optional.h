#pragma once

// C++14 没有 std::optional，这里提供项目在用的 vcs::Optional（只实现用到的子集）。
// 存储用 alignas(T) 的字节数组，而不是 std::aligned_storage：VS2017 < 15.8
// 对 alignof(T) > alignof(max_align_t) 的类型（如 32/64 字节对齐的 SIMD 缓冲）
// 会触发 C2338。

#include <new>
#include <stdexcept>
#include <utility>

namespace vcs {

	template<typename T>
	class Optional {
	public:
		Optional() : hasValue_(false) {}

		Optional(const T& value) : hasValue_(true) {
			new (storage_) T(value);
		}

		Optional(T&& value) : hasValue_(true) {
			new (storage_) T(std::move(value));
		}

		Optional(const Optional& other) : hasValue_(other.hasValue_) {
			if (hasValue_) {
				new (storage_) T(other.value());
			}
		}

		Optional(Optional&& other) noexcept : hasValue_(other.hasValue_) {
			if (hasValue_) {
				new (storage_) T(std::move(other.value()));
				other.reset();
			}
		}

		~Optional() { reset(); }

		Optional& operator=(const T& value) {
			reset();
			new (storage_) T(value);
			hasValue_ = true;
			return *this;
		}

		Optional& operator=(T&& value) {
			reset();
			new (storage_) T(std::move(value));
			hasValue_ = true;
			return *this;
		}

		Optional& operator=(const Optional& other) {
			if (this != &other) {
				reset();
				if (other.hasValue_) {
					new (storage_) T(other.value());
					hasValue_ = true;
				}
			}
			return *this;
		}

		Optional& operator=(Optional&& other) noexcept {
			if (this != &other) {
				reset();
				if (other.hasValue_) {
					new (storage_) T(std::move(other.value()));
					hasValue_ = true;
					other.reset();
				}
			}
			return *this;
		}

		explicit operator bool() const { return hasValue_; }
		bool has_value() const { return hasValue_; }

		T& value() {
			if (!hasValue_) {
				throw std::runtime_error("Optional has no value");
			}
			return *reinterpret_cast<T*>(storage_);
		}

		const T& value() const {
			if (!hasValue_) {
				throw std::runtime_error("Optional has no value");
			}
			return *reinterpret_cast<const T*>(storage_);
		}

		T& operator*() { return value(); }
		const T& operator*() const { return value(); }

		T* operator->() { return &value(); }
		const T* operator->() const { return &value(); }

		void reset() {
			if (hasValue_) {
				reinterpret_cast<T*>(storage_)->~T();
				hasValue_ = false;
			}
		}

	private:
		alignas(T) unsigned char storage_[sizeof(T)];
		bool hasValue_;
	};

} // namespace vcs
