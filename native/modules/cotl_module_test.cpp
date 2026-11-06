// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <map>
#include <string>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"
#include "cotl_fake_game.h"
#include "cotl_guest.h"
#include "cotl_action_abi.h"
#include "cotl_offsets_1_5_27.h"
#include "cotl_test_util.h"

namespace {

struct Published {
    std::map<std::string, int64_t> i64;
    std::map<std::string, double> f64;
    std::map<std::string, std::string> text;
    size_t Count() const { return i64.size() + f64.size() + text.size(); }
};

Published g_pub;
std::map<std::string, int64_t> g_flags; // package flags (@flag:…) the host would supply
int64_t GetI64(void*, const char* n, int64_t d) {
    const auto it = g_flags.find(n);
    return it == g_flags.end() ? d : it->second;
}

void PubI64(void*, const char* n, int64_t v) { g_pub.i64[n] = v; }
void PubF64(void*, const char* n, double v) { g_pub.f64[n] = v; }
void PubText(void*, const char* n, const char* v) { g_pub.text[n] = v; }
void PubAddr(void*, const char* n, uint64_t v) { g_pub.i64[n] = static_cast<int64_t>(v); }
EdenDsmodBool NotMapped(void*, uint64_t, uint64_t) { return EDEN_DSMOD_FALSE; }
EdenDsmodBool NoRead(void*, uint64_t, void*, size_t) { return EDEN_DSMOD_FALSE; }
void Log(void*, uint32_t, const char*) {}

const uint8_t kBuild[16] = {0x31, 0x6A, 0x4D, 0x48, 0x81, 0xA9, 0xF1, 0xE0,
                            0x77, 0xA0, 0x72, 0x62, 0xE9, 0x17, 0x91, 0x7C};
const char* kBuildHex = "316A4D4881A9F1E077A07262E917917C"
                        "00000000000000000000000000000000";

EdenDsmodHostApi MakeHost() {
    EdenDsmodHostApi h{};
    h.abi_version = EDEN_DSMOD_MODULE_ABI_VERSION;
    h.struct_size = sizeof(EdenDsmodHostApi);
    h.abi_hash = EDEN_DSMOD_MODULE_ABI_HASH;
    h.title_id = 0x01002E7016C46000ull;
    std::memcpy(h.build_id, kBuild, sizeof(kBuild));
    h.main_base = 0x80000000;
    h.main_size = 0x8000000;
    h.is_mapped = NotMapped;
    h.read_memory = NoRead;
    h.log = Log;
    h.publish_i64 = PubI64;
    h.publish_f64 = PubF64;
    h.publish_text = PubText;
    h.publish_address = PubAddr;
    h.get_i64 = GetI64;
    return h;
}

const EdenDsmodModuleApi* Module() {
    return eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION, EDEN_DSMOD_MODULE_ABI_HASH);
}

} // namespace

TEST(module_table_matches_abi) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK_EQ(m->title_id, uint64_t{0x01002E7016C46000ull});
    CHECK_EQ(m->struct_size, uint32_t{sizeof(EdenDsmodModuleApi)});
    CHECK(m->supports_build && m->create && m->destroy && m->sample && m->tick);
    CHECK(eden_dsmod_get_module(EDEN_DSMOD_MODULE_ABI_VERSION + 1, EDEN_DSMOD_MODULE_ABI_HASH) ==
          nullptr);
}

TEST(module_supports_only_exact_build) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK(m->supports_build(kBuildHex) != 0);
    CHECK(m->supports_build("316A4D4881A9F1E0") == 0);
    CHECK(m->supports_build("0000000000000000000000000000000000000000000000000000000000000000") == 0);
}

TEST(module_create_rejects_other_build) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    auto host = MakeHost();
    host.build_id[0] ^= 0xFF;
    CHECK(m->create(&host, "{}") == nullptr);
}

TEST(module_exports_image_extension) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    CHECK((m->capabilities & EDEN_DSMOD_CAP_EXTENSIONS) != 0);
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(ext != nullptr);
    CHECK(ext->configure != nullptr);
    CHECK(ext->load_image != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    // Unknown keys are rejected, without a romfs there is no icon
    CHECK(ext->load_image(inst, &host, "module:cotl:nothing", nullptr, nullptr) == 0);
    CHECK(ext->load_image(inst, &host, "module:cotl:item/1", nullptr, nullptr) == 0);
    m->destroy(inst);
}

