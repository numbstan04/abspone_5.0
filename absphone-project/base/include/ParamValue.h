#pragma once

// 调用参数取值类型：Params / Response 的载荷（键 -> 值）。

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace vcs {

	enum ParamType {
		ParamType_None = 0,
		ParamType_Int64 = 1,
		ParamType_Double = 2,
		ParamType_String = 3,
		ParamType_Bool = 4,
		ParamType_Int64Array = 5,
		ParamType_StringArray = 6
	};

	class ParamVariant {
	public:
		ParamVariant() : type_(ParamType_None) {}

		explicit ParamVariant(int64_t val) : type_(ParamType_Int64) {
			new (&data_.int64_val) int64_t(val);
		}

		explicit ParamVariant(double val) : type_(ParamType_Double) {
			new (&data_.double_val) double(val);
		}

		explicit ParamVariant(const std::string& val) : type_(ParamType_String) {
			new (&data_.string_val) std::string(val);
		}

		explicit ParamVariant(bool val) : type_(ParamType_Bool) {
			data_.bool_val = val;
		}

		explicit ParamVariant(const std::vector<int64_t>& val) : type_(ParamType_Int64Array) {
			new (&data_.int64_arr_val) std::vector<int64_t>(val);
		}

		explicit ParamVariant(const std::vector<std::string>& val) : type_(ParamType_StringArray) {
			new (&data_.string_arr_val) std::vector<std::string>(val);
		}

		~ParamVariant() { destroy(); }

		ParamVariant(const ParamVariant& other) : type_(other.type_) {
			copy_from(other);
		}

		ParamVariant& operator=(const ParamVariant& other) {
			if (this != &other) {
				destroy();
				type_ = other.type_;
				copy_from(other);
			}
			return *this;
		}

		ParamVariant(ParamVariant&& other) : type_(other.type_) {
			move_from(other);
			other.type_ = ParamType_None;
		}

		ParamVariant& operator=(ParamVariant&& other) {
			if (this != &other) {
				destroy();
				type_ = other.type_;
				move_from(other);
				other.type_ = ParamType_None;
			}
			return *this;
		}

		ParamType GetType() const { return type_; }

		int64_t* GetInt64() {
			if (type_ == ParamType_Int64) return &data_.int64_val;
			return NULL;
		}
		const int64_t* GetInt64() const {
			if (type_ == ParamType_Int64) return &data_.int64_val;
			return NULL;
		}

		double* GetDouble() {
			if (type_ == ParamType_Double) return &data_.double_val;
			return NULL;
		}
		const double* GetDouble() const {
			if (type_ == ParamType_Double) return &data_.double_val;
			return NULL;
		}

		std::string* GetString() {
			if (type_ == ParamType_String) return &data_.string_val;
			return NULL;
		}
		const std::string* GetString() const {
			if (type_ == ParamType_String) return &data_.string_val;
			return NULL;
		}

		bool* GetBool() {
			if (type_ == ParamType_Bool) return &data_.bool_val;
			return NULL;
		}
		const bool* GetBool() const {
			if (type_ == ParamType_Bool) return &data_.bool_val;
			return NULL;
		}

		std::vector<int64_t>* GetInt64Array() {
			if (type_ == ParamType_Int64Array) return &data_.int64_arr_val;
			return NULL;
		}
		const std::vector<int64_t>* GetInt64Array() const {
			if (type_ == ParamType_Int64Array) return &data_.int64_arr_val;
			return NULL;
		}

		std::vector<std::string>* GetStringArray() {
			if (type_ == ParamType_StringArray) return &data_.string_arr_val;
			return NULL;
		}
		const std::vector<std::string>* GetStringArray() const {
			if (type_ == ParamType_StringArray) return &data_.string_arr_val;
			return NULL;
		}

	private:
		ParamType type_;

		union DataUnion {
			int64_t int64_val;
			double double_val;
			std::string string_val;
			bool bool_val;
			std::vector<int64_t> int64_arr_val;
			std::vector<std::string> string_arr_val;

			DataUnion() {}
			~DataUnion() {}
		} data_;

		void destroy() {
			if (type_ == ParamType_String) {
				data_.string_val.~basic_string();
			} else if (type_ == ParamType_Int64Array) {
				data_.int64_arr_val.~vector();
			} else if (type_ == ParamType_StringArray) {
				data_.string_arr_val.~vector();
			}
			type_ = ParamType_None;
		}

		void copy_from(const ParamVariant& other) {
			switch (other.type_) {
				case ParamType_Int64: data_.int64_val = other.data_.int64_val; break;
				case ParamType_Double: data_.double_val = other.data_.double_val; break;
				case ParamType_String: new (&data_.string_val) std::string(other.data_.string_val); break;
				case ParamType_Bool: data_.bool_val = other.data_.bool_val; break;
				case ParamType_Int64Array: new (&data_.int64_arr_val) std::vector<int64_t>(other.data_.int64_arr_val); break;
				case ParamType_StringArray: new (&data_.string_arr_val) std::vector<std::string>(other.data_.string_arr_val); break;
				default: break;
			}
		}

		void move_from(ParamVariant& other) {
			switch (other.type_) {
				case ParamType_Int64: data_.int64_val = other.data_.int64_val; break;
				case ParamType_Double: data_.double_val = other.data_.double_val; break;
				case ParamType_String: new (&data_.string_val) std::string(std::move(other.data_.string_val)); break;
				case ParamType_Bool: data_.bool_val = other.data_.bool_val; break;
				case ParamType_Int64Array: new (&data_.int64_arr_val) std::vector<int64_t>(std::move(other.data_.int64_arr_val)); break;
				case ParamType_StringArray: new (&data_.string_arr_val) std::vector<std::string>(std::move(other.data_.string_arr_val)); break;
				default: break;
			}
		}
	};

	inline bool VCS_HOLDS_INT64(const ParamVariant& v) {
		return v.GetType() == ParamType_Int64;
	}
	inline bool VCS_HOLDS_DOUBLE(const ParamVariant& v) {
		return v.GetType() == ParamType_Double;
	}
	inline bool VCS_HOLDS_STRING(const ParamVariant& v) {
		return v.GetType() == ParamType_String;
	}
	inline bool VCS_HOLDS_BOOL(const ParamVariant& v) {
		return v.GetType() == ParamType_Bool;
	}
	inline bool VCS_HOLDS_INT64_ARRAY(const ParamVariant& v) {
		return v.GetType() == ParamType_Int64Array;
	}
	inline bool VCS_HOLDS_STRING_ARRAY(const ParamVariant& v) {
		return v.GetType() == ParamType_StringArray;
	}

} // namespace vcs
