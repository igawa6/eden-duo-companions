// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Chained Echoes 1.41 live reader (build 17B6A110CC529FBE). Contracts: research/chained-echoes/
// contracts/battle-readers.md (roots, battle phase, actor, turn queue, party, enemies, bestiary)
// and contracts/hud-hide.md (top-screen turn strip, party panel, Overdrive gauge, Ultra bar and
// Sky Armor gear badge visibility writes).
//
// Every root is resolved by IL2CPP class name on every refresh (TypeInfo slot -> klass, name at
// [klass+0x10], statics at [klass+0xB8]); nothing is cached across a failed refresh, so a scene
// switch or a destroyed battle object simply ends the battle view. Values are published only
// while their guard holds (no default 0 is ever published as a live value).
//
// Outputs (published every sample, rebuilt at 10 Hz):
//   ce.page / ce.pagev   the picture shown ("module:ce:..." key; double-buffered: a new key is
//                        shown only once its picture was delivered, so changes never blank)
//   ce.next              the wanted picture while it decodes (an off-screen prefetch widget)
//   ce.state             0 unknown, 1 title, 2 in game (holding view), 3 normal battle (B1),
//                        4 Sky Armor battle, 5 unsupported build, 6 game over (the last
//                        battle picture stays while the wipe-out / Game Over screen runs)
//   ce.gameover          1 in state 6 (the manifest dims the page and blocks input)
//   ce.loading           1 while GameManager reports loading / a scene switch (after 300 ms)
//   ce.battle, ce.linked, ce.chip<i>, ce.sheetok, ce.sheeton, ce.cog   tap-target gates
// The debug reader line carries the HUD bits ("hud="): 1/2 strip found/hidden, 4/8 party rows
// found/hidden, 16 gauge open, 32 gauge hidden, 64 Ultra bar hidden, 128 gear badge hidden.
#pragma once

#include "ce_mech_reader.h"
#include "ce_pages.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ce_reader {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

class Reader {
public:
    explicit Reader(bool unsupported_build);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    void ConfigureWrites(const EdenDsmodHostWriteApi* api);
    void Sample(const EdenDsmodHostApi& host);
    /// Module actions (timing thread). Returns false for an unknown action.
    bool Action(std::string_view name, s64 argument);

    /// Asset worker: a key's picture was handed to the host.
    void MarkDelivered(const std::string& key);
    /// Asset worker: the battle model behind a "module:ce:b1:<hash>" key.
    std::optional<ce_pages::BattleView> Model(const std::string& key);
    /// Asset worker: the Sky Armor model behind a "module:ce:sky:<hash>" key.
    std::optional<ce_sky::SkyView> SkyModel(const std::string& key);
    /// Asset worker: the Formation mirror model behind a "module:ce:fm:<hash>" key.
    std::optional<ce_pages::FormationView> FormationModel(const std::string& key);

    /// Field Home slice (ce_field_reader): the picture for state 2 (in game, no battle); empty =
    /// the holding view. The picture shown / the loading dim, for the field gates.
    std::function<std::string()> idle_key;
    /// Plan steps / skill plans ticked so far (Skills + Crystals readers): the next page picture
    /// cross-fades in.
    std::function<unsigned()> plan_ticks;
    /// Asset worker: the turn strip behind a "module:ce:strip:<hash>" key (entries, dy).
    std::optional<std::pair<std::vector<ce_pages::TurnEntry>, int>> StripModel(const std::string& key);
    const std::string& Shown() const {
        return shown;
    }
    int State() const {
        return frame.state;
    }
    bool LoadingShown() const {
        return loading_now;
    }

private:
    // ---- guest memory --------------------------------------------------------------------
    bool Read(u64 at, void* out, std::size_t n) const;
    u8 U8(u64 at, bool* ok = nullptr) const;
    s32 I32(u64 at, bool* ok = nullptr) const;
    float F32(u64 at, bool* ok = nullptr) const;
    u64 Ptr(u64 at) const;
    static bool Ok(u64 p);
    std::string ClassName(u64 obj);
    std::string CStr(u64 at, std::size_t max = 64) const;
    std::string String(u64 str) const;
    std::vector<u64> ListItems(u64 list, std::size_t limit) const;
    std::vector<u64> ArrayPtrs(u64 arr, std::size_t limit) const;
    u64 Statics(u64 slot, std::string_view name);
    /// Managed GameObject -> (managed component class name -> managed component).
    std::map<std::string, u64> Components(u64 go);
    /// Managed GameObject -> native component pointers (for vtable identification).
    std::vector<u64> NativeComponents(u64 go) const;
    u64 AnimatorOpenCell(u64 go) const;