// Review Focus 1: before the title screen no game memory is readable – only mod.ready and g.in_run 0
// (the tab selection in the manifest always needs g.in_run) and act.available 0 (the button derivation), nothing else.
TEST(module_sample_without_game_memory_publishes_only_ready) {
    const auto* m = Module();
    REQUIRE(m != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(g_pub.i64["mod.ready"], int64_t{1});
    CHECK_EQ(g_pub.i64["g.in_run"], int64_t{0});
    CHECK_EQ(g_pub.i64["act.available"], int64_t{0});
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{0});   // the module holds the tab and the follower window
    CHECK_EQ(g_pub.i64["fol.open"], int64_t{-1});
    CHECK_EQ(g_pub.Count(), size_t{6}); // mod.ready, nav.tab, fol.open, g.in_run, g.loaded, act.available
    m->destroy(inst);
}

// The helper constants live in C headers; pinned here so they cannot drift apart
static_assert(COTL_ACT_DM_ITEMS == cotl::off::dm::kItems);
static_assert(COTL_ACT_DM_CURRENT_DAY == cotl::off::dm::kCurrentDayIndex);
static_assert(COTL_ACT_DM_SEASON == cotl::off::dm::kCurrentSeason);
static_assert(COTL_ACT_DM_WEATHER == cotl::off::dm::kCurrentWeatherEvent);
static_assert(COTL_ACT_ITEM_TYPE == cotl::off::item::kType);
static_assert(COTL_ACT_ITEM_QUANTITY == cotl::off::item::kQuantity);
static_assert(COTL_ACT_INFO_IS_SNOWMAN == cotl::off::fi::kIsSnowman);
static_assert(COTL_ACT_INFO_KISSED == cotl::off::fi::kKissed);
static_assert(COTL_ACT_INFO_INSPIRED == cotl::off::fi::kInspired);
static_assert(COTL_ACT_INFO_PET_DOG == cotl::off::fi::kPetDog);
static_assert(COTL_ACT_INFO_INTIMIDATED == cotl::off::fi::kIntimidated);
static_assert(COTL_ACT_INFO_BRIBED == cotl::off::fi::kBribed);
static_assert(COTL_ACT_INFO_PAID_TITHES == cotl::off::fi::kPaidTithes);
static_assert(COTL_ACT_INFO_SCARED_INTERACTED == cotl::off::fi::kScaredTraitInteracted);
static_assert(COTL_ACT_INFO_WORK_THROUGH_NIGHT == cotl::off::fi::kWorkThroughNightDay);
static_assert(COTL_ACT_PLAYER_FARMING_TYPEINFO == cotl::off::kPlayerFarmingTypeInfo);
static_assert(COTL_ACT_CLASS_STATIC_FIELDS == cotl::kClassStaticFields);
static_assert(COTL_ACT_PLAYER_FARMING_INSTANCE == cotl::off::kPlayerFarmingInstance);
static_assert(COTL_ACT_PLAYER_LOCATION == cotl::off::player::kLocation);
static_assert(COTL_ACT_LOCATION_BASE == cotl::off::player::kLocationBase);
static_assert(COTL_ACT_PLAYER_STATE == cotl::off::player::kState);
static_assert(COTL_ACT_STATE_CURRENT == cotl::off::player::kStateCurrent);
static_assert(COTL_ACT_UNITY_CACHED_PTR == cotl::off::kUnityCachedPtr);
static_assert(COTL_ACT_FOLLOWER_TYPEINFO == cotl::off::kFollowerTypeInfo);
static_assert(COTL_ACT_BRAIN_TYPEINFO == cotl::off::kFollowerBrainTypeInfo);
static_assert(COTL_ACT_INFO_TYPEINFO == cotl::off::kFollowerInfoTypeInfo);
static_assert(COTL_ACT_DATAMANAGER_TYPEINFO == cotl::off::kDataManagerTypeInfo);
static_assert(COTL_ACT_FLOATING_TYPEINFO == cotl::off::kFloatingTypeInfo);
static_assert(COTL_ACT_FOLLOWER_FOLLOWERS == cotl::off::follower::kFollowers);
static_assert(COTL_ACT_DATAMANAGER_INSTANCE == cotl::off::kDataManagerInstance);
static_assert(COTL_ACT_FOLLOWER_INTERACTION == cotl::off::follower::kInteraction);
static_assert(COTL_ACT_FOLLOWER_BRAIN == cotl::off::follower::kBrain);
static_assert(COTL_ACT_FOLLOWER_STATE == cotl::off::follower::kState);
static_assert(COTL_ACT_FOLLOWER_TIMED_ACTION == cotl::off::follower::kStateTimedAction);
static_assert(COTL_ACT_INTERACTION_LIGHTNING == cotl::off::follower::kLightningIncoming);
static_assert(COTL_ACT_BRAIN_INFO == cotl::off::brain::kInfo);
static_assert(COTL_ACT_BRAIN_TASK == cotl::off::brain::kCurrentTask);
static_assert(COTL_ACT_TASK_CHANGE_LOCATION == cotl::off::kTaskChangeLocation);
static_assert(COTL_ACT_INFO_ID == cotl::off::fi::kId);
static_assert(COTL_ACT_INFO_THOUGHT == cotl::off::fi::kCursedState);
static_assert(COTL_ACT_INFO_LEAVING == cotl::off::fi::kLeavingCult);
static_assert(COTL_ACT_INFO_TRAITS == cotl::off::fi::kTraits);
static_assert(COTL_ACT_DM_CULT_TRAITS == cotl::off::dm::kCultTraits);
static_assert(COTL_ACT_DM_RECRUIT == cotl::off::dm::kRecruit);
static_assert(COTL_ACT_DM_TRAIT_MANIPULATING == cotl::off::dm::kTraitManipulatingIds);
static_assert(COTL_ACT_DM_IMPRISONED == cotl::off::dm::kImprisonedIds);
static_assert(COTL_ACT_DM_ON_MISSIONARY == cotl::off::dm::kOnMissionaryIds);
static_assert(COTL_ACT_DM_LEFT_IN_DUNGEON == cotl::off::dm::kLeftInDungeonIds);
static_assert(COTL_ACT_DM_TRANSITIONING == cotl::off::dm::kTransitioningIds);
static_assert(COTL_ACT_DM_DEMONS == cotl::off::dm::kDemonIds);
static_assert(COTL_ACT_THOUGHT_STARVING == cotl::off::fi::kThoughtStarving);
static_assert(COTL_ACT_THOUGHT_FREEZING == cotl::off::fi::kThoughtFreezing);
static_assert(COTL_ACT_THOUGHT_CHILD == cotl::off::fi::kThoughtChild);
static_assert(COTL_ACT_TRAIT_EXISTENTIAL_DREAD == cotl::off::fi::kTraitExistentialDread);
static_assert(COTL_ACT_TRAIT_MISSIONARY_TERRIFIED == cotl::off::fi::kTraitMissionaryTerrified);
static_assert(COTL_ACT_UIMENUBASE_TYPEINFO == cotl::off::kUiMenuBaseTypeInfo);
static_assert(COTL_ACT_UIMENU_ACTIVE_MENUS == cotl::off::kUiMenuActiveMenus);
static_assert(COTL_ACT_GAMEMANAGER_TYPEINFO == cotl::off::kGameManagerTypeInfo);
static_assert(COTL_ACT_GAMEMANAGER_INMENU == cotl::off::kGameManagerInMenu);
static_assert(COTL_ACT_MMTRANSITION_TYPEINFO == cotl::off::kMmTransitionTypeInfo);
static_assert(COTL_ACT_MMTRANSITION_ISPLAYING == cotl::off::kMmTransitionIsPlaying);
static_assert(COTL_ACT_MMCONVERSATION_TYPEINFO == cotl::off::kMmConversationTypeInfo);
static_assert(COTL_ACT_MMCONVERSATION_ISPLAYING == cotl::off::kMmConversationIsPlaying);
static_assert(COTL_ACT_PLAYER_PLAYERS == cotl::off::player::kPlayers);
static_assert(COTL_ACT_PLAYER_IN_CONVERSATION == cotl::off::player::kWasInConversation);
static_assert(COTL_ACT_PLAYER_PICKED_UP == cotl::off::player::kPickedUpFollower);
static_assert(COTL_ACT_FOLLOWER_DYING == cotl::off::follower::kDying);
static_assert(COTL_ACT_PLAYER_CONTROLLER == cotl::off::player::kPlayerController);
static_assert(COTL_ACT_PLAYER_UNIT == cotl::off::player::kUnitObject);
static_assert(COTL_ACT_PLAYER_GOTO_FLAG == cotl::off::player::kGoToAndStopping);
static_assert(COTL_ACT_PLAYER_GOTO_CALLBACK == cotl::off::player::kGoToCallback);
static_assert(COTL_ACT_PLAYER_ABORT_CALLBACK == cotl::off::player::kAbortGoToCallback);
static_assert(COTL_ACT_PLAYER_GOTO_TARGET == cotl::off::player::kGoToTarget);
static_assert(COTL_ACT_PLAYER_LAST_POSITION == cotl::off::player::kLastPosition);
static_assert(COTL_ACT_INTERACT_STATE == cotl::off::follower::kInteractState);
static_assert(COTL_ACT_INTERACT_INTERACTABLE == cotl::off::follower::kInteractInteractable);
static_assert(COTL_ACT_INTERACT_PLAYER == cotl::off::follower::kInteractPlayer);
static_assert(COTL_ACT_INTERACT_ACTIVATED == cotl::off::follower::kInteractActivated);
static_assert(COTL_ACT_INTERACT_TASK == cotl::off::follower::kInteractTask);
static_assert(COTL_ACT_MANUAL_TYPEINFO == cotl::off::kManualControlTypeInfo);
static_assert(COTL_ACT_MANUAL_HARD_TYPEINFO == cotl::off::kHardManualControlTypeInfo);
static_assert(COTL_ACT_MANUAL_LIGHT_TYPEINFO == cotl::off::kLightManualControlTypeInfo);
static_assert(COTL_ACT_MANUAL_ENFORCING_TYPEINFO == cotl::off::kEnforcingManualControlTypeInfo);
static_assert(COTL_ACT_SLEEP_TYPEINFO == cotl::off::kSleepTaskTypeInfo);
static_assert(COTL_ACT_INFO_RECEIVED_BLESSING == cotl::off::fi::kReceivedBlessing);
static_assert(COTL_ACT_BED_REST_TYPEINFO == cotl::off::kBedRestTaskTypeInfo);
static_assert(COTL_ACT_STATE_LOCK == cotl::off::player::kStateLock);
static_assert(COTL_ACT_LAMB_INACTIVE == cotl::off::player::kStateInActive);
static_assert(COTL_ACT_INTERACT_ANIMATOR == cotl::off::follower::kInteractAnimator);
static_assert(COTL_ACT_TASK_STATE == cotl::off::kTaskState);
static_assert(COTL_ACT_TASK_STATE_DOING == cotl::off::kTaskStateDoing);

