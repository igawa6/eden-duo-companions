// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// ABI entry point of the companion module (MODULE_GUIDE.md §1).
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "core/mods/modules/dsmod_module_sdk.h"
#include "cotl_actions.h"
#include "cotl_font.h"
#include "cotl_guest.h"
#include "cotl_action_abi.h"
#include "cotl_i2.h"
#include "cotl_icons.h"
#include "cotl_offsets_1_5_27.h"
#include "cotl_problems.h"
#include "cotl_publish.h"
#include "cotl_quests.h"
#include "cotl_snapshot.h"

namespace {

// Re-read the lists every 10 ticks (6 Hz); in between, publish the last state again,
// because the host clears the values every tick.
constexpr uint32_t kReadEvery = 10;

// How long all values keep flowing after a page change (120 ms crossfade, plus margin)
constexpr int kAllPagesTicks = 30;

// Mailbox of the guest helper via the host's load_u32/store_u32
struct HostMailbox : cotl::MailboxIo {
    const EdenDsmodHostExtensions* ext = nullptr;
    bool Load(uint32_t off, uint32_t* v) override {
        return ext != nullptr && ext->load_u32 != nullptr && ext->load_u32(ext->userdata, off, v);
    }
    bool Store(uint32_t off, uint32_t v) override {
        return ext != nullptr && ext->store_u32 != nullptr && ext->store_u32(ext->userdata, off, v);
    }
};

// Tabs like TABS in tools/cotl/gen_manifest.py
constexpr int64_t kTabCount = 6, kFollowersTab = 1;

struct Selections {
    int64_t follower = -1, problem_filter = -1, problem = -1, quest = -1, item = -1, tarot = -1;
    bool texts = false;
    bool operator==(const Selections&) const = default;
};

struct Instance {
    EdenDsmodHostApi host; // Copy: pointers in callbacks are only borrowed
    std::optional<cotl::GameSnapshot> last;
    uint32_t ticks = 0;
    // I2 texts: loaded in the background (parsing 12 MB would block a tick)
    std::unique_ptr<cotl::I2Table> i2;
    std::atomic<bool> i2_ready{false};
    std::thread i2_loader;
    // Icons: only used on the asset worker (load_image), separate from the tick thread
    std::mutex icons_mutex;
    EdenDsmodHostExtensions ext{};
    // Follower actions: client on top of the mailbox (ext is copied in Configure)
    HostMailbox mailbox;
    cotl::ActionClient actions{mailbox};
    std::unique_ptr<cotl::IconLoader> icons;
    // Game font: loaded once, used by decode_font (host thread) and load_image (asset worker)
    std::mutex font_mutex;
    std::optional<cotl::HostFont> font;
    cotl::ProblemSelection problem_selection;
    cotl::SkinColourCache colours; // only on the read thread (ReadSnapshot)
    cotl::KeepSelection<int32_t> follower_selection;
    cotl::KeepSelection<std::string> quest_selection;
    cotl::KeepSelection<int32_t> item_selection;
    // Tarot window: held by the module so that it closes with the run (only the package can set flags)
    std::atomic<int64_t> run_open{-1};
    // Selected tab and open follower window (row): also in the module, so that the link from the
    // Problems window (prob.goto) can set both at once. Published as nav.tab, fol.open.
    std::atomic<int64_t> tab{0};
    std::atomic<int64_t> fol_open{-1};
    // Tab that closing the follower window returns to (-1 = none): set by
    // prob.goto, consumed on close, discarded on any other tab or window change
    std::atomic<int64_t> return_tab{-1};
    // Row of the follower in the open Problems window (-1 = none); target of prob.goto. Only in
    // Sample/OnAction, which run one after the other
    int64_t prob_row = -1;
    // Follower (ID) in the open Problems window (-1 = none); target of prob.walk. Like prob_row
    int32_t prob_follower = -1;
    // Quest giver in the open Quests window (-1 = none); target of quest.walk. Like prob_row
    int32_t quest_walk = -1;
    // Follower that Walk over last went to, while its status is shown (-1 = none).
    // The status is then in walk.msg (under the button), not in act.message. Discarded when
    // a follower window is opened or closed and on every command.
    int32_t walk_follower = -1;
    // The last command was Walk over: its status (e.g. "Done") never belongs in act.message,
    // not even after closing. Only a real command shows its status there again.
    bool last_was_walk = false;
    // Finished values; replayed every tick, rebuilt only on a new state or a new selection
    cotl::RecordingPublisher values;
    Selections selections;
    // Open submenu in the follower window (Eat, Drink, Work); closed with the window or another follower
    cotl::ActionMenu menu;
    bool built = false;
    std::string page;
    int all_pages_ticks = 0;
};

// icons_mutex held
void EnsureIcons(Instance* inst) {
    if (!inst->icons) {
        inst->icons = std::make_unique<cotl::IconLoader>(
            ce_unity::MakeHostRangeReader(&inst->host),
            cotl::MakeTiledAstcDecoder(inst->ext.userdata, inst->ext.decode_astc));
    }
}

// Loads the game font on first use; false while the romfs cannot be read
bool EnsureFont(Instance* inst) {
    std::lock_guard lock(inst->font_mutex);
    if (inst->font) {
        return true;
    }
    if (inst->host.read_romfs == nullptr) {
        return false;
    }
    ce_unity::Reader reader(ce_unity::MakeHostRangeReader(&inst->host),
                            cotl::MakeTiledAstcDecoder(inst->ext.userdata, inst->ext.decode_astc));
    const auto tmp = ce_unity::LoadTmpFont(reader, cotl::kFontBundle, cotl::kFontName, true);
    if (tmp) {
        inst->font = cotl::BuildHostFont(*tmp, cotl::kFontCapPx, cotl::kFontLineHeight);
    }
    if (!inst->font) {
        return false;
    }
    // Heart icons for texts (cotl::TextWithIcons); if they are missing, that spot in the text stays empty
    std::lock_guard icons_lock(inst->icons_mutex);
    EnsureIcons(inst);
    std::vector<cotl::IconGlyph> glyphs;
    const auto add = [&](uint32_t cp, const char* kind, const char* state) {
        if (auto img = inst->icons->LoadHeart(kind, state)) {
            glyphs.push_back({cp, std::move(*img)});
        }
    };
    add(cotl::kIconHeart, "red", "full");
    add(cotl::kIconHalfHeart, "blue", "half"); // no container: just the half shape
    add(cotl::kIconIceHeart, "ice", "full");
    add(cotl::kIconFireHeart, "fire", "full");
    cotl::AddIconGlyphs(*inst->font, glyphs, cotl::kFontCapPx);
    return true;
}

void LoadTexts(Instance* inst) {
    try {
        if (auto t = cotl::I2Table::Load(ce_unity::MakeHostRangeReader(&inst->host))) {
            inst->i2 = std::make_unique<cotl::I2Table>(std::move(*t));
            inst->i2_ready.store(true, std::memory_order_release);
        }
    } catch (...) {
    }
}

EdenDsmodBool SupportsBuild(const char* build_hex) {
    return build_hex != nullptr && std::strcmp(build_hex, cotl::off::kBuildIdHex) == 0
               ? EDEN_DSMOD_TRUE
               : EDEN_DSMOD_FALSE;
}

void* Create(const EdenDsmodHostApi* host, const char*) {
    try {
        if (!dsmod_sdk::HostAbiMatches(host) || host->title_id != cotl::off::kTitleId) {
            return nullptr;
        }
        uint8_t expected[0x20]{};
        std::memcpy(expected, cotl::off::kBuildId, sizeof(cotl::off::kBuildId));
        if (std::memcmp(host->build_id, expected, sizeof(expected)) != 0) {
            return nullptr;
        }
        auto* inst = new (std::nothrow) Instance{};
        if (inst != nullptr) {
            inst->host = *host;
            if (host->read_romfs != nullptr) {
                inst->i2_loader = std::thread(LoadTexts, inst);
            }
        }
        return inst;
    } catch (...) {
        return nullptr;
    }
}

void Destroy(void* instance) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst != nullptr && inst->i2_loader.joinable()) {
        inst->i2_loader.join();
    }
    delete inst;
}

