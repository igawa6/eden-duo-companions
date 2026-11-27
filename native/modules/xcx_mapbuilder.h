// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Background builder of the native map widgets' world pictures (xcx_maptiles.h). The pictures take
// seconds to build when the game's files are slow (hundreds of source tiles), and the host's single
// asset worker also serves the page picture, so they are built here, on a thread of their own, and
// shown only once built: the module publishes a picture's show flag after Ready(), and the host's
// image request is then answered from this cache at once. Nothing runs on the module's tick.
#include "xcx_view.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace xcx {
class MapBuilder {
  public:
    MapBuilder() { renderer_.SetCancel(&cancel_); }
    MapBuilder(const MapBuilder &) = delete;
    MapBuilder &operator=(const MapBuilder &) = delete;
    ~MapBuilder();
    // Tick thread. `wanted` replaces the previous wanted set, most important first; the pictures are
    // built from the player's area and saved flags. Built ones that are no longer wanted stay cached
    // (LRU, bounded) until the budget needs the room.
    void Want(const EdenDsmodHostApi &, std::uint32_t zone, const std::vector<std::uint8_t> &flags,
              const std::vector<tiles::Key> &wanted);
    // Any thread.
    bool Ready(const tiles::Key &) const;
    std::shared_ptr<const Image> Get(const tiles::Key &) const;
    std::size_t CachedBytes() const;
    // Test hook: block until every wanted picture is built (or failed). Never used by the module.
    void WaitIdle();
    static constexpr std::size_t BudgetBytes = 40u * 1024 * 1024;

  private:
    struct Slot {
        std::shared_ptr<const Image> image;
        std::uint64_t used{};
    };
    void Loop();
    mutable std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::thread thread_;
    EdenDsmodHostApi host_{};
    bool have_host_{}, stop_{}, running_{};
    // Set by the destructor: a picture being built stops at its next source tile (seconds on slow files
    // otherwise), so destroying the module never waits for it.
    std::atomic<bool> cancel_{false};
    std::uint32_t zone_{};
    std::vector<std::uint8_t> flags_;
    std::vector<tiles::Key> wanted_;
    mutable std::map<std::string, Slot> cache_;
    std::map<std::string, std::chrono::steady_clock::time_point> retry_after_;
    mutable std::uint64_t tick_{};
    Renderer renderer_; // its own: the host's asset worker has another one
};
} // namespace xcx