namespace {

struct FakeMailbox {
    uint32_t heartbeat = 0;
    uint32_t axis_x = 0;
    uint32_t axis_y = 0;
    bool hooked = true; // false: no hook installed, load_u32 fails
    uint32_t last_offset = 0xFFFFFFFF;
};

EdenDsmodBool LoadU32(void* user, uint32_t offset, uint32_t* value) {
    auto* box = static_cast<FakeMailbox*>(user);
    box->last_offset = offset;
    if (!box->hooked) {
        return EDEN_DSMOD_FALSE;
    }
    switch (offset) {
    case COTL_ACT_HEARTBEAT:
        *value = box->heartbeat;
        return EDEN_DSMOD_TRUE;
    case COTL_ACT_RUN_AXIS_X:
        *value = box->axis_x;
        return EDEN_DSMOD_TRUE;
    case COTL_ACT_RUN_AXIS_Y:
        *value = box->axis_y;
        return EDEN_DSMOD_TRUE;
    default:
        return EDEN_DSMOD_FALSE;
    }
}

} // namespace

// Diagnostics until task 8: act.dbg_axis holds the raw float bits, y on top, x below
TEST(module_publishes_raw_stick_read) {
    const auto* m = Module();
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(m != nullptr);
    REQUIRE(ext != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    FakeMailbox box;
    EdenDsmodHostExtensions hx{};
    hx.userdata = &box;
    hx.load_u32 = LoadU32;
    ext->configure(inst, &hx);

    box.axis_x = 0xBF800000u; // -1.0
    box.axis_y = 0x3F000000u; // 0.5
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(g_pub.i64["act.dbg_axis"], int64_t{0x3F000000BF800000ll});
    box.hooked = false;
    g_pub = {};
    m->sample(inst, &host);
    CHECK(g_pub.i64.count("act.dbg_axis") == 0);
    m->destroy(inst);
}

// Debug value of the helper: act.heartbeat comes from the first mailbox word
TEST(module_publishes_heartbeat_from_mailbox) {
    const auto* m = Module();
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(m != nullptr);
    REQUIRE(ext != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    FakeMailbox box;
    EdenDsmodHostExtensions hx{};
    hx.userdata = &box;
    hx.load_u32 = LoadU32;
    ext->configure(inst, &hx);

    box.heartbeat = 41;
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(box.last_offset, uint32_t{COTL_ACT_HEARTBEAT});
    CHECK_EQ(g_pub.i64["act.heartbeat"], int64_t{41});
    box.heartbeat = 0xFFFFFFFFu; // published unsigned
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(g_pub.i64["act.heartbeat"], int64_t{0xFFFFFFFFll});
    m->destroy(inst);
}

// As long as no hook is installed (other build, plan rejected), the value is missing entirely
TEST(module_omits_heartbeat_without_hook) {
    const auto* m = Module();
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(m != nullptr);
    REQUIRE(ext != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    // Without Configure and without load_u32
    g_pub = {};
    m->sample(inst, &host);
    CHECK(g_pub.i64.count("act.heartbeat") == 0);
    // With Configure, but load_u32 fails
    FakeMailbox box;
    box.hooked = false;
    EdenDsmodHostExtensions hx{};
    hx.userdata = &box;
    hx.load_u32 = LoadU32;
    ext->configure(inst, &hx);
    g_pub = {};
    m->sample(inst, &host);
    CHECK(g_pub.i64.count("act.heartbeat") == 0);
    m->destroy(inst);
}

// fol.act without a selected follower or with an unknown row is rejected (haptics "refused")
TEST(module_refuses_follower_action_without_selection) {
    const auto* m = Module();
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(m != nullptr);
    REQUIRE(ext != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    CHECK(ext->on_action(inst, "fol.act", 0) == EDEN_DSMOD_FALSE);  // without Configure, without selection
    CHECK(ext->on_action(inst, "fol.act", 7) == EDEN_DSMOD_FALSE);
    CHECK(ext->on_action(inst, "fol.act", -1) == EDEN_DSMOD_FALSE);
    m->destroy(inst);
}

// Without a hook the module shows no buttons; with a live heartbeat the list appears
TEST(module_publishes_actions_only_with_helper) {
    const auto* m = Module();
    const auto* ext = eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH);
    REQUIRE(m != nullptr);
    REQUIRE(ext != nullptr);
    auto host = MakeHost();
    void* inst = m->create(&host, "{}");
    REQUIRE(inst != nullptr);
    g_pub = {};
    m->sample(inst, &host);
    CHECK_EQ(g_pub.i64["act.available"], int64_t{0});
    CHECK(g_pub.i64.count("act.count") == 0);
    FakeMailbox box;
    EdenDsmodHostExtensions hx{};
    hx.userdata = &box;
    hx.load_u32 = LoadU32;
    ext->configure(inst, &hx);
    for (uint32_t i = 1; i <= 3; ++i) {
        box.heartbeat = i;
        g_pub = {};
        m->sample(inst, &host);
    }
    CHECK_EQ(g_pub.i64["act.available"], int64_t{1});
    CHECK_EQ(g_pub.i64["act.count"], int64_t{0}); // no follower selected
    m->destroy(inst);
}

namespace {
// Mailbox as a free word list; counts writes
struct WordBox {
    std::map<uint32_t, uint32_t> words;
    int stores = 0;
};
EdenDsmodBool WordLoad(void* user, uint32_t off, uint32_t* v) {
    *v = static_cast<WordBox*>(user)->words[off];
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool WordStore(void* user, uint32_t off, uint32_t v) {
    auto* box = static_cast<WordBox*>(user);
    box->words[off] = v;
    ++box->stores;
    return EDEN_DSMOD_TRUE;
}
EdenDsmodBool GameRead(void* user, uint64_t addr, void* out, size_t n) {
    return static_cast<FakeGuest*>(user)->Read(addr, out, n) ? EDEN_DSMOD_TRUE : EDEN_DSMOD_FALSE;
}

// Module with fake game, live helper and selected follower 7 ("Bramble")
struct SelectedFollower {
    fake::Game game;
    WordBox box;
    EdenDsmodHostApi host = MakeHost();
    EdenDsmodHostExtensions hx{};
    const EdenDsmodModuleApi* m = Module();
    const auto* Ext() { return eden_dsmod_get_extensions(EDEN_DSMOD_EXT_VERSION, EDEN_DSMOD_EXT_HASH); }
    void* inst = nullptr;

    SelectedFollower() {
        game.SetFollowers({{7, "Bramble"}});
        game.g.PutU8(fake::kDataManager + 0x75A, 1); // ShowLoyaltyBars
        game.g.PutU8(fake::kDataManager + 0x850, 1); // InTutorial
    }
    void Start() {
        host.userdata = &game.g;
        host.read_memory = GameRead;
        inst = m->create(&host, "{}");
        hx.userdata = &box;
        hx.load_u32 = WordLoad;
        hx.store_u32 = WordStore;
        Ext()->configure(inst, &hx);
        Samples(1); // first state, no selection yet
        Open(0);
        Samples(3);
    }
    void Samples(int n) {
        for (int i = 0; i < n; ++i) {
            ++box.words[COTL_ACT_HEARTBEAT];
            g_pub = {};
            m->sample(inst, &host);
        }
    }
    // Tap the follower card (row) or -1 = close the window
    void Open(int64_t row) { Ext()->on_action(inst, "fol.open", row); }
    bool Tap(int row) { return Ext()->on_action(inst, "fol.act", row) == EDEN_DSMOD_TRUE; }
    ~SelectedFollower() {
        if (inst) m->destroy(inst);
    }
};
} // namespace

TEST(module_lists_bless_for_selected_follower) {
    SelectedFollower t;
    t.Start();
    CHECK_EQ(g_pub.i64["act.available"], int64_t{1});
    CHECK_EQ(g_pub.i64["act.count"], int64_t{4}); // Bless, Work, Sleep, Eat (grey: no meal)
    CHECK_EQ(g_pub.text["act.name0"], std::string("Bless"));
    CHECK_EQ(g_pub.i64["act.command0"], int64_t{59});
    CHECK_EQ(g_pub.i64["act.enabled0"], int64_t{1});
    CHECK_EQ(g_pub.text["act.name2"], std::string("Sleep"));
    CHECK_EQ(g_pub.i64["act.command2"], int64_t{26});
    // The tap goes to the helper; the poll itself writes nothing to the mailbox
    CHECK_EQ(t.box.stores, 0);
    CHECK(t.Tap(0));
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{59});
    CHECK_EQ(t.box.words[COTL_ACT_REQ_FOLLOWER], uint32_t{7});
}

TEST(module_tap_on_locked_command_sends_nothing) {
    SelectedFollower t;
    t.game.g.PutU8(fake::Game::FollowerObj(0) + 0x2D1, 1); // ReceivedBlessing
    t.Start();
    CHECK_EQ(g_pub.i64["act.count"], int64_t{4});
    CHECK_EQ(g_pub.i64["act.enabled0"], int64_t{0});
    CHECK_EQ(g_pub.i64["act.locked0"], int64_t{1});
    CHECK_EQ(g_pub.text["act.message"], std::string("Bless: Already blessed today, Eat: No meals at the base"));
    CHECK(!t.Tap(0));
    CHECK_EQ(t.box.stores, 0);
}

TEST(module_hides_bless_when_the_tutorial_flag_is_off) {
    SelectedFollower t;
    t.game.g.PutU8(fake::kDataManager + 0x850, 0);
    t.Start();
    CHECK_EQ(g_pub.i64["act.count"], int64_t{1}); // Work stays
    CHECK_EQ(g_pub.text["act.name0"], std::string("Work"));
    CHECK(t.Tap(0)); // only opens the submenu
    CHECK_EQ(t.box.stores, 0);
}

// The game does not offer Bless: no button, and an old tap does not reach the helper
TEST(module_hides_bless_when_the_game_does_not_offer_it) {
    SelectedFollower t;
    t.game.g.PutS32(fake::Game::FollowerObj(0) + 0x20, 0x42); // Dissenter
    t.Start();
    CHECK_EQ(g_pub.i64["act.available"], int64_t{1});
    CHECK_EQ(g_pub.i64["act.count"], int64_t{1}); // Eat, grey without a meal
    CHECK_EQ(g_pub.i64["act.locked0"], int64_t{1});
    CHECK(!t.Tap(0));
    CHECK_EQ(t.box.stores, 0);
}

TEST(module_sends_the_command_of_each_button) {
    SelectedFollower t;
    t.Start();
    CHECK(t.Tap(2));
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{26});
}


// Submenus in the same window: Work swaps the buttons for its roles with Back at the front; Send,
// Back and Close lead to the top level
TEST(module_work_opens_its_sub_menu_and_back_returns) {
    SelectedFollower t;
    t.Start();
    CHECK_EQ(g_pub.text["act.name1"], std::string("Work"));
    CHECK(t.Tap(1));
    t.Samples(1);
    CHECK_EQ(t.box.stores, 0);
    CHECK_EQ(g_pub.i64["act.count"], int64_t{4}); // Back, wood, rubble, Build (grey)
    CHECK_EQ(g_pub.text["act.name0"], std::string("Back"));
    CHECK_EQ(g_pub.text["act.name1"], std::string("Chop Trees"));
    CHECK_EQ(g_pub.i64["act.locked3"], int64_t{1});
    CHECK(!t.Tap(3));
    CHECK(t.Tap(1));
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{14});
    CHECK_EQ(t.box.words[COTL_ACT_REQ_PARENT], uint32_t{1});
    t.Samples(1);
    CHECK_EQ(g_pub.text["act.name1"], std::string("Work"));

    CHECK(t.Tap(1));
    t.Samples(1);
    CHECK_EQ(g_pub.text["act.name0"], std::string("Back"));
    CHECK(t.Tap(0));
    t.Samples(1);
    CHECK_EQ(g_pub.text["act.name1"], std::string("Work"));

    CHECK(t.Tap(1));
    t.Samples(1);
    t.Open(-1);
    t.Samples(1);
    t.Open(0);
    t.Samples(2);
    CHECK_EQ(g_pub.text["act.name1"], std::string("Work"));
}

// Link from the problem window: Followers tab and the follower's window at once
TEST(module_problem_link_opens_follower_actions) {
    SelectedFollower t;
    t.game.SetFollowers({{7, "Bramble", 1, 5}}); // hungry: one problem
    t.Start();
    t.Open(-1);
    REQUIRE(t.Ext()->on_action(t.inst, "tab", 2) == EDEN_DSMOD_TRUE);
    t.Samples(1);
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{2});
    CHECK_EQ(g_pub.i64["fol.open"], int64_t{-1});
    CHECK(t.Ext()->on_action(t.inst, "prob.goto", 0) == EDEN_DSMOD_FALSE); // no problem window open
    g_flags["@flag:prob.open"] = 0;
    t.Samples(2);
    g_flags.clear();
    REQUIRE(t.Ext()->on_action(t.inst, "prob.goto", 0) == EDEN_DSMOD_TRUE); // argument irrelevant
    t.Samples(2);
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{1}); // Followers
    CHECK_EQ(g_pub.i64["fol.open"], int64_t{0});
    CHECK_EQ(g_pub.text["act.name0"], std::string("Bless"));
    CHECK(t.Ext()->on_action(t.inst, "tab", 6) == EDEN_DSMOD_FALSE);
    // Closing returns to Problems
    t.Open(-1);
    t.Samples(1);
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{2});
    CHECK_EQ(g_pub.i64["fol.open"], int64_t{-1});
    // After that the way back is used up: opened and closed normally stays Followers
    REQUIRE(t.Ext()->on_action(t.inst, "tab", 1) == EDEN_DSMOD_TRUE);
    t.Open(0);
    t.Open(-1);
    t.Samples(1);
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{1});
}

