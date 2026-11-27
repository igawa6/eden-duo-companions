// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pla_blocks.h"
#include <optional>
#include <set>

namespace pla {
// Supplied observations only. Matching tags reject mixed inputs; they do not
// establish guest coherence, native ownership, disclosure or action permission.
enum class BattlePhase { Unknown, Command, ForcedSwitch, Other };
enum class BattleAffinity { Unknown, Immune, Resisted, Effective, SuperEffective };
// Host-side native artwork identifiers, NOT native status enum/PA8 bit values.
// A provider must establish the visible native icon; no status-bit conversion here.
enum class BattleConditionIcon { Unknown, None, Doku, Dokudoku, Yakedo, Mahi, Nemuri, Koori };
inline const char *BattleConditionTag(BattleConditionIcon icon) {
    switch (icon) {
    case BattleConditionIcon::Doku:return "doku";
    case BattleConditionIcon::Dokudoku:return "dokudoku";
    case BattleConditionIcon::Yakedo:return "yakedo";
    case BattleConditionIcon::Mahi:return "mahi";
    case BattleConditionIcon::Nemuri:return "nemuri";
    case BattleConditionIcon::Koori:return "koori";
    default:return nullptr;
    }
}
struct BattleActor {
    unsigned id = 31;
    bool disclosed = false;
    Pokemon pokemon;
    std::optional<unsigned> hp_percent, research_level;
    BattleConditionIcon condition = BattleConditionIcon::Unknown;
};
struct BattleMove {
    unsigned id = 0, pp = 0, max_pp = 0;
    std::optional<bool> mastered, style_locked;
    BattleAffinity affinity = BattleAffinity::Unknown;
};
struct BattlePageInput {
    uint64_t generation = 0, actor_generation = 0, move_generation = 0, order_generation = 0,
        party_generation = 0;
    bool available = false, moves_available = false, order_available = false;
    BattlePhase phase = BattlePhase::Unknown;
    std::optional<unsigned> command_actor, opponent, move_actor;
    std::vector<BattleActor> actors;
    std::array<BattleMove, 4> moves{};
    std::vector<unsigned> order;
    Party party;
    std::array<BattleConditionIcon, 6> party_conditions{};
    std::array<std::optional<bool>, 6> party_out_markers{};
    unsigned inspected_move = 0;
};
struct BattlePageModel {
    bool available = false, moves_available = false, order_available = false;
    BattlePhase phase = BattlePhase::Unknown;
    std::optional<BattleActor> own, foe;
    std::array<BattleMove, 4> moves{};
    std::vector<BattleActor> order;
    Party party;
    std::array<BattleConditionIcon, 6> party_conditions{};
    std::array<std::optional<bool>, 6> party_out_markers{};
    std::optional<unsigned> inspected_move;
};
inline bool BattleIdentity(const Pokemon &p) {
    return p.valid && p.species && p.species <= 905 && p.form <= 255 &&
        p.level && p.level <= 100 && p.gender <= 2;
}
inline Pokemon BattleIdentityOnly(const Pokemon &p) {
    Pokemon identity;
    identity.valid = true; identity.species = p.species; identity.form = p.form;
    identity.gender = p.gender; identity.level = p.level; identity.shiny = p.shiny;
    identity.nicknamed = p.nicknamed; identity.nickname = p.nickname;
    return identity;
}
inline BattlePageModel ProjectBattle(const BattlePageInput &s) {
    BattlePageModel out;
    if (!s.available || !s.generation || s.actor_generation != s.generation ||
        (s.phase != BattlePhase::Command && s.phase != BattlePhase::ForcedSwitch &&
         s.phase != BattlePhase::Other) || s.actors.size() > 36) return out;
    std::set<unsigned> ids;
    for (const auto &a : s.actors)
        if (a.id >= 31 || !ids.insert(a.id).second) return out;
    const auto find = [&](std::optional<unsigned> id) -> std::optional<BattleActor> {
        if (!id) return {};
        for (const auto &a : s.actors)
            if (a.id == *id && a.disclosed && BattleIdentity(a.pokemon) &&
                (!a.hp_percent || *a.hp_percent <= 100) &&
                (!a.research_level || *a.research_level <= 10)) return a;
        return {};
    };
    out.own = find(s.command_actor);
    out.foe = find(s.opponent);
    if (s.command_actor && s.opponent && s.command_actor == s.opponent) return {};
    if (out.own && (!out.own->pokemon.max_hp || out.own->pokemon.max_hp > 65535 ||
        out.own->pokemon.hp > out.own->pokemon.max_hp)) out.own.reset();
    if (out.own) out.own->pokemon.fainted = !out.own->pokemon.hp;
    // Enemy current/max HP is not an approved disclosure. Keep only native
    // visible percentage; even the sanitized rendering model has no exact HP.
    if (out.foe) {
        out.foe->pokemon = BattleIdentityOnly(out.foe->pokemon);
    }
    out.available = true;
    out.phase = s.phase;
    if (s.party.valid && s.party.count <= 6 && s.party_generation == s.generation) {
        out.party = s.party;
        for (unsigned n = 0; n < out.party.count; ++n) {
            const auto &p = out.party.slots[n];
            if (!BattleIdentity(p) || !p.max_hp || p.max_hp > 65535 || p.hp > p.max_hp)
                out.party.slots[n] = {};
            else {
                out.party.slots[n].fainted = !p.hp;
                out.party_conditions[n] = s.party_conditions[n];
                out.party_out_markers[n] = s.party_out_markers[n];
            }
        }
    }
    if (s.phase != BattlePhase::Command || !out.own || !out.own->pokemon.hp) return out;
    if (s.moves_available && s.move_generation == s.generation && s.move_actor == s.command_actor) {
        std::set<unsigned> moves;
        bool valid = true;
        for (const auto &m : s.moves) {
            if (!m.id) {
                if (m.pp || m.max_pp || m.mastered || m.style_locked ||
                    m.affinity != BattleAffinity::Unknown) valid = false;
            } else if (m.id > 850 || !m.max_pp || m.max_pp > 255 ||
                unsigned(m.affinity) > unsigned(BattleAffinity::SuperEffective) ||
                m.pp > m.max_pp || !moves.insert(m.id).second) valid = false;
        }
        if (valid) {
            out.moves = s.moves;
            if (!out.foe) for (auto &move : out.moves) move.affinity = BattleAffinity::Unknown;
            out.moves_available = true;
            if (s.inspected_move < 4 && s.moves[s.inspected_move].id)
                out.inspected_move = s.inspected_move;
        }
    }
    if (s.order_available && s.order_generation == s.generation && s.order.size() <= 6) {
        bool valid = true;
        for (auto id : s.order) {
            auto a = find(id);
            // This two-card layout cannot assign other allies/opponents a side.
            // Multi-foe battles list other foes too; any disclosed roster actor may appear.
            if (!a) { valid = false; break; }
            // Timeline portraits do not carry additional undisclosed statistics.
            a->pokemon = BattleIdentityOnly(a->pokemon);
            out.order.push_back(*a);
        }
        out.order_available = valid;
        if (!valid) out.order.clear();
    }
    return out;
}
} // namespace pla
