// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Switch suggestions for the battle's Pokémon list: which benched Pokémon to send in against the
// foe(s), each with short reasons (resists Fire, Surf is super effective, faster). Pure: the module
// fills the battlers from the live reader and the move table, the tests from literals; the reasons
// are data, worded by the module in the game's language (lp_strings switch_* rows).
//
// A candidate is scored against each foe and the scores are averaged (a double: both foes):
// - defence: the foe's strongest hit on it (its damaging moves, or its own types when none are
//   known), effectiveness x power x STAB x the foe's attacking stat over the candidate's defence;
// - offence: the candidate's best hit on the foe, the same way round;
// - Speed against the foe's (staged) Speed; HP left; major status.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace lp_switch {

struct Move {
    int type = -1;     // 0 Normal .. 17 Fairy
    int category = 0;  // 0 status, 1 physical, 2 special
    int power = 0;     // 0 / 1 = no fixed power
    std::string name;
};

struct Battler {
    int slot = -1;                       // a candidate's own party slot (the battle's member order)
    std::array<int, 3> types{-1, -1, -1}; // battle types; 18 = none, [2] a third type or -1
    std::array<int, 5> stats{-1, -1, -1, -1, -1}; // atk, def, spa, spd, spe (-1 unknown)
    int hp = 0, hp_max = 0;
    int status = 0;                      // 0 none, 1 paralysis, 2 sleep, 3 freeze, 4 burn, 5 poison
    std::vector<Move> moves;
};

// Why a candidate is suggested. Warnings (Weak, Status, LowHp) are drawn red.
struct Reason {
    enum Kind { Immune, Resists, Super, Faster, Weak, Status, LowHp, Even } kind = Even;
    int type = -1;    // Immune / Resists / Weak: the attack type; Super: the move's type
    std::string move; // Super: the move's name ("" = unknown: name the type)
    int status = 0;   // Status: 1 paralysis, 2 sleep, 3 freeze, 4 burn, 5 poison
    bool Warning() const {
        return kind == Weak || kind == Status || kind == LowHp;
    }
    bool operator==(const Reason&) const = default;
};

struct Suggestion {
    int slot = -1;
    int score = 0;
    int strength = 0; // equal scores (the caps reached against a weak foe): the higher stat total first
    std::vector<Reason> reasons; // short, best first; warnings after the good ones
};

// Effectiveness of an attack type on battle types in sixteenths (16 neutral, 0 immune, -1 unknown).
using EffFn = int (*)(int attack, const std::array<int, 3>& types);

namespace detail {

constexpr double Neutral = 80; // a neutral STAB-less hit of power 80 at even stats: the 0 point

inline bool HasType(const std::array<int, 3>& t, int type) {
    return type >= 0 && type < 18 && (t[0] == type || t[1] == type || t[2] == type);
}

inline int Eff(EffFn eff, int type, const std::array<int, 3>& t) {
    const int e = eff ? eff(type, t) : -1;
    return e < 0 ? 16 : e;
}

// attacking stat over defending stat for a category; 1 when either is unknown
inline double Ratio(const Battler& a, const Battler& d, int category) {
    const int at = a.stats[category == 1 ? 0 : 2], df = d.stats[category == 1 ? 1 : 3];
    if (at <= 0 || df <= 0)
        return 1;
    return std::clamp(static_cast<double>(at) / df, 0.25, 4.0);
}

struct Hit {
    double damage = 0;
    int type = -1, eff = 16, power = 0;
    const Move* move = nullptr;
};

// The attacks `a` threatens with: its damaging moves; none known -> its own types (power 70, its
// better attacking stat)
inline std::vector<Move> Attacks(const Battler& a) {
    std::vector<Move> out;
    for (const auto& m : a.moves)
        if (m.category != 0 && m.type >= 0 && m.type < 18 && m.power > 1)
            out.push_back(m);
    if (!out.empty())
        return out;
    const int cat = a.stats[0] >= 0 && a.stats[2] > a.stats[0] ? 2 : 1;
    for (int k = 0; k < 2; ++k)
        if (a.types[k] >= 0 && a.types[k] < 18 && (k == 0 || a.types[1] != a.types[0]))
            out.push_back({a.types[k], cat, 70, {}});
    return out;
}

// a's best hit on d
inline Hit Best(const Battler& a, const std::vector<Move>& attacks, const Battler& d, EffFn eff) {
    Hit best;
    for (const auto& m : attacks) {
        const int e = Eff(eff, m.type, d.types);
        const double stab = HasType(a.types, m.type) ? 1.5 : 1.0;
        const double dmg = m.power * stab * e / 16.0 * Ratio(a, d, m.category);
        if (!best.move || dmg > best.damage) {
            best = {dmg, m.type, e, m.power, &m};
        }
    }
    return best;
}

inline double Log2Score(double v, double scale, double limit) {
    if (v <= 0)
        return -limit;
    return std::clamp(scale * std::log2(v), -limit, limit);
}

} // namespace detail

