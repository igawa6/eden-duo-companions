// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_mapbuilder.h"
#include <algorithm>
#include <chrono>
namespace xcx {
MapBuilder::~MapBuilder() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        cancel_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
void MapBuilder::Want(const EdenDsmodHostApi &h, std::uint32_t zone, const std::vector<std::uint8_t> &flags,
                      const std::vector<tiles::Key> &wanted) {
    bool changed = false;
    {
        std::scoped_lock lock(mutex_);
        host_ = h;
        have_host_ = true;
        const bool same_context = zone == zone_ && flags == flags_;
        if (!same_context) {
            zone_ = zone;
            flags_ = flags;
        }
        std::vector<std::string> names;
        for (const auto &k : wanted)
            names.push_back(tiles::KeyText(k));
        std::vector<std::string> before;
        for (const auto &k : wanted_)
            before.push_back(tiles::KeyText(k));
        changed = names != before || !same_context;
        if (changed)
            wanted_ = wanted;
        if (!thread_.joinable() && !wanted.empty())
            thread_ = std::thread([this] { Loop(); });
    }
    if (changed)
        wake_.notify_all();
}
bool MapBuilder::Ready(const tiles::Key &k) const {
    std::scoped_lock lock(mutex_);
    return cache_.contains(tiles::KeyText(k));
}
std::shared_ptr<const Image> MapBuilder::Get(const tiles::Key &k) const {
    std::scoped_lock lock(mutex_);
    const auto it = cache_.find(tiles::KeyText(k));
    if (it == cache_.end())
        return nullptr;
    it->second.used = ++tick_;
    return it->second.image;
}
std::size_t MapBuilder::CachedBytes() const {
    std::scoped_lock lock(mutex_);
    std::size_t n = 0;
    for (const auto &[name, slot] : cache_)
        n += slot.image ? slot.image->rgba.size() : 0;
    return n;
}
void MapBuilder::WaitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait_for(lock, std::chrono::seconds(300), [this] {
        if (running_)
            return false;
        for (const auto &k : wanted_)
            if (!cache_.contains(tiles::KeyText(k)) &&
                (!retry_after_.contains(tiles::KeyText(k)) || retry_after_[tiles::KeyText(k)] < std::chrono::steady_clock::now()))
                return false;
        return true;
    });
}
void MapBuilder::Loop() {
    // A low-priority background thread: the game and the host keep the CPU they need.
    std::unique_lock lock(mutex_);
    while (!stop_) {
        // The most important wanted picture that is neither built nor waiting out a failure.
        const tiles::Key *next = nullptr;
        std::string name;
        const auto now = std::chrono::steady_clock::now();
        for (const auto &k : wanted_) {
            const auto n = tiles::KeyText(k);
            if (cache_.contains(n))
                continue;
            if (const auto r = retry_after_.find(n); r != retry_after_.end() && r->second > now)
                continue;
            next = &k;
            name = n;
            break;
        }
        if (!next) {
            running_ = false;
            idle_.notify_all();
            wake_.wait_for(lock, std::chrono::seconds(2));
            continue;
        }
        const tiles::Key key = *next;
        const auto host = host_;
        const auto zone = zone_;
        const auto flags = flags_;
        running_ = true;
        lock.unlock();
        Image picture;
        try {
            picture = renderer_.MapPicture(host, zone, flags, key);
        } catch (...) {
            picture = {};
        }
        lock.lock();
        running_ = false;
        if (picture.Empty()) {
            retry_after_[name] = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        } else {
            cache_[name] = Slot{std::make_shared<const Image>(std::move(picture)), ++tick_};
            // Over budget: drop the least recently used pictures that are not wanted.
            std::size_t total = 0;
            for (const auto &[n, slot] : cache_)
                total += slot.image->rgba.size();
            while (total > BudgetBytes) {
                auto victim = cache_.end();
                for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                    const bool wanted = std::any_of(wanted_.begin(), wanted_.end(), [&](const tiles::Key &k) { return tiles::KeyText(k) == it->first; });
                    if (!wanted && (victim == cache_.end() || it->second.used < victim->second.used))
                        victim = it;
                }
                if (victim == cache_.end())
                    break;
                total -= victim->second.image->rgba.size();
                cache_.erase(victim);
            }
        }
        idle_.notify_all();
    }
}
} // namespace xcx
