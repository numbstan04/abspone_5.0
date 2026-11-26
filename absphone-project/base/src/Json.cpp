#include "Json.h"

#include <map>
#include <string>
#include <utility>

namespace vcs { namespace base {

// ============================================================================
// Json::Impl
// ============================================================================

struct Json::Impl {
    enum class Type { String, Int, Bool };

    struct Value {
        Type type = Type::String;
        std::string s;
        int64_t i = 0;
        bool b = false;
    };

    std::map<std::string, Value> values;
};

// ============================================================================
// Anonymous helpers
// ============================================================================
namespace {

void SkipWhitespace(const std::string& text, size_t& i) {
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' ||
            text[i] == '\n' || text[i] == '\r')) {
        ++i;
    }
}

bool ParseString(const std::string& text, size_t& i, std::string& out) {
    out.clear();
    if (i >= text.size() || text[i] != '"') {
        return false;
    }
    ++i;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\') {
            ++i;
            if (i >= text.size()) {
                return false;
            }
            switch (text[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                default:   out += text[i]; break;
            }
            ++i;
        } else if (c == '"') {
            ++i;
            return true;
        } else {
            out += c;
            ++i;
        }
    }
    return false;
}

std::string Escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) {
                    out += c;
                }
                break;
        }
    }
    return out;
}

bool ParseInt(const std::string& text, size_t& i, int64_t& out) {
    const size_t start = i;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    int64_t n = 0;
    bool got = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        n = n * 10 + (text[i] - '0');
        ++i;
        got = true;
    }
    if (!got) {
        i = start;
        return false;
    }
    out = negative ? -n : n;
    return true;
}

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

Json::Json()
    : impl_(new Impl()) {
}

Json::~Json() = default;

Json::Json(Json&& other) noexcept
    : impl_(std::move(other.impl_)) {
}

Json& Json::operator=(Json&& other) noexcept {
    if (this != &other) {
        impl_ = std::move(other.impl_);
    }
    return *this;
}

// ============================================================================
// Set
// ============================================================================

Json& Json::Set(const std::string& key, const std::string& value) {
    Impl::Value v;
    v.type = Impl::Type::String;
    v.s = value;
    impl_->values[key] = std::move(v);
    return *this;
}

Json& Json::Set(const std::string& key, const char* value) {
    return Set(key, std::string(value ? value : ""));
}

Json& Json::Set(const std::string& key, int64_t value) {
    Impl::Value v;
    v.type = Impl::Type::Int;
    v.i = value;
    impl_->values[key] = std::move(v);
    return *this;
}

Json& Json::Set(const std::string& key, int value) {
    return Set(key, static_cast<int64_t>(value));
}

Json& Json::Set(const std::string& key, uint32_t value) {
    return Set(key, static_cast<int64_t>(value));
}

Json& Json::Set(const std::string& key, bool value) {
    Impl::Value v;
    v.type = Impl::Type::Bool;
    v.b = value;
    impl_->values[key] = std::move(v);
    return *this;
}

// ============================================================================
// Get
// ============================================================================

std::string Json::GetString(const std::string& key,
                            const std::string& defaultValue) const {
    const auto it = impl_->values.find(key);
    if (it == impl_->values.end() ||
        it->second.type != Impl::Type::String) {
        return defaultValue;
    }
    return it->second.s;
}

int64_t Json::GetInt(const std::string& key, int64_t defaultValue) const {
    const auto it = impl_->values.find(key);
    if (it == impl_->values.end() ||
        it->second.type != Impl::Type::Int) {
        return defaultValue;
    }
    return it->second.i;
}

bool Json::GetBool(const std::string& key, bool defaultValue) const {
    const auto it = impl_->values.find(key);
    if (it == impl_->values.end() ||
        it->second.type != Impl::Type::Bool) {
        return defaultValue;
    }
    return it->second.b;
}

// ============================================================================
// Introspection
// ============================================================================

bool Json::HasKey(const std::string& key) const {
    return impl_->values.find(key) != impl_->values.end();
}

void Json::Clear() {
    impl_->values.clear();
}

// ============================================================================
// Serialization
// ============================================================================

std::string Json::Dump() const {
    std::string out = "{";
    bool first = true;
    for (const auto& kv : impl_->values) {
        if (!first) {
            out += ",";
        }
        first = false;

        out += "\"";
        out += Escape(kv.first);
        out += "\":";

        switch (kv.second.type) {
            case Impl::Type::String:
                out += "\"";
                out += Escape(kv.second.s);
                out += "\"";
                break;
            case Impl::Type::Int:
                out += std::to_string(kv.second.i);
                break;
            case Impl::Type::Bool:
                out += kv.second.b ? "true" : "false";
                break;
        }
    }
    out += "}";
    return out;
}

bool Json::Parse(const std::string& text, Json& out) {
    out.Clear();

    size_t i = 0;
    SkipWhitespace(text, i);
    if (i >= text.size() || text[i] != '{') {
        return false;
    }
    ++i;
    SkipWhitespace(text, i);

    if (i < text.size() && text[i] == '}') {
        return true;  // empty object
    }

    while (i < text.size()) {
        SkipWhitespace(text, i);

        std::string key;
        if (!ParseString(text, i, key)) {
            return false;
        }

        SkipWhitespace(text, i);
        if (i >= text.size() || text[i] != ':') {
            return false;
        }
        ++i;
        SkipWhitespace(text, i);

        if (i >= text.size()) {
            return false;
        }

        if (text[i] == '"') {
            std::string s;
            if (!ParseString(text, i, s)) {
                return false;
            }
            out.Set(key, s);
        } else if (text.compare(i, 4, "true") == 0) {
            out.Set(key, true);
            i += 4;
        } else if (text.compare(i, 5, "false") == 0) {
            out.Set(key, false);
            i += 5;
        } else {
            int64_t n = 0;
            if (!ParseInt(text, i, n)) {
                return false;
            }
            out.Set(key, n);
        }

        SkipWhitespace(text, i);
        if (i >= text.size()) {
            return false;
        }
        if (text[i] == ',') {
            ++i;
            continue;
        }
        if (text[i] == '}') {
            return true;
        }
        return false;
    }
    return false;
}

}} // namespace vcs::base