    // ---- model -----------------------------------------------------------------------------
    struct Frame {
        int state{};
        bool loading{};
        bool battle_valid{};
        ce_pages::BattleView view;
        u64 battle_id{};
        ce_mech::Result sky; ///< state 4 (Sky Armor battle)
        bool formation{};             ///< states 2 / 6: the native Formation screen is open
        ce_pages::FormationView fm;   ///< its mirror (read-only)
    };
    Frame Refresh();
    bool ReadBattle(u64 sf_battle, u64 sf_funcs, ce_pages::BattleView& v, u64 battle_id);
    void Publish();
    /// The picture key of a refreshed frame (battle, Sky Armor, Formation mirror, title...),
    /// registering its model; "" for the in-game / game-over states, which DesiredKey resolves
    /// every sample. Computed once per refresh: the signature hashes the whole model.
    std::string FrameKey(const Frame& f);
    std::string DesiredKey();
    /// Turn strip slide (motion overlay): the strip of a refreshed frame; a passed turn
    /// registers the new strip picture.
    void UpdateStrip();
    void ApplyHud(bool force_restore);
    bool WriteCell(u64 addr, const void* expect, const void* value, u32 size);
    /// All-or-nothing when the write extension is present; checked sequential writes otherwise
    /// (the destructor's restore while the runtime shuts down: see ~Reader in the .cpp).
    bool WriteOps(const EdenDsmodWriteOp* ops, u32 count);
    struct UiGraphic {
        u64 g;    ///< managed Image / TextMeshProUGUI
        bool tmp; ///< TextMeshProUGUI (font colour) rather than Image (Graphic.m_Color)
    };
    /// Image / TextMeshProUGUI components under a managed GameObject (native transform walk),
    /// skipping the child subtree named `skip`; empty when more than max_count are found.
    std::vector<UiGraphic> GraphicsUnder(u64 go_managed, std::string_view skip, int max_depth,
                                         std::size_t max_count);
    float AlphaOf(const UiGraphic& u);
    struct QueueRef {
        u64 list{}, items{};
        s32 n{};
        u64 cap{};
    };
    /// CanvasUpdateRegistry.m_GraphicRebuildQueue's list (validated); false when unavailable.
    bool RebuildQueue(QueueRef& q);
    /// Whether g already waits in the queue; known = false when the queue is too long to scan.
    bool Queued(const QueueRef& q, u64 g, bool& known);
    /// Graphic.SetVerticesDirty() with data only: optional new alpha, m_VertsDirty (+ TMP
    /// m_havePropertiesChanged) and one queue entry (not appended twice). All-or-nothing.
    bool MarkForRebuild(const UiGraphic& u, const float* alpha);
    /// Re-queue graphics whose entry was lost to a concurrent List.Add (see the .cpp).
    void CheckRebuilds();
    /// A graphic hidden by this module: the alpha to restore and its kind.
    struct Hidden {
        float alpha{};
        bool tmp{};
    };
    /// Whether g still is a live graphic of that kind (Image / Text, or TextMeshProUGUI).
    bool IsGraphic(u64 g, bool tmp);
    /// Records of graphics a walk did not reach (a failed read, a partial walk) stay while their
    /// class check holds; a destroyed or reused object is forgotten.
    void KeepUnseen(std::map<u64, Hidden>& kept, const std::map<u64, Hidden>& old,
                    const std::set<u64>& seen);
    void ApplyUltra(u64 sf, bool hide, bool show);
    void ApplyGear(u64 sf, u64 uie, bool running, bool hide, bool show);
    void ApplyParty(u64 uie, bool running, bool hide, bool show);

    const EdenDsmodHostApi* host{};
    EdenDsmodHostApi host_copy{}; ///< for the destructor's restore (function pointers + userdata)
    bool have_host{};
    EdenDsmodBool (*write_batch)(void*, const EdenDsmodWriteOp*, u32){};
    void* write_user{};
    bool unsupported{};

    std::unordered_map<u64, std::string> class_names;
    u64 sample{};
    Frame frame;
    int loading_samples{};

    // selection (module-local, never moves the game cursor)
    int selected_enemy{-1};
    bool sheet_open{};
    bool refresh_now{};
    u64 selection_battle{};

    // picture keys
    std::string shown, wanted;
    u64 wanted_since{};
    std::mutex key_mutex;
    /// Keys handed to the host, most recently used first (bounded LRU). The host serves a cached
    /// picture without calling load_image again, so a key once delivered must stay known while it
    /// can recur: the keys the double buffer and the strip wait on are touched on every check.
    std::list<std::string> delivered_lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> delivered;
    /// key_mutex held: whether the key was delivered (and mark it recently used).
    bool DeliveredLocked(const std::string& key);
    std::map<std::string, u64> delivered_seen; ///< key -> sample it was first seen delivered
    std::map<std::string, ce_pages::BattleView> models;
    std::deque<std::string> model_order;