// Scores every candidate (fainted ones are skipped: the caller leaves out the active ones) against
// the foes and returns the best `max` first (equal scores: the higher stat total, then the party order).
inline std::vector<Suggestion> Suggest(const std::vector<Battler>& candidates, const std::vector<Battler>& foes,
                                       EffFn eff, std::size_t max = 3) {
    using namespace detail;
    std::vector<Suggestion> out;
    if (foes.empty())
        return out;
    for (const auto& c : candidates) {
        if (c.hp <= 0 || c.hp_max <= 0)
            continue;
        Suggestion s;
        s.slot = c.slot;
        double score = 0;
        // reasons collected over the foes: the type of the strongest threat it resists / is immune or
        // weak to, the best super-effective move, Speed
        int immune = -1, resist = -1, weak = -1;
        const Move* super = nullptr;
        int super_eff = 0;
        bool faster = true;
        const auto own_attacks = Attacks(c);
        // no moves known: its own types stand in (Attacks); only status moves: no offence at all
        const bool has_damage = c.moves.empty() || std::any_of(c.moves.begin(), c.moves.end(), [](const Move& m) {
                                    return m.category != 0 && m.power > 1;
                                });
        for (const auto& f : foes) {
            const auto threats = Attacks(f);
            // defence: the foe's best hit on this candidate (0 point: a neutral 80-power hit)
            const Hit in = Best(f, threats, c, eff);
            score += in.move ? -40 * std::max(-1.5, std::min(1.5, std::log2(std::max(in.damage, 1.0) / Neutral)))
                             : 0;
            if (in.move && in.damage <= 0)
                score += 20; // nothing the foe has can touch it
            // the reason's type: of the foe's attacks, strongest first (power x STAB against a neutral
            // target), the first it is immune to / resists / is weak to
            std::vector<const Move*> order;
            for (const auto& m : threats)
                order.push_back(&m);
            std::stable_sort(order.begin(), order.end(), [&f](const Move* a, const Move* b) {
                return a->power * (HasType(f.types, a->type) ? 3 : 2) > b->power * (HasType(f.types, b->type) ? 3 : 2);
            });
            for (const Move* m : order) {
                const int e = Eff(eff, m->type, c.types);
                if (e == 0 && immune < 0)
                    immune = m->type;
                else if (e > 0 && e < 16 && resist < 0)
                    resist = m->type;
                else if (e > 16 && weak < 0)
                    weak = m->type;
            }
            // offence: its best hit on the foe
            if (has_damage) {
                const Hit out_hit = Best(c, own_attacks, f, eff);
                score += Log2Score(out_hit.damage / Neutral, 30, 45);
                if (out_hit.move && out_hit.eff > 16 && out_hit.eff >= super_eff) {
                    super = out_hit.move;
                    super_eff = out_hit.eff;
                }
            } else {
                score -= 45;
            }
            // Speed (paralysis halves it in Gen 8 / BDSP)
            const int cs = c.stats[4] < 0 ? -1 : c.status == 1 ? c.stats[4] / 2 : c.stats[4];
            const int fs = f.stats[4] < 0 ? -1 : f.status == 1 ? f.stats[4] / 2 : f.stats[4];
            if (cs >= 0 && fs >= 0) {
                score += cs > fs ? 12 : cs < fs ? -6 : 0;
                faster = faster && cs > fs;
            } else {
                faster = false;
            }
        }
        score /= static_cast<double>(foes.size());
        // HP left and status
        const double frac = static_cast<double>(c.hp) / c.hp_max;
        score += 30 * (frac - 1);
        if (frac < 0.25)
            score -= 15;
        static constexpr int StatusCost[6] = {0, 12, 30, 30, 10, 6};
        if (c.status >= 1 && c.status <= 5)
            score -= StatusCost[c.status];
        // type matchups count a little beyond the caps (a weak foe saturates the damage terms)
        score += (immune >= 0 ? 8 : resist >= 0 ? 6 : 0) + (super ? 6 : 0) - (weak >= 0 ? 6 : 0);
        s.score = static_cast<int>(std::lround(score));
        for (const int v : c.stats)
            s.strength += std::max(0, v);
        // up to three reasons: defence, offence, Speed, then warnings; a warning always keeps a line
        std::vector<Reason> good, warn;
        if (immune >= 0)
            good.push_back({Reason::Immune, immune, {}, 0});
        else if (resist >= 0)
            good.push_back({Reason::Resists, resist, {}, 0});
        if (super)
            good.push_back({Reason::Super, super->type, super->name, 0});
        if (faster)
            good.push_back({Reason::Faster, -1, {}, 0});
        if (weak >= 0)
            warn.push_back({Reason::Weak, weak, {}, 0});
        if (c.status >= 1 && c.status <= 5)
            warn.push_back({Reason::Status, -1, {}, c.status});
        if (frac < 0.25)
            warn.push_back({Reason::LowHp, -1, {}, 0});
        const std::size_t keep = std::min(good.size(), warn.empty() ? std::size_t{3} : std::size_t{2});
        s.reasons.assign(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(keep));
        for (std::size_t k = 0; k < warn.size() && s.reasons.size() < 3; ++k)
            s.reasons.push_back(warn[k]);
        if (s.reasons.empty())
            s.reasons.push_back({Reason::Even, -1, {}, 0});
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const Suggestion& a, const Suggestion& b) {
        return a.score != b.score ? a.score > b.score : a.strength > b.strength;
    });
    if (out.size() > max)
        out.resize(max);
    return out;
}

} // namespace lp_switch