// Buttons of the selected follower. The state belongs to it, otherwise (selection just changed, torn read)
// none; Sample and OnAction run one after the other, both see the same list.
std::vector<cotl::ActionRow> SelectedActions(const Instance& inst) {
    const auto& follower = inst.follower_selection.Current();
    if (!follower || !inst.last || !inst.last->command_state ||
        inst.last->command_state->follower_id != *follower) {
        return {};
    }
    return cotl::MenuActions(cotl::OfferedCommands(*inst.last->command_state), inst.menu);
}

// Rebuild all values from the last read state; they are then passed on every tick
void Rebuild(Instance* inst, const Selections& now) {
    auto& rec = inst->values;
    rec.Clear();
    if (!inst->last) {
        rec.I64("g.in_run", 0); // tab switching always needs this value
        rec.I64("g.loaded", 0);
        inst->prob_row = -1;
        inst->prob_follower = -1;
        inst->quest_walk = -1;
        return;
    }
    const auto& s = *inst->last;
    const cotl::Terms terms(now.texts ? inst->i2.get() : nullptr);
    cotl::PublishFollowers(rec, s, terms);
    const auto& follower = inst->follower_selection.Current();
    cotl::PublishSelectedFollower(rec, s, terms, follower ? cotl::FollowerRowOf(s, *follower) : -1);
    const auto problems = cotl::DeriveProblems(s, terms, static_cast<int32_t>(now.problem_filter));
    cotl::PublishProblems(rec, s, problems);
    const int32_t prob_follower = inst->problem_selection.Update(now.problem, problems);
    cotl::PublishSelectedProblems(rec, s, prob_follower);
    inst->prob_row = prob_follower < 0 ? -1 : cotl::FollowerRowOf(s, prob_follower);
    inst->prob_follower = inst->prob_row < 0 ? -1 : prob_follower;
    const auto quests = cotl::BuildQuests(s, terms);
    const auto& quest = inst->quest_selection.Update(now.quest, [&](int64_t row) {
        return row < static_cast<int64_t>(quests.size()) ? quests[static_cast<size_t>(row)].key : std::string{};
    });
    int64_t quest_row = -1;
    for (size_t i = 0; quest && i < quests.size(); ++i) {
        quest_row = quests[i].key == *quest ? static_cast<int64_t>(i) : quest_row;
    }
    cotl::PublishQuests(rec, quests, quest_row);
    inst->quest_walk = quest_row >= 0 && quests[static_cast<size_t>(quest_row)].offered
                           ? quests[static_cast<size_t>(quest_row)].giver_id
                           : -1;
    const auto& item = inst->item_selection.Update(now.item, [&](int64_t selected) {
        return cotl::InventoryTypeAt(s, selected).value_or(-1);
    });
    cotl::PublishInventory(rec, s, terms, item ? cotl::InventoryRowOf(s, *item) : -1);
    cotl::PublishRun(rec, s, terms, now.tarot);
    cotl::PublishMap(rec, s);
    cotl::PublishCult(rec, s);
}