// Opened via the link, then switched the tab yourself: no jump back any more
TEST(module_problem_link_return_cleared_by_tab_change) {
    SelectedFollower t;
    t.game.SetFollowers({{7, "Bramble", 1, 5}});
    t.Start();
    t.Open(-1);
    REQUIRE(t.Ext()->on_action(t.inst, "tab", 2) == EDEN_DSMOD_TRUE);
    g_flags["@flag:prob.open"] = 0;
    t.Samples(2);
    g_flags.clear();
    REQUIRE(t.Ext()->on_action(t.inst, "prob.goto", 0) == EDEN_DSMOD_TRUE);
    REQUIRE(t.Ext()->on_action(t.inst, "tab", 3) == EDEN_DSMOD_TRUE);
    REQUIRE(t.Ext()->on_action(t.inst, "tab", 1) == EDEN_DSMOD_TRUE);
    t.Open(-1);
    t.Samples(1);
    CHECK_EQ(g_pub.i64["nav.tab"], int64_t{1});
}

// Walk over in the quest window: only go to the follower who offers a quest
TEST(module_walks_to_the_quest_giver) {
    SelectedFollower t;
    t.game.SetBrains({{0, true, 8}}); // Bramble offers a quest
    t.Start();
    t.Open(-1);
    t.Samples(1);
    CHECK(t.Ext()->on_action(t.inst, "quest.walk", 0) == EDEN_DSMOD_FALSE); // no quest window open
    g_flags["@flag:quest.open"] = 0;
    t.Samples(2);
    g_flags.clear();
    CHECK_EQ(g_pub.i64["quest.sel_offer"], int64_t{1});
    REQUIRE(t.Ext()->on_action(t.inst, "quest.walk", 0) == EDEN_DSMOD_TRUE);
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{COTL_ACT_COMMAND_WALK_ONLY});
    CHECK_EQ(t.box.words[COTL_ACT_REQ_FOLLOWER], uint32_t{7});
    // The helper reports the walk: in the quest window, not in the follower window
    t.box.words[COTL_ACT_ST_SEQ] = t.box.words[COTL_ACT_REQ_SEQ];
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_WALKING;
    t.Samples(2);
    CHECK_EQ(g_pub.text["walk.msg"], std::string("Walking over"));
    CHECK_EQ(g_pub.i64["walk.active"], int64_t{1});
    CHECK(g_pub.text["act.message"].find("Walking") == std::string::npos);
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_DONE;
    t.Samples(1);
    CHECK_EQ(g_pub.text["walk.msg"], std::string("Arrived"));
    // A follower window opened afterwards shows its own status line again
    t.Open(0);
    t.Samples(1);
    CHECK_EQ(g_pub.text["walk.msg"], std::string());
    CHECK_EQ(g_pub.i64["walk.active"], int64_t{0});
}

