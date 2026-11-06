// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Turns GameSnapshot contents into published values.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cotl_actions.h"
#include "cotl_snapshot.h"
#include "cotl_terms.h"

struct EdenDsmodHostApi;

namespace cotl {

struct Publisher {
    virtual ~Publisher() = default;
    virtual void I64(const std::string& name, int64_t v) = 0;
    virtual void F64(const std::string& name, double v) = 0;
    virtual void Text(const std::string& name, std::string_view v) = 0;
};

class HostPublisher : public Publisher {
public:
    explicit HostPublisher(const EdenDsmodHostApi* host) : host_(host) {}
    void I64(const std::string& name, int64_t v) override;
    void F64(const std::string& name, double v) override;
    void Text(const std::string& name, std::string_view v) override;

private:
    const EdenDsmodHostApi* host_;
};

// Values per page: holds the values of one read state. The host clears them every tick; they
// only need rebuilding when the state changes, and only the visible page needs its own.
enum class PageGroup : uint8_t { Always, Followers, Problems, Quests, Items, Dungeon };
PageGroup GroupOfValue(std::string_view name);
std::optional<PageGroup> GroupOfPage(std::string_view page); // nullopt: unknown, show everything

class RecordingPublisher : public Publisher {
public:
    void I64(const std::string& name, int64_t v) override;
    void F64(const std::string& name, double v) override;
    void Text(const std::string& name, std::string_view v) override;
    void Clear() { entries_.clear(); }
    // page nullopt: all values; otherwise Always and this page's values
    void Replay(Publisher& out, std::optional<PageGroup> page) const;

private:
    enum class Kind : uint8_t { Int, Float, Str };
    struct Entry {
        std::string name;
        PageGroup group;
        Kind kind;
        int64_t i;
        double f;
        std::string s;
    };
    std::vector<Entry> entries_;
};

// Keeps a window on the tapped entry even if the rows re-sort: on a tap
// the row (flag) is translated into a key, -1 = closed.
template <class Key>
class KeepSelection {
public:
    template <class KeyAt>
    const std::optional<Key>& Update(int64_t flag, KeyAt key_at) {
        if (flag != flag_) {
            flag_ = flag;
            key_ = flag < 0 ? std::nullopt : std::optional<Key>(key_at(flag));
        }
        return key_;
    }
    const std::optional<Key>& Current() const { return key_; }

private:
    int64_t flag_ = -1;
    std::optional<Key> key_;
};

// Follower list row to follower ID and back; inventory selection (g * 1000 + i) to ITEM_TYPE and back. For
// KeepSelection: Key_at returns the empty case through optional, hence value_or(-1) at the caller.
std::optional<int32_t> FollowerIdAtRow(const GameSnapshot& s, int64_t row);
int64_t FollowerRowOf(const GameSnapshot& s, int32_t id);
std::optional<int32_t> InventoryTypeAt(const GameSnapshot& s, int64_t selected);
int64_t InventoryRowOf(const GameSnapshot& s, int32_t item_type);

// fol.count, fol.name{i}, fol.level{i}, fol.role{i}, fol.sat{i}, fol.loyal{i}, fol.ill{i},
// fol.happy{i}, fol.group{i}, fol.pic{i} (head image, PortraitKey; empty without a known skin).
// Order: normal followers, then missionaries,
// then prisoners; within the group as in the game.
void PublishFollowers(Publisher& p, const GameSnapshot& s, const Terms& terms);

// Groups as in the game menu (0 Currency, 1 Food, 2 Items), order of the menu lists; only quantities > 0
// and only items the menu shows. inv.count (total), inv.n{g}, inv.g{g}.name{i}/qty{i}/
// icon{i} ("module:cotl:item/<ITEM_TYPE>")/quest{i} (1 = an open collect quest needs it).
// `selected` = g * 1000 + i: inv.sel_name, inv.sel_desc, inv.sel_qty, inv.sel_icon.
void PublishInventory(Publisher& p, const GameSnapshot& s, const Terms& terms, int64_t selected);

// g.in_run (1 during a run), run.heart_count and
// run.heart{i} (image key module:cotl:heart/<kind>_<full|half|empty> in HUD order),
// run.weapon_icon/curse_icon (module:cotl:equip/<EquipmentType>), run.relic_icon
// (module:cotl:relic/<RelicType>), run.has_weapon,
// run.weapon/curse/relic (name, empty if none), run.weapon_lvl, run.curse_lvl, run.has_curse,
// run.has_relic, run.tarot_count, run.tarot_name{i} (name + "+" per upgrade as in the game),
// run.tarot_icon{i}. `selected` = index of the tapped card: run.sel_open, run.sel_name, run.sel_text.
void PublishRun(Publisher& p, const GameSnapshot& s, const Terms& terms, int64_t selected);

// g.loaded (save loaded), cult.name, cult.level (1-based as in the game), cult.seed/seed_cap/has_seed and
// cult.fert/fert_cap/has_fert (silo contents and capacity, has_* = at least one silo).
void PublishCult(Publisher& p, const GameSnapshot& s);

// Selection in the run next to the tarot indices
inline constexpr int64_t kRunSelWeapon = 100, kRunSelCurse = 101, kRunSelRelic = 102;

// Map area on the Dungeon page (tools/cotl/gen_manifest.py, MAP_W/MAP_H)
inline constexpr int32_t kMapAreaW = 1160;
inline constexpr int32_t kMapAreaH = 380;

// Dungeon map (BuildMap): map.sz (index into kMapSizes), map.count, map.x{i}/y{i} (px in the
// map area), map.cur{i} (current), map.vis{i} (visited, not current), map.dim{i} (only
// discovered), map.q{i} (question mark), map.has_icon{i}/map.icon{i} (room icon
// module:cotl:mapicon/<sprite>); counters also per size (map.count<sz>, map.hcount<sz>, ...);
// door stubs separated by direction:
// map.hcount/hx{i}/hy{i}/hdim{i} (east/west), map.vcount/vx{i}/vy{i}/vdim{i} (north/south).
// Without a run: all counters 0.
void PublishMap(Publisher& p, const GameSnapshot& s);

// fol.sel_name, fol.sel_detail for row `index` of the follower list (same order as
// PublishFollowers). Nothing is published outside the list.
void PublishSelectedFollower(Publisher& p, const GameSnapshot& s, const Terms& terms,
                             int64_t index);

// act.available (1 = show buttons), act.state, act.message, act.count, act.name{i}, act.enabled{i},
// act.command{i}. Without a helper only act.available = 0. `available` is ActionClient::Available(): it
// stays true during a pause so the buttons do not flicker; the line then says "Game is paused".
// When idle, the line names the reason for a locked row.
void PublishActions(Publisher& p, bool available, const ActionStatus& status,
                    const std::vector<ActionRow>& rows);

} // namespace cotl