// Threads: on_action and sample run serialized on the host's timing thread (contract in
// dsmod_module_extensions.h: "Runs serialized with sample/tick on the timing thread"; the tap actions
// run in the same tick as the module call, mod_module_host.cpp). ActionClient and
// follower_selection therefore need no lock; only load_image runs on its own worker.
void Sample(void* instance, const EdenDsmodHostApi* host) {
    try {
        if (instance == nullptr || host == nullptr || host->publish_i64 == nullptr) {
            return;
        }
        auto* inst = static_cast<Instance*>(instance);
        host->publish_i64(host->userdata, "mod.ready", 1);
        host->publish_i64(host->userdata, "nav.tab", inst->tab.load(std::memory_order_relaxed));
        host->publish_i64(host->userdata, "fol.open", inst->fol_open.load(std::memory_order_relaxed));
        // Diagnostic, goes away in Task 8 like act.heartbeat: raw float bits of the last stick reading,
        // y high, x low
        if (uint32_t x = 0, y = 0; inst->ext.load_u32 != nullptr &&
                                   inst->ext.load_u32(inst->ext.userdata, COTL_ACT_RUN_AXIS_X, &x) &&
                                   inst->ext.load_u32(inst->ext.userdata, COTL_ACT_RUN_AXIS_Y, &y)) {
            host->publish_i64(host->userdata, "act.dbg_axis",
                              static_cast<int64_t>((uint64_t{y} << 32) | x));
        }
        // Debug: heartbeat of the guest helper; absent while no hook is installed
        if (uint32_t heartbeat = 0; inst->ext.load_u32 != nullptr &&
                                    inst->ext.load_u32(inst->ext.userdata, COTL_ACT_HEARTBEAT,
                                                       &heartbeat)) {
            host->publish_i64(host->userdata, "act.heartbeat", heartbeat);
        }
        const auto flag = [&](const char* name) {
            return host->get_i64 ? host->get_i64(host->userdata, name, -1) : -1;
        };
        const Selections now{inst->fol_open.load(std::memory_order_relaxed), flag("@flag:prob.filter"),
                             flag("@flag:prob.open"),  flag("@flag:quest.open"),
                             flag("@flag:inv.open"),   inst->run_open.load(std::memory_order_relaxed),
                             inst->i2_ready.load(std::memory_order_acquire)};
        // A newly tapped follower: read thoughts now, otherwise the window stays empty until the next read
        const auto before = inst->follower_selection.Current();
        const auto& follower = inst->follower_selection.Update(now.follower, [&](int64_t row) {
            return inst->last ? cotl::FollowerIdAtRow(*inst->last, row).value_or(-1) : -1;
        });
        // A submenu belongs to the follower it was opened for, and only while the window is open
        if (now.follower < 0 || follower != before) inst->menu = {};
        bool dirty = now != inst->selections;
        if (inst->ticks++ % kReadEvery == 0 || follower != before) {
            const cotl::HostGuest guest(host);
            const auto statics =
                cotl::StaticFields(guest, host->main_base, cotl::off::kDataManagerTypeInfo);
            const bool has_game =
                statics && cotl::ReadPtr(guest, *statics + cotl::off::kDataManagerInstance);
            if (!has_game) {
                dirty = dirty || inst->last.has_value();
                inst->last.reset(); // before the title screen or while loading
            } else if (auto snap = cotl::ReadSnapshot(guest, host->main_base,
                                                      follower.value_or(cotl::kNoThoughts),
                                                      inst->run_open.load(std::memory_order_relaxed) >=
                                                          cotl::kRunSelWeapon,
                                                      &inst->colours)) {
                inst->last = std::move(snap);
                dirty = true;
            }
            // On a torn read the last valid state stays in place
        }
        // Action status after the selection: a message only applies to the follower it went to. After
        // reading, so that a newly selected follower has its buttons right away.
        {
            const int32_t walk = inst->walk_follower;
            const cotl::ActionStatus action_status =
                inst->actions.Poll(walk >= 0 ? walk : follower ? *follower : cotl::kAnyFollower);
            cotl::HostPublisher pub(host);
            cotl::ActionStatus shown = action_status;
            if (walk >= 0 || inst->last_was_walk) { // Walk over: message in walk.msg, not in act.message
                shown = {};
                shown.helper_alive = action_status.helper_alive;
            }
            cotl::PublishActions(pub, inst->actions.Available(), shown, SelectedActions(*inst));
            if (inst->actions.Available()) { // like act.*: without a helper there is no button
                pub.I64("walk.active", walk >= 0 ? 1 : 0);
                pub.Text("walk.msg", walk >= 0 ? cotl::WalkMessage(action_status) : "");
            }
        }
        // Tarot window belongs to the run: closed afterwards, even if it was open at death
        if (!(inst->last && inst->last->run) && inst->run_open.exchange(-1) != -1) {
            dirty = true;
        }
        if (dirty || !inst->built) {
            inst->selections = now;
            inst->built = true;
            Rebuild(inst, now);
        }
        // During a page change (the crossfade shows both pages) pass all values on
        const char* page = host->get_text ? host->get_text(host->userdata, "__page") : nullptr;
        if (page != nullptr && inst->page != page) {
            inst->page = page;
            inst->all_pages_ticks = kAllPagesTicks;
        }
        std::optional<cotl::PageGroup> group;
        if (inst->all_pages_ticks > 0) {
            --inst->all_pages_ticks;
        } else if (page != nullptr) {
            group = cotl::GroupOfPage(inst->page);
        }
        cotl::HostPublisher pub(host);
        inst->values.Replay(pub, group);
    } catch (...) {
    }
}