// Walk over in the follower window: to the open follower, message until the window closes
TEST(module_walks_to_the_open_follower) {
    SelectedFollower t;
    t.Start();
    REQUIRE(t.Ext()->on_action(t.inst, "fol.walk", 0) == EDEN_DSMOD_TRUE);
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{COTL_ACT_COMMAND_WALK_ONLY});
    CHECK_EQ(t.box.words[COTL_ACT_REQ_FOLLOWER], uint32_t{7});
    t.box.words[COTL_ACT_ST_SEQ] = t.box.words[COTL_ACT_REQ_SEQ];
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_WALKING;
    t.Samples(1);
    CHECK_EQ(g_pub.text["walk.msg"], std::string("Walking over"));
    CHECK_EQ(g_pub.i64["walk.active"], int64_t{1});
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_DONE;
    t.Samples(1);
    CHECK_EQ(g_pub.text["walk.msg"], std::string("Arrived"));
    t.Open(-1);
    t.Samples(1);
    CHECK_EQ(g_pub.i64["walk.active"], int64_t{0});
    CHECK(t.Ext()->on_action(t.inst, "fol.walk", 0) == EDEN_DSMOD_FALSE); // no window open
}

// After Walk over the reopened window shows no "Done" of the walking in act.message
TEST(module_walk_leaves_no_done_message) {
    SelectedFollower t;
    t.Start();
    REQUIRE(t.Ext()->on_action(t.inst, "fol.walk", 0) == EDEN_DSMOD_TRUE);
    t.box.words[COTL_ACT_ST_SEQ] = t.box.words[COTL_ACT_REQ_SEQ];
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_DONE;
    t.Samples(1);
    t.Open(-1);
    t.Samples(1);
    CHECK(g_pub.text["act.message"] != std::string("Done"));
    t.Open(0);
    t.Samples(1);
    CHECK(g_pub.text["act.message"] != std::string("Done"));
    // A real command afterwards shows its status again
    REQUIRE(t.Ext()->on_action(t.inst, "fol.act", 0) == EDEN_DSMOD_TRUE);
    t.box.words[COTL_ACT_ST_SEQ] = t.box.words[COTL_ACT_REQ_SEQ];
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_DONE;
    t.Samples(1);
    CHECK_EQ(g_pub.text["act.message"], std::string("Done"));
}

