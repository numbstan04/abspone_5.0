#pragma once

#include "base_export.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vcs { namespace base {

/**
 * @brief Minimal JSON object for internal use.
 *
 * Supports flat objects with string / int64 / bool values only.
 * No nested objects, no arrays, no floating point. Extend when needed.
 *
 * Design notes:
 * - Pimpl hides the internal storage so the implementation can be
 *   replaced (e.g. by a third-party JSON library) without changing
 *   this header or any caller.
 * - Only the API used by the project is exposed.
 * - C++11 compatible.
 */
class VCS_BASE_API Json {
public:
    Json();
    ~Json();

    Json(const Json&) = delete;
    Json& operator=(const Json&) = delete;

    Json(Json&& other) noexcept;
    Json& operator=(Json&& other) noexcept;

    // ---- Set ----
    Json& Set(const std::string& key, const std::string& value);
    Json& Set(const std::string& key, const char* value);
    Json& Set(const std::string& key, int64_t value);
    Json& Set(const std::string& key, int value);
    Json& Set(const std::string& key, uint32_t value);
    Json& Set(const std::string& key, bool value);

    // ---- Get ----
    std::string GetString(const std::string& key,
                          const std::string& defaultValue = "") const;
    int64_t GetInt(const std::string& key,
                   int64_t defaultValue = 0) const;
    bool GetBool(const std::string& key,
                 bool defaultValue = false) const;

    // ---- Introspection ----
    bool HasKey(const std::string& key) const;
    void Clear();

    // ---- Serialization ----
    std::string Dump() const;

    /**
     * @brief Parse a JSON text into `out`.
     *
     * On success, `out` is fully replaced with the parsed contents.
     * On failure, `out` is left empty.
     *
     * @return true on success, false on malformed input.
     */
    static bool Parse(const std::string& text, Json& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}} // namespace vcs::base
