#include "vcs_export.h"
#include "Response.h"
#include <unordered_map>
#include <string>
#include <iterator>

namespace vcs {

using namespace std;

// ========== Response 实现 ==========

Response::Response()
    : code_(ResponseCode::Success), message_("OK") {
}

Response::Response(ResponseCode code, const std::string& message)
    : code_(code), message_(message) {
}

Response Response::Success(const std::string& message) {
    return Response(ResponseCode::Success, message.empty() ? "OK" : message);
}

Response Response::Error(ResponseCode code, const std::string& message) {
    return Response(code, message);
}

bool Response::IsSuccess() const {
    return code_ == ResponseCode::Success;
}

ResponseCode Response::GetCode() const {
    return code_;
}

// [V7.0] Renamed from GetMessage() to GetMsg() to avoid collision with
// the VS2017 system-provided GetMessage() macro on Windows.
const std::string& Response::GetMsg() const {
    return message_;
}

// ========== Params 实现 ==========

void Params::SetInt(const std::string& key, int64_t value) {
    params_[key] = ParamVariant(value);
}

void Params::SetDouble(const std::string& key, double value) {
    params_[key] = ParamVariant(value);
}

void Params::SetString(const std::string& key, const std::string& value) {
    params_[key] = ParamVariant(value);
}

void Params::SetBool(const std::string& key, bool value) {
    params_[key] = ParamVariant(value);
}

void Params::SetIntArray(const std::string& key, const std::vector<int64_t>& value) {
    params_[key] = ParamVariant(value);
}

void Params::SetStringArray(const std::string& key, const std::vector<std::string>& value) {
    params_[key] = ParamVariant(value);
}

void Params::SetSharedPtr(const std::string& key, std::shared_ptr<void> value) {
    sharedPtrs_[key] = std::move(value);
}

void Params::SetSharedPtr(const std::string& key, std::weak_ptr<void> value) {
    if (auto locked = value.lock()) {
        sharedPtrs_[key] = std::move(locked);
    }
}

Optional<int64_t> Params::GetInt(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_INT64(it->second)) {
        return Optional<int64_t>(*(it->second.GetInt64()));
    }
    return Optional<int64_t>();
}

Optional<double> Params::GetDouble(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_DOUBLE(it->second)) {
        return Optional<double>(*(it->second.GetDouble()));
    }
    return Optional<double>();
}

Optional<std::string> Params::GetString(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_STRING(it->second)) {
        return Optional<std::string>(*(it->second.GetString()));
    }
    return Optional<std::string>();
}

Optional<bool> Params::GetBool(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_BOOL(it->second)) {
        return Optional<bool>(*(it->second.GetBool()));
    }
    return Optional<bool>();
}

Optional<std::vector<int64_t>> Params::GetIntArray(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_INT64_ARRAY(it->second)) {
        return Optional<std::vector<int64_t>>(*(it->second.GetInt64Array()));
    }
    return Optional<std::vector<int64_t> >();
}

Optional<std::vector<std::string>> Params::GetStringArray(const std::string& key) const {
    auto it = params_.find(key);
    if (it != params_.end() && VCS_HOLDS_STRING_ARRAY(it->second)) {
        return Optional<std::vector<std::string>>(*(it->second.GetStringArray()));
    }
    return Optional<std::vector<std::string> >();
}

bool Params::HasKey(const std::string& key) const {
    return params_.find(key) != params_.end();
}

std::vector<std::string> Params::GetKeys() const {
    std::vector<std::string> keys;
    for (const auto& pair : params_) {
        keys.push_back(pair.first);
    }
    return keys;
}

std::string Params::ToString() const {
    std::string result = "{";
    bool first = true;
    for (const auto& pair : params_) {
        if (!first) result += ", ";
        first = false;
        result += "\"" + pair.first + "\": ";
        if (VCS_HOLDS_INT64(pair.second)) {
            result += std::to_string(*pair.second.GetInt64());
        } else if (VCS_HOLDS_DOUBLE(pair.second)) {
            result += std::to_string(*pair.second.GetDouble());
        } else if (VCS_HOLDS_STRING(pair.second)) {
            result += "\"" + *pair.second.GetString() + "\"";
        } else if (VCS_HOLDS_BOOL(pair.second)) {
            result += *pair.second.GetBool() ? "true" : "false";
        }
    }
    result += "}";
    return result;
}

Params Params::FromString(const std::string& str) {
    // 简化实现
    (void)str;
    return Params();
}

} // namespace vcs