// Walk over in the problem window: to the follower with the problems
TEST(module_walks_to_the_problem_follower) {
    SelectedFollower t;
    t.game.SetFollowers({{7, "Bramble", 1, 5}}); // hungry: one problem
    t.Start();
    t.Open(-1);
    t.Samples(1);
    CHECK(t.Ext()->on_action(t.inst, "prob.walk", 0) == EDEN_DSMOD_FALSE); // no problem window open
    g_flags["@flag:prob.open"] = 0;
    t.Samples(2);
    g_flags.clear();
    REQUIRE(t.Ext()->on_action(t.inst, "prob.walk", 0) == EDEN_DSMOD_TRUE);
    CHECK_EQ(t.box.words[COTL_ACT_REQ_COMMAND], uint32_t{COTL_ACT_COMMAND_WALK_ONLY});
    CHECK_EQ(t.box.words[COTL_ACT_REQ_FOLLOWER], uint32_t{7});
    t.box.words[COTL_ACT_ST_SEQ] = t.box.words[COTL_ACT_REQ_SEQ];
    t.box.words[COTL_ACT_ST_STATE] = COTL_ACT_STATE_WALKING;
    t.Samples(1);
    CHECK_EQ(g_pub.text["walk.msg"], std::string("Walking over"));
}

// Without an offered quest there is no Walk over
TEST(module_quest_walk_needs_an_offer) {
    SelectedFollower t;
    t.game.SetBrains({{0, true, 0}}); // attention, but no quest
    t.Start();
    g_flags["@flag:quest.open"] = 0;
    t.Samples(2);
    g_flags.clear();
    CHECK_EQ(g_pub.i64["quest.sel_offer"], int64_t{0});
    CHECK(t.Ext()->on_action(t.inst, "quest.walk", 0) == EDEN_DSMOD_FALSE);
}
