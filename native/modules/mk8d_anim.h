// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Mario Kart 8 Deluxe module: the rank table's card-swap animation.
//
// Presentation only. Every racer's card has a displayed row position (row units: 1.0 = the
// row of rank 1, 12.0 = rank 12) that chases the racer's CURRENT rank. The page places each card
// at that position (y_bind); ranks, names and items shown on the card are the reader's values,
// unchanged, and the number column / local number cell stay on the exact ranks.
//
// Rules:
//   - chase, never queue: a rank change starts a new slide from wherever the card is drawn now
//     toward the latest rank; there is no backlog of intermediate ranks;
//   - fixed duration: every slide takes the same time whatever its distance (an ease-out re-timed
//     from the current position), NormalMs for an ordinary overtake;
//   - pile-up: when PileupMinMoving or more cards are moving at the moment a slide (re)starts,
//     that slide takes PileupMs and has no sideways drift / glow (race start, a multi-rank jump,
//     overtakes overlapping in time). A single overtake moves exactly two cards;
//   - an overtaking (moving-up) card of an ordinary overtake drifts right (0 -> DriftRows at
//     mid-slide -> 0) and glows (level 0..3, peak mid-slide), so the two cards visibly cross;
//     a drift still in progress when a slide is retargeted fades out continuously;
//   - snap (no animation): on the first update, after a gap longer than SnapGapMs (screen hidden,
//     emulation paused), when the racer count or any card's validity changes, and when the
//     caller's race context key changes (new race / race entry).
// Time is the caller's wall clock in milliseconds; the module calls Update on every runtime tick
// (60 Hz), although the reader re-reads race state only every second tick.

#pragma once

#include <array>
#include <cstdint>

namespace Mk8dAnim {

constexpr int MaxCards = 12;

struct Config {
    double normal_ms = 150.0;   ///< ordinary overtake slide
    double pileup_ms = 100.0;   ///< pile-up slide
    int pileup_min_moving = 3;  ///< moving cards (incl. the one starting) for pile-up mode
    double drift_rows = 0.30;   ///< peak rightward drift of an overtaking card (row units)
    int glow_levels = 3;        ///< overtaking card glow: 0..glow_levels
    double snap_gap_ms = 250.0; ///< an update gap longer than this snaps every card
};

/// One update's input: the reader's racers (index = racer slot), exactly as published.
struct Input {
    int count{};                        ///< racers.count (0 = no racers)
    std::array<bool, MaxCards> valid{}; ///< r{i}.valid
    std::array<int, MaxCards> rank{};   ///< r{i}.rank (1..12)
    std::uint64_t context{};            ///< race context key; a change snaps
};

struct Card {
    double y{};  ///< displayed row position (row units, 1.0 = top row); 0 for an invalid card
    double dx{}; ///< sideways offset (row units, + = right)
    int glow{};  ///< 0..glow_levels
    bool moving{};
};

class RankTable {
public:
    explicit RankTable(const Config& config = {}) : cfg{config} {}

    /// Advances every card to `now_ms` for the current ranks.
    void Update(double now_ms, const Input& in);
    /// The next Update snaps every card to its rank.
    void Reset() {
        have = false;
    }

    const Card& At(int i) const {
        return cards[static_cast<std::size_t>(i)];
    }
    int Moving() const {
        return moving;
    }
    /// A slide in pile-up mode is in progress.
    bool Pileup() const {
        return pileup;
    }
    std::uint64_t Snaps() const {
        return snaps;
    }
    std::uint64_t Slides() const {
        return slides_started;
    }
    std::uint64_t PileupSlides() const {
        return pile_slides_started;
    }
    const Config& Settings() const {
        return cfg;
    }

private:
    struct Slide {
        double from{};  ///< position at the (re)start
        double to{};    ///< target rank
        double start{}; ///< ms
        double dur{};   ///< ms
        double dx0{};   ///< sideways offset at the (re)start (fades out)
        bool active{};
        bool pile{};
        bool up{}; ///< moving toward rank 1
    };
    void Snap(const Input& in);
    /// Position / offset / glow of slide `s` at `now`.
    Card Eval(const Slide& s, double now) const;

    Config cfg;
    bool have{};
    double last_ms{};
    int last_count{};
    std::array<bool, MaxCards> last_valid{};
    std::uint64_t last_context{};
    std::array<Slide, MaxCards> slide{};
    std::array<Card, MaxCards> cards{};
    int moving{};
    bool pileup{};
    std::uint64_t snaps{}, slides_started{}, pile_slides_started{};
};

} // namespace Mk8dAnim
