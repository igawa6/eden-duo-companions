// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "xcx_still_cache.h"
#include <algorithm>
#include <cstdio>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
namespace xcx {
namespace {
constexpr std::size_t CacheSlots = 4;
constexpr unsigned MaxAttempts = 3;
constexpr auto RetryDelay = std::chrono::seconds(8);
// The transient budget for one job (decoded textures, buffers, scene). Logged when exceeded.
constexpr std::size_t TransientBudget = 64u * 1024 * 1024;
void Log(const EdenDsmodHostApi &host, std::uint32_t level, const std::string &text) {
    if (host.log)
        host.log(host.userdata, level, text.c_str());
}
} // namespace
StillCache::Key StillCache::KeyOf(const StillRequest &r) {
    std::uint64_t h = 0;
    if (r.paint) {
        h = 14695981039346656037ULL;
        for (const auto &c : r.paint->rgb)
            for (const auto v : c) {
                h ^= v;
                h *= 1099511628211ULL;
            }
        h |= 1;
    }
    if (r.weapons) {
        std::uint64_t w = 1099511628211ULL;
        for (const auto v : *r.weapons)
            w = (w ^ v) * 14695981039346656037ULL;
        h ^= w << 1;
        h |= 2;
    }
    return {r.frame, h};
}
StillCache::~StillCache() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        cancel_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}
void StillCache::Want(const EdenDsmodHostApi &host, const std::vector<StillRequest> &wanted) {
    bool start = false;
    {
        std::scoped_lock lock(mutex_);
        if (stop_)
            return;
        host_ = host;
        have_host_ = true;
        wanted_ = wanted;
        if (running_ &&
            std::none_of(wanted_.begin(), wanted_.end(),
                         [&](const StillRequest &r) { return KeyOf(r) == running_key_; }))
            cancel_ = true; // the frame changed: stop the stale job, never publish it
        if (!thread_.joinable() && !wanted_.empty())
            start = true;
        if (start)
            thread_ = std::thread(&StillCache::Loop, this);
    }
    wake_.notify_all();
}
std::shared_ptr<const Image> StillCache::Get(const StillRequest &request) const {
    std::scoped_lock lock(mutex_);
    const auto it = cache_.find(KeyOf(request));
    if (it == cache_.end())
        return nullptr;
    it->second.used = ++tick_;
    return it->second.image;
}
void StillCache::Clear() {
    {
        std::scoped_lock lock(mutex_);
        wanted_.clear();
        cache_.clear();
        attempts_.clear();
        retry_after_.clear();
        if (running_)
            cancel_ = true;
    }
    wake_.notify_all();
}
StillCache::Stats StillCache::GetStats() const {
    std::scoped_lock lock(mutex_);
    return stats_;
}
void StillCache::WaitIdle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] {
        if (stop_ || !thread_.joinable())
            return true;
        if (running_)
            return false;
        for (const auto &r : wanted_) {
            const auto k = KeyOf(r);
            if (cache_.count(k))
                continue;
            // A failed frame waits out its retry delay: for the test hook that counts as idle.
            if (attempts_.count(k))
                continue;
            return false;
        }
        return true;
    });
}
void StillCache::Loop() {
#if defined(__linux__)
    // Courtesy to the game: a lower priority for this one-off background render.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
#endif
    std::unique_lock lock(mutex_);
    while (!stop_) {
        const auto now = std::chrono::steady_clock::now();
        const StillRequest *pick = nullptr;
        std::chrono::steady_clock::time_point earliest = now + std::chrono::hours(24);
        for (const auto &r : wanted_) {
            const auto k = KeyOf(r);
            if (cache_.count(k))
                continue;
            const auto a = attempts_.find(k);
            if (a != attempts_.end() && a->second >= MaxAttempts)
                continue;
            const auto t = retry_after_.find(k);
            if (t != retry_after_.end() && t->second > now) {
                earliest = std::min(earliest, t->second);
                continue;
            }
            pick = &r;
            break;
        }
        if (!pick || !have_host_) {
            running_ = false;
            idle_.notify_all();
            if (earliest > now + std::chrono::hours(23))
                wake_.wait(lock);
            else
                wake_.wait_until(lock, earliest);
            continue;
        }
        const StillRequest request = *pick;
        const Key key = KeyOf(request);
        const EdenDsmodHostApi host = host_;
        running_ = true;
        running_key_ = key;
        cancel_ = false;
        lock.unlock();
        Image image;
        StillStats stats;
        bool ok = false, cancelled = false;
        std::string error;
        try {
            Archive archive; // transient: the archive index lives only as long as the job
            const auto model = FrameModelForRow(archive, host, request.frame);
            StillOptions options;
            options.palette = request.paint;
            if (options.palette) {
                const auto factory = FactoryPalette(archive, host, model.name_id);
                options.palette->rgb[4] = factory.rgb[4];
                options.palette->rgb[5] = factory.rgb[5];
            }
            if (request.weapons)
                options.weapons = ResolveWeapons(archive, host, model, *request.weapons);
            image = RenderStill(archive, host, model, options, &cancel_, &stats);
            ok = true;
        } catch (const StillCancelled &) {
            cancelled = true;
        } catch (const std::exception &e) {
            error = e.what();
        } catch (...) {
            error = "unknown error";
        }
        lock.lock();
        running_ = false;
        const bool still_wanted =
            std::any_of(wanted_.begin(), wanted_.end(),
                        [&](const StillRequest &r) { return KeyOf(r) == key; });
        if (ok && still_wanted && !stop_) {
            ++stats_.renders;
            stats_.last_ms = stats.ms_total;
            stats_.last_peak_bytes = stats.tracked_peak_bytes();
            cache_[key] = {std::make_shared<const Image>(std::move(image)), ++tick_};
            while (cache_.size() > CacheSlots) {
                auto oldest = cache_.begin();
                for (auto it = cache_.begin(); it != cache_.end(); ++it)
                    if (it->second.used < oldest->second.used)
                        oldest = it;
                cache_.erase(oldest);
            }
            char text[200];
            std::snprintf(text, sizeof text,
                          "xcx still frame %u rendered in %.0f ms (decode %.0f raster %.0f shade "
                          "%.0f), job memory %.1f MiB%s",
                          request.frame, stats.ms_total, stats.ms_textures + stats.ms_geometry,
                          stats.ms_raster, stats.ms_shade,
                          stats.tracked_peak_bytes() / 1048576.0,
                          stats.tracked_peak_bytes() > TransientBudget ? " OVER BUDGET" : "");
            const std::string line = text;
            lock.unlock();
            Log(host, EDEN_DSMOD_LOG_INFO, line);
            lock.lock();
        } else if (cancelled || (ok && !still_wanted)) {
            ++stats_.cancelled;
        } else if (!ok) {
            ++stats_.failures;
            ++attempts_[key];
            retry_after_[key] = std::chrono::steady_clock::now() + RetryDelay;
            const std::string line = "xcx still frame " + std::to_string(request.frame) +
                                     " failed: " + error;
            lock.unlock();
            Log(host, EDEN_DSMOD_LOG_WARNING, line);
            lock.lock();
        }
        idle_.notify_all();
    }
    running_ = false;
    idle_.notify_all();
}
} // namespace xcx
