// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pla {
// Instance-owned immutable asset storage. The caller loads outside the cache,
// validates success before Insert, and supplies retained payload bytes (including
// vector capacity). Keys identify immutable ROMFS content and its transformation.
// Never insert guest snapshots or failed/empty loads. Clear on ROMFS replacement.
// The byte bound covers cache-owned payload, not caller-held handles, temporary
// decoder allocations, or container overhead; entry_limit bounds metadata count.
template <class T> class AssetCache {
public:
    using Handle = std::shared_ptr<const T>;
    explicit AssetCache(size_t byte_limit, size_t entry_limit = 64)
        : byte_limit_(byte_limit), entry_limit_(entry_limit) {}
    AssetCache(const AssetCache &) = delete;
    AssetCache &operator=(const AssetCache &) = delete;

    Handle Find(const std::string &key) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(key);
        if (it == entries_.end()) return {};
        recent_.splice(recent_.begin(), recent_, it->second.recent);
        return it->second.value;
    }
    // A valid asset larger than the budget is returned for this request only.
    // Zero-byte inputs are refused and are never negative-cached.
    Handle Insert(std::string key, Handle value_ptr, size_t retained_bytes) {
        if (!value_ptr || !retained_bytes || key.empty()) return {};
        std::lock_guard lock(mutex_);
        const auto existing = entries_.find(key);
        if (existing != entries_.end()) Erase(existing);
        if (!entry_limit_ || retained_bytes > byte_limit_ || key.size() > 512) return value_ptr;
        while (!recent_.empty() &&
               (entries_.size() >= entry_limit_ || bytes_ > byte_limit_ - retained_bytes))
            Erase(entries_.find(recent_.back()));
        recent_.push_front(std::move(key));
        entries_.emplace(recent_.front(), Entry{value_ptr, retained_bytes, recent_.begin()});
        bytes_ += retained_bytes;
        return value_ptr;
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
        recent_.clear();
        bytes_ = 0;
    }
    size_t Bytes() const { std::lock_guard lock(mutex_); return bytes_; }
    size_t Size() const { std::lock_guard lock(mutex_); return entries_.size(); }

private:
    struct Entry {
        Handle value;
        size_t bytes;
        typename std::list<std::string>::iterator recent;
    };
    using Entries = std::unordered_map<std::string, Entry>;
    void Erase(typename Entries::iterator it) {
        bytes_ -= it->second.bytes;
        recent_.erase(it->second.recent);
        entries_.erase(it);
    }
    size_t byte_limit_, entry_limit_, bytes_ = 0;
    mutable std::mutex mutex_;
    std::list<std::string> recent_;
    Entries entries_;
};
} // namespace pla
