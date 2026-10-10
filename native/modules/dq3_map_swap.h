// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Flash-free swaps of the Map page image (Dragon Quest III HD-2D Remake companion, 0.10.2).
//
// Root cause of the owner's "when moving, map flashing" (research runs/map-stable-0.10.2/RESULT.md): the
// location marker is drawn into the map image, so dq3.m.img gets a new module key whenever the marker's
// pixel position, the facing or the field camera window changes. The host's image widget draws nothing
// while a module key is not in its cache (mod_ui.cpp DrawImage: `if (image == nullptr) return;`; module
// images are composed on the asset worker and installed by DrainModuleImages at the start of a later tick),
// so every step showed the map box empty for one or more frames, and on a continuous walk new keys were
// queued faster than they landed.
//
// The fix keeps the shown key on a picture the host already has: the wanted key is first published to a
// hidden prefetch widget (dq3.m.img.next, 1 px, alpha 0, under the map frame), and the visible widget
// moves to it only after the module's own load_image composed it (the host caches what load_image
// returns) plus two samples, so DrainModuleImages has installed it. One request is in flight at a time, so
// a walk never queues stale positions; the shown map lags the game by about one compose. A box change
// (normal <-> full map: another widget, another size) and the first picture switch at once.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace dq3 {

/// Map keys load_image composed (or failed to compose) recently: written on the asset worker, read on the
/// tick thread. Bounded by the pictures' bytes: the newest entries up to 32 MB (of the host's 64 MB module image
/// budget, the rest left to the cards, art and battle pictures), so a key noted Ready is still in the host's LRU
/// cache; the module clears it when a battle starts or the map state resets (battle art can take the budget).
class MapImageRegistry {
public:
    enum class State { Unknown, Ready, Failed };
    static constexpr std::size_t BudgetBytes = std::size_t{32} << 20;
    static constexpr std::size_t MaxEntries = 64;

    void Note(std::string_view key, bool ok, std::size_t bytes = 0) {
        std::scoped_lock lock{mutex_};
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const Entry& e) { return e.key == key; });
        if (it != entries_.end()) {
            total_ -= it->bytes;
            entries_.erase(it);
        }
        entries_.push_back({std::string{key}, ok, ok ? bytes : 0});
        total_ += entries_.back().bytes;
        while (entries_.size() > 1 && (total_ > BudgetBytes || entries_.size() > MaxEntries)) {
            total_ -= entries_.front().bytes;
            entries_.pop_front();
        }
    }
    State Get(std::string_view key) const {
        std::scoped_lock lock{mutex_};
        for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
            if (it->key == key)
                return it->ok ? State::Ready : State::Failed;
        return State::Unknown;
    }
    void Clear() {
        std::scoped_lock lock{mutex_};
        entries_.clear();
        total_ = 0;
    }
    std::size_t Bytes() const {
        std::scoped_lock lock{mutex_};
        return total_;
    }

private:
    struct Entry {
        std::string key;
        bool ok{};
        std::size_t bytes{};
    };
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    std::size_t total_{};
};

/// The map box of a mapimg / fieldimg key ("<w>x<h>": mapimg/<pin>/<box>/..., fieldimg/<src>/<v>/<box>/...),
/// "" when the key is neither.
inline std::string_view MapKeyBox(std::string_view key) {
    constexpr std::string_view prefix = "module:dq3:";
    if (key.starts_with(prefix))
        key.remove_prefix(prefix.size());
    int skip = 0;
    if (key.starts_with("mapimg/"))
        skip = 2;
    else if (key.starts_with("fieldimg/"))
        skip = 3;
    else
        return {};
    for (int k = 0; k < skip; ++k) {
        const auto slash = key.find('/');
        if (slash == std::string_view::npos)
            return {};
        key.remove_prefix(slash + 1);
    }
    const auto end = key.find_first_of("/~");
    return key.substr(0, end);
}

/// The swap state of the Map page image (tick thread).
class MapImageSwap {
public:
    static constexpr std::uint64_t SettleSamples = 2;   ///< samples between compose and the switch
    /// a request with no load_image news (0.75 s): the host already held the key (older than the registry) or its
    /// worker is stuck; far above a compose (desktop < 15 ms, a first decode of a map texture ~150 ms)
    static constexpr std::uint64_t TimeoutSamples = 45;

    struct Out {
        std::string shown;   ///< dq3.m.img ("" = none)
        std::string request; ///< dq3.m.img.next ("" = none)
    };

    /// One sample: `want` is the key the page wants now ("" = no image / the Map tab is closed).
    template <class ReadyFn>
    Out Step(const std::string& want, std::uint64_t sample, ReadyFn&& state) {
        if (want.empty()) {
            Reset();
            return {};
        }
        if (shown_.empty() || MapKeyBox(shown_) != MapKeyBox(want)) {
            // nothing on screen yet, or another box (normal <-> full map): no old picture to keep
            shown_ = want;
            request_.clear();
            return {shown_, {}};
        }
        if (!request_.empty()) {
            const MapImageRegistry::State st = state(request_);
            if (st == MapImageRegistry::State::Ready) {
                if (!ready_at_)
                    ready_at_ = sample;
                if (sample >= ready_at_ + SettleSamples)
                    Settle();
            } else if (st == MapImageRegistry::State::Failed || sample >= since_ + TimeoutSamples) {
                Settle(); // no better picture is coming: behave as before (the key shows when it can)
            }
        }
        if (request_.empty() && want != shown_) {
            if (state(want) == MapImageRegistry::State::Ready) {
                // composed earlier (walking back to a position): the host still holds it
                request_ = want;
                since_ = sample;
                ready_at_ = sample >= SettleSamples ? sample - SettleSamples : 0;
                Settle();
            } else {
                request_ = want;
                since_ = sample;
                ready_at_ = 0;
            }
        }
        return {shown_, request_};
    }
    void Reset() {
        shown_.clear();
        request_.clear();
        since_ = ready_at_ = 0;
    }
    const std::string& shown() const { return shown_; }
    const std::string& request() const { return request_; }
    std::uint64_t swaps() const { return swaps_; }

private:
    void Settle() {
        shown_ = request_;
        request_.clear();
        ready_at_ = 0;
        ++swaps_;
    }
    std::string shown_, request_;
    std::uint64_t since_{}, ready_at_{}, swaps_{};
};

} // namespace dq3