void Tick(void*, const EdenDsmodHostApi*) {}

void Configure(void* instance, const EdenDsmodHostExtensions* ext) {
    if (instance != nullptr && ext != nullptr) {
        static_cast<Instance*>(instance)->ext = *ext; // copy: the pointer is only borrowed
        static_cast<Instance*>(instance)->mailbox.ext = &static_cast<Instance*>(instance)->ext;
    }
}

// Display state of the companion; fol.act sends a request to the guest helper
EdenDsmodBool OnAction(void* instance, const char* action, int64_t argument) {
    auto* inst = static_cast<Instance*>(instance);
    if (inst == nullptr || action == nullptr) {
        return EDEN_DSMOD_FALSE;
    }
    if (std::strcmp(action, "run.select") == 0) {
        inst->run_open.store(argument, std::memory_order_relaxed);
        return EDEN_DSMOD_TRUE;
    }
    if (std::strcmp(action, "run.close") == 0) {
        inst->run_open.store(-1, std::memory_order_relaxed);
        return EDEN_DSMOD_TRUE;
    }
    if (std::strcmp(action, "tab") == 0) { // Tab bar outside a run; argument = tab
        if (argument < 0 || argument >= kTabCount) return EDEN_DSMOD_FALSE;
        inst->tab.store(argument, std::memory_order_relaxed);
        inst->return_tab.store(-1, std::memory_order_relaxed);
        return EDEN_DSMOD_TRUE;
    }
    if (std::strcmp(action, "fol.open") == 0) { // Follower card (row) or -1 = close
        inst->fol_open.store(argument < 0 ? -1 : argument, std::memory_order_relaxed);
        inst->walk_follower = -1;
        const int64_t back = inst->return_tab.exchange(-1, std::memory_order_relaxed);
        if (argument < 0 && back >= 0) { // opened via the Problems link: back to Problems
            inst->tab.store(back, std::memory_order_relaxed);
        }
        return EDEN_DSMOD_TRUE;
    }
    if (std::strcmp(action, "prob.goto") == 0) { // Problems window → commands (argument unused)
        if (inst->prob_row < 0) return EDEN_DSMOD_FALSE;
        inst->return_tab.store(inst->tab.exchange(kFollowersTab, std::memory_order_relaxed),
                               std::memory_order_relaxed);
        inst->fol_open.store(inst->prob_row, std::memory_order_relaxed);
        inst->walk_follower = -1;
        return EDEN_DSMOD_TRUE;
    }
    // Walk over (argument unused): quest giver, open follower window or Problems window
    const bool quest_walk = std::strcmp(action, "quest.walk") == 0;
    const bool fol_walk = std::strcmp(action, "fol.walk") == 0;
    if (quest_walk || fol_walk || std::strcmp(action, "prob.walk") == 0) {
        const auto& open = inst->follower_selection.Current();
        const int32_t target = quest_walk ? inst->quest_walk
                               : fol_walk ? (inst->fol_open.load(std::memory_order_relaxed) >= 0 && open
                                                 ? *open
                                                 : -1)
                                          : inst->prob_follower;
        if (target < 0 || !inst->actions.Submit(target, COTL_ACT_COMMAND_WALK_ONLY)) return EDEN_DSMOD_FALSE;
        inst->walk_follower = target;
        inst->last_was_walk = true;
        return EDEN_DSMOD_TRUE;
    }
    if (std::strcmp(action, "fol.act") == 0) { // Button of the action list in the follower window; argument = row
        const auto& follower = inst->follower_selection.Current();
        const auto rows = SelectedActions(*inst);
        if (argument < 0 || static_cast<size_t>(argument) >= rows.size() || !follower || *follower < 0) {
            return EDEN_DSMOD_FALSE;
        }
        const auto& row = rows[static_cast<size_t>(argument)];
        if (!row.enabled()) return EDEN_DSMOD_FALSE;
        // Back, next page and submenus stay in the module; only an entry goes to the helper
        if (row.command == cotl::kMenuBack) {
            inst->menu = {};
            return EDEN_DSMOD_TRUE;
        }
        if (row.command == cotl::kMenuMore) {
            ++inst->menu.page;
            return EDEN_DSMOD_TRUE;
        }
        if (row.opens) {
            inst->menu = {row.command, 0};
            return EDEN_DSMOD_TRUE;
        }
        if (!inst->actions.Submit(*follower, row.command, row.parent)) return EDEN_DSMOD_FALSE;
        inst->walk_follower = -1;
        inst->last_was_walk = false;
        inst->menu = {};
        return EDEN_DSMOD_TRUE;
    }
    return EDEN_DSMOD_FALSE;
}

