#pragma once

#include "vcs_export.h"
#include "Optional.h"
#include "ParamValue.h"

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>

namespace vcs {

// Response status codes
enum class ResponseCode : int {
    Success = 0,
    InvalidParams = 400,
    NotFound = 404,
    Conflict = 409,
    InvalidOperation = 422,
    InternalError = 500,
    NotImplemented = 501
};

// Response result
class VCS_BASE_API Response {
public:
    Response();
    Response(ResponseCode code, const std::string& message);

    static Response Success(const std::string& message = "OK");
    static Response Error(ResponseCode code, const std::string& message);

    bool IsSuccess() const;
    ResponseCode GetCode() const;
    // [V7.0] Renamed from GetMessage() to GetMsg() to avoid collision with
    // the VS2017 system-provided GetMessage() macro on Windows.
    const std::string& GetMsg() const;

private:
    ResponseCode code_;
    std::string message_;
};

// Response with data
template<typename T>
class DataResponse : public Response {
public:
    DataResponse() : Response() {}
    DataResponse(ResponseCode code, const std::string& message)
        : Response(code, message) {}

    static DataResponse<T> Success(const T& data) {
        DataResponse<T> resp;
        resp.SetData(data);
        return resp;
    }

    const T& GetData() const { return data_; }
    T& GetData() { return data_; }
    void SetData(const T& data) { data_ = data; }

private:
    T data_;
};

// Parameter value type
using ParamValue = ParamVariant;

// Parameter container
class VCS_BASE_API Params {
public:
    Params() = default;

    void SetInt(const std::string& key, int64_t value);
    void SetDouble(const std::string& key, double value);
    void SetString(const std::string& key, const std::string& value);
    void SetBool(const std::string& key, bool value);
    void SetIntArray(const std::string& key, const std::vector<int64_t>& value);
    void SetStringArray(const std::string& key, const std::vector<std::string>& value);

    void SetSharedPtr(const std::string& key, std::shared_ptr<void> value);
    void SetSharedPtr(const std::string& key, std::weak_ptr<void> value);

    Optional<int64_t> GetInt(const std::string& key) const;
    Optional<double> GetDouble(const std::string& key) const;
    Optional<std::string> GetString(const std::string& key) const;
    Optional<bool> GetBool(const std::string& key) const;
    Optional<std::vector<int64_t>> GetIntArray(const std::string& key) const;
    Optional<std::vector<std::string>> GetStringArray(const std::string& key) const;

    template<typename T>
    Optional<std::shared_ptr<T>> GetSharedPtr(const std::string& key) const {
        auto it = sharedPtrs_.find(key);
        if (it != sharedPtrs_.end()) {
            return Optional<std::shared_ptr<T>>(std::static_pointer_cast<T>(it->second));
        }
        return Optional<std::shared_ptr<T>>();
    }

    std::shared_ptr<void> GetSharedPtr(const std::string& key) const {
        auto it = sharedPtrs_.find(key);
        if (it != sharedPtrs_.end()) {
            return it->second;
        }
        return std::shared_ptr<void>();
    }

    bool HasKey(const std::string& key) const;
    std::vector<std::string> GetKeys() const;
    std::string ToString() const;
    static Params FromString(const std::string& str);

private:
    std::unordered_map<std::string, ParamValue> params_;
    std::unordered_map<std::string, std::shared_ptr<void>> sharedPtrs_;
};

} // namespace vcs
