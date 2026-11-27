// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Background renderer for the Skell stills: one job at a time on its own low-priority thread,
// started only when a frame (or its paint) is first wanted, results cached by frame id. Nothing
// runs on the module's tick; the tick only asks and looks.
#include "xcx_still.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
namespace xcx {
struct StillRequest {
    unsigned frame{}; // table-130 frame row (Member::frame)
    // Player paint (saved record groups 0-3 -> paint slots 0-3); the job fills glow slots 4-5 from
    // the factory table. Absent = factory paint.
    std::optional<StillPalette> paint;
    // The record's weapon item ids (Member::weapons). When set, the
    // equipped weapons are drawn stowed on the frame (see xcx_weapons.h).
    std::optional<WeaponItems> weapons;
};
class StillCache {
  public:
    struct Stats {
        unsigned renders{}, failures{}, cancelled{};
        double last_ms{};
        std::size_t last_peak_bytes{};
    };
    StillCache() = default;
    StillCache(const StillCache &) = delete;
    StillCache &operator=(const StillCache &) = delete;
    ~StillCache();
    // Tick thread. The wanted set replaces the previous one (most important first). A running job
    // whose frame is no longer wanted is cancelled; finished ones stay cached (LRU, 4 stills).
    void Want(const EdenDsmodHostApi &, const std::vector<StillRequest> &);
    // Any thread. Null until the still has landed.
    std::shared_ptr<const Image> Get(const StillRequest &) const;
    // Drop everything and stop the running job (context lost / host revoked).
    void Clear();
    Stats GetStats() const;
    // Test hook: block until no job is pending or running (never used by the module).
    void WaitIdle();

  private:
    struct Key {
        unsigned frame;
        std::uint64_t paint;
        bool operator<(const Key &o) const {
            return frame != o.frame ? frame < o.frame : paint < o.paint;
        }
        bool operator==(const Key &o) const { return frame == o.frame && paint == o.paint; }
    };
    struct Slot {
        std::shared_ptr<const Image> image;
        std::uint64_t used{};
    };
    static Key KeyOf(const StillRequest &);
    void Loop();
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::thread thread_;
    EdenDsmodHostApi host_{};
    bool have_host_{};
    bool stop_{};
    std::vector<StillRequest> wanted_;
    mutable std::map<Key, Slot> cache_;
    std::map<Key, unsigned> attempts_;
    std::map<Key, std::chrono::steady_clock::time_point> retry_after_;
    bool running_{};
    Key running_key_{};
    std::atomic<bool> cancel_{false};
    mutable std::uint64_t tick_{};
    Stats stats_;
};
} // namespace xcx