    // HUD hide (contracts/hud-hide.md). want_*: -1 unknown (no switch value received yet).
    int want_ctb{-1}, want_party{-1}, want_od{-1}, want_ultra{-1}, want_gear{-1};
    std::map<u64, Hidden> ultra_hidden; ///< Ultra bar graphic -> its alpha before the hide
    std::map<u64, Hidden> gear_hidden;  ///< gear badge graphic -> its alpha before the hide
    struct Pending {
        bool tmp{};
        int retries{};
    };
    std::map<u64, Pending> rebuild_pending; ///< graphics queued for a rebuild, until rebuilt
    std::set<u64> ctb_hidden, od_hidden;
    std::map<u64, Hidden> party_hidden; ///< party row graphic -> its alpha before the hide
    bool od_active{}; ///< the game's gauge is open, or closed by this module (system unlocked)
    int od_open{-1}; ///< the game's own Overdrive gauge Animator "open" (-1 unknown)
    bool battle_running{};
    int hud_bits{};
    int title_alive{-1}; ///< StartMenu alive (1), gone (0), unknown (-1)
    int raw_loading{}, logged_loading{-1};
    int dbg_cmd{-1}, dbg_skill{-1}; ///< command / skill help Animator "open" (the Ultra gate)
    float dbg_target{-1.0f};        ///< target box CanvasGroup alpha (the Ultra gate)
    bool loading_now{};
    /// What the debug reader line depends on beyond the picture key (logged on change only).
    struct LogMark {
        int state{};
        std::string key;
        int hud{}, armed{}, tgt_now{}, tgt_mode{}, tgt_idx{}, target{};
        double od{};
        bool ui_names{};
        bool operator==(const LogMark&) const = default;
    };
    LogMark last_mark;
    // game over hold (state 6)
    bool go_hold{}, go_menu_seen{};
    int logged_go{};
    int go_flags{}; ///< diagnostics: 1 Battle.gameOver, 2 GameOverMenu.gameOver
    std::string last_battle_key; ///< the last battle picture shown
    int menu_pos{-1};             ///< MainMenu.menuPos while the pause menu is open (-1 closed)
    int menu_pos_logged{-9};
    int menu_opened{};
    ce_pages::BattleHit shown_hit; ///< tap geometry of the battle picture on screen
    bool shown_unlocked{};
    int native_target{-1}; ///< enemy index under the game's target cursor (-1 none)
    int tgt_now{-1}, tgt_mode{-1}, tgt_idx{-1};
    struct Ghost {
        int last{-1}, from{-1};
        u64 until{};
    };
    std::map<std::string, Ghost> hp_ghosts;
    double od_from{-1}, od_to{}, od_disp{};
    u64 od_t0{}, od_flash_until{}, ultra_pulse_until{};
    char od_zone{};
    bool ultra_was_ready{};
    u64 fade_until{};
    std::string shown_group; ///< page group of the picture on screen (page-change fade)
    // motion overlays: full-picture cross-fade (old picture fading out over the new) and the turn
    // strip sliding in from the right when a turn passes
    std::string xf_last, xf_src;
    u64 xf_t0{~u64{}}, tick_until{};
    unsigned ticks_seen{};
    std::string frame_key; ///< FrameKey(frame), set at each refresh
    // the shown battle picture's model and tap geometry (re-read only when the picture changes)
    std::string sv_key;
    std::optional<ce_pages::BattleView> sv_model;
    ce_pages::BattleHit sv_hit;
    int strip_v_now{-1}; ///< strip variant of the current frame (-1 none)
    std::string strip_sig, strip_pending, strip_src;
    int strip_pending_v{-1}, strip_v{-1};
    u64 strip_t0{~u64{}};
    std::map<std::string, std::pair<std::vector<ce_pages::TurnEntry>, int>> strip_models;
    std::deque<std::string> strip_order;
    bool formation_screen{};
    std::map<std::string, ce_pages::FormationView> fm_models;
    std::deque<std::string> fm_order;
    /// The native Formation screen (or the Retry menu of a game-over hold) is open: fill f.fm.
    bool ReadFormation(u64 sf_gm, bool retry, Frame& f);

    // Sky Armor (ce_mech_reader / ce_page_skyarmor): tap-twice gear shift, module-local
    ce_mech::Reader mech;
    std::map<std::string, ce_sky::SkyView> sky_models;
    std::deque<std::string> sky_order;
    int sky_armed{-1};       ///< armed gear row (-1 none)
    int sky_armed_slot{-1};  ///< acting slot the arm belongs to
    u64 sky_armed_at{};      ///< sample of the arm (expires after 5 s)
    void SkyArmCheck();
};

} // namespace ce_reader