// Images from the game files: module:cotl:item/<ITEM_TYPE> (inventory), equip/<EquipmentType>,
// relic/<RelicType>, tarot/<Card>, heart/<kind>_<state>, mapicon/<Sprite>, follower/<skin;colours>
// (native/modules/cotl_icons.h, follower_portrait.h); module:cotl:font →
// Atlas of the game font
EdenDsmodBool LoadImage(void* instance, const EdenDsmodHostApi*, const char* key, void* receiver,
                        EdenDsmodImageSink sink) {
    try {
        constexpr std::string_view kPrefix = "module:cotl:";
        auto* inst = static_cast<Instance*>(instance);
        const std::string_view k = key != nullptr ? key : "";
        if (inst != nullptr && k == "module:cotl:font" && sink != nullptr) {
            if (!EnsureFont(inst)) {
                return EDEN_DSMOD_FALSE;
            }
            const auto& a = inst->font->atlas;
            sink(receiver, a.width, a.height, a.rgba.data(), a.rgba.size());
            return EDEN_DSMOD_TRUE;
        }
        if (inst == nullptr || !k.starts_with(kPrefix) || inst->host.read_romfs == nullptr) {
            return EDEN_DSMOD_FALSE;
        }
        const std::string_view rest = k.substr(kPrefix.size()); // "<kind>/<key>"
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            return EDEN_DSMOD_FALSE;
        }
        const std::string_view kind = rest.substr(0, slash);
        const std::string arg(rest.substr(slash + 1));
        const auto num = static_cast<int32_t>(std::strtol(arg.c_str(), nullptr, 10));
        std::lock_guard lock(inst->icons_mutex);
        EnsureIcons(inst);
        std::optional<ce_unity::Image> img;
        if (kind == "item") {
            img = inst->icons->LoadItem(num);
        } else if (kind == "equip") {
            img = inst->icons->LoadEquipment(num);
        } else if (kind == "relic") {
            img = inst->icons->LoadRelic(num);
        } else if (kind == "mapicon") {
            img = inst->icons->LoadMapIcon(arg);
        } else if (kind == "follower") { // "<skin>;<SLOT>=<RRGGBBAA>;…"
            img = inst->icons->LoadFollower(arg);
        } else if (kind == "tarot") {
            img = inst->icons->LoadTarot(num);
        } else if (kind == "heart") { // "<kind>_<state>"
            const auto us = arg.find('_');
            if (us != std::string::npos) {
                img = inst->icons->LoadHeart(arg.substr(0, us), arg.substr(us + 1));
            }
        }
        if (!img || sink == nullptr) {
            if (!img && inst->host.log != nullptr) {
                const std::string msg = "cotl icon " + std::string(k) + ": " + inst->icons->LastError();
                inst->host.log(inst->host.userdata, EDEN_DSMOD_LOG_WARNING, msg.c_str());
            }
            return EDEN_DSMOD_FALSE;
        }
        sink(receiver, img->width, img->height, img->rgba.data(), img->rgba.size());
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

// The manifest "font" is the package file cotl_font.txt ("COTL_FONT_V1 ..."); the metrics come from
// the game font (native/modules/cotl_font.h), the atlas through module:cotl:font.
EdenDsmodBool DecodeFont(void* instance, const uint8_t* bytes, size_t size, void* receiver,
                         EdenDsmodFontSink sink) {
    try {
        constexpr std::string_view kMagic = "COTL_FONT_V1";
        auto* inst = static_cast<Instance*>(instance);
        if (inst == nullptr || sink == nullptr || bytes == nullptr ||
            std::string_view(reinterpret_cast<const char*>(bytes), size).substr(0, kMagic.size()) != kMagic ||
            !EnsureFont(inst)) {
            return EDEN_DSMOD_FALSE; // the host tries again later
        }
        const auto& f = *inst->font;
        sink(receiver, f.line_height, f.first_codepoint, f.glyphs.data(),
             static_cast<uint32_t>(f.glyphs.size()));
        return EDEN_DSMOD_TRUE;
    } catch (...) {
        return EDEN_DSMOD_FALSE;
    }
}

const EdenDsmodFontExtensions kFontExtensions = {
    EDEN_DSMOD_FONT_EXT_VERSION, sizeof(EdenDsmodFontExtensions), EDEN_DSMOD_FONT_EXT_HASH, DecodeFont,
};

const EdenDsmodModuleExtensions kExtensions = {
    EDEN_DSMOD_EXT_VERSION, sizeof(EdenDsmodModuleExtensions), EDEN_DSMOD_EXT_HASH,
    Configure, OnAction, LoadImage,
};

const EdenDsmodModuleApi kModule = {
    EDEN_DSMOD_MODULE_ABI_VERSION,
    sizeof(EdenDsmodModuleApi),
    0,
    EDEN_DSMOD_MODULE_ABI_HASH,
    cotl::off::kTitleId,
    "Cult of the Lamb companion",
    EDEN_DSMOD_CAP_NO_TICK_WHEN_HIDDEN | EDEN_DSMOD_CAP_EXTENSIONS,
    SupportsBuild,
    Create,
    Destroy,
    Sample,
    Tick,
};

} // namespace

DSMOD_SDK_EXPORT_MODULE(kModule)
DSMOD_SDK_EXPORT_EXTENSIONS(kExtensions)
DSMOD_SDK_EXPORT_FONT_EXTENSIONS(kFontExtensions)
