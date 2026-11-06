// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_snapshot.h"

#include <algorithm>
#include <cmath>

#include "cotl_offsets_1_5_27.h"

namespace cotl {

namespace {

std::optional<uint64_t> DataManager(const Guest& g, uint64_t main_base) {
    const auto statics = StaticFields(g, main_base, off::kDataManagerTypeInfo);
    if (!statics) {
        return std::nullopt;
    }
    return ReadPtr(g, *statics + off::kDataManagerInstance);
}

// Slot colours of a skin as in SetFollowerCostume: first SkinAndData entry with the name in the
// skin list, colour set SkinColour clamped to [0, count − 1]
std::vector<SlotTint> ReadTints(const Guest& g, uint64_t main_base, const std::string& skin, int32_t colour,
                                SkinColourCache* cache) {
    const auto statics = StaticFields(g, main_base, off::kWorshipperDataTypeInfo);
    const auto inst = statics ? ReadPtr(g, *statics + off::wd::kInstance) : std::nullopt;
    if (!inst || skin.empty()) {
        return {};
    }
    SkinColourCache local;
    SkinColourCache& c = cache != nullptr ? *cache : local;
    if (c.instance != *inst || c.skin_data.empty()) { // empty shortly after loading: read again
        c = SkinColourCache{};
        c.instance = *inst;
        const auto chars = ReadPtr(g, *inst + off::wd::kCharacters);
        for (const uint64_t sad : chars ? ReadRefList(g, *chars).value_or(std::vector<uint64_t>{})
                                        : std::vector<uint64_t>{}) {
            const auto skins = ReadPtr(g, sad + off::wd::kSkin);
            for (const uint64_t cs : skins ? ReadRefList(g, *skins).value_or(std::vector<uint64_t>{})
                                           : std::vector<uint64_t>{}) {
                const auto name = ReadPtr(g, cs + off::wd::kSkinName);
                if (name) {
                    c.skin_data.emplace(ReadString(g, *name).value_or(""), sad); // first wins
                }
            }
        }
    }
    const auto key = std::make_pair(skin, colour);
    if (const auto it = c.tints.find(key); it != c.tints.end()) {
        return it->second;
    }
    std::vector<SlotTint> out;
    const auto sad = c.skin_data.find(skin);
    const auto sets_list = sad != c.skin_data.end() ? ReadPtr(g, sad->second + off::wd::kSlotAndColours)
                                                    : std::nullopt;
    const auto sets = sets_list ? ReadRefList(g, *sets_list) : std::nullopt;
    if (sets && !sets->empty()) {
        const auto n = static_cast<int32_t>(sets->size());
        const uint64_t set = (*sets)[static_cast<size_t>(colour < 0 ? 0 : std::min(colour, n - 1))];
        const auto list = ReadPtr(g, set + off::wd::kSetList);
        for (const uint64_t e : list ? ReadRefList(g, *list).value_or(std::vector<uint64_t>{})
                                     : std::vector<uint64_t>{}) {
            const auto slot = ReadPtr(g, e + off::wd::kSlot);
            if (!slot) {
                continue;
            }
            uint32_t rgba = 0;
            for (uint64_t k = 0; k < 4; ++k) {
                const float v = std::clamp(ReadPod<float>(g, e + off::wd::kColor + k * 4).value_or(1.0f), 0.0f, 1.0f);
                rgba = rgba << 8 | static_cast<uint32_t>(std::lround(v * 255.0f));
            }
            out.push_back({ReadString(g, *slot).value_or(""), rgba});
        }
    }
    if (!out.empty()) { // empty = not there yet (or skin without colours): do not hold on to it
        c.tints.emplace(key, out);
    }
    return out;
}

constexpr float kMaxHalfHearts = 88;
constexpr size_t kMaxTarot = 60;

std::optional<Follower> ReadFollower(const Guest& g, uint64_t obj, int32_t thoughts_for) {
    Follower f;
    const auto id = ReadPod<int32_t>(g, obj + off::fi::kId);
    if (!id) {
        return std::nullopt;
    }
    f.id = *id;
    if (const auto name_obj = ReadPtr(g, obj + off::fi::kName)) {
        f.name = ReadString(g, *name_obj).value_or("");
    }
    f.level = ReadPod<int32_t>(g, obj + off::fi::kXpLevel).value_or(0);
    f.age = ReadPod<int32_t>(g, obj + off::fi::kAge).value_or(0);
    f.life_expectancy = ReadPod<int32_t>(g, obj + off::fi::kLifeExpectancy).value_or(0);
    f.role = ReadPod<int32_t>(g, obj + off::fi::kRole).value_or(0);
    f.cursed_state = ReadPod<int32_t>(g, obj + off::fi::kCursedState).value_or(0);
    f.dwelling_id = ReadPod<int32_t>(g, obj + off::fi::kDwellingId).value_or(0);
    f.old_age = ReadPod<uint8_t>(g, obj + off::fi::kOldAge).value_or(0) != 0;
    if (const auto traits = ReadPtr(g, obj + off::fi::kTraits)) {
        f.traits = ReadValueList<int32_t>(g, *traits).value_or(std::vector<int32_t>{});
    }
    const auto list = thoughts_for == kAllThoughts || thoughts_for == f.id
                          ? ReadPtr(g, obj + off::fi::kThoughts)
                          : std::nullopt;
    if (list) {
        for (const uint64_t t : ReadRefList(g, *list).value_or(std::vector<uint64_t>{})) {
            ThoughtEntry e;
            e.type = ReadPod<int32_t>(g, t + off::thought::kType).value_or(0);
            e.modifier = ReadPod<float>(g, t + off::thought::kModifier).value_or(0);
            e.quantity = ReadPod<int32_t>(g, t + off::thought::kQuantity).value_or(1);
            f.thoughts.push_back(e);
        }
    }
    f.satiation = ReadPod<float>(g, obj + off::fi::kSatiation).value_or(0);
    f.starvation = ReadPod<float>(g, obj + off::fi::kStarvation).value_or(0);
    f.faith = ReadPod<float>(g, obj + off::fi::kFaith).value_or(0);
    f.happiness = ReadPod<float>(g, obj + off::fi::kHappiness).value_or(0);
    f.adoration = ReadPod<float>(g, obj + off::fi::kAdoration).value_or(0);
    f.illness = ReadPod<float>(g, obj + off::fi::kIllness).value_or(0);
    f.injured = ReadPod<float>(g, obj + off::fi::kInjured).value_or(0);
    f.exhaustion = ReadPod<float>(g, obj + off::fi::kExhaustion).value_or(0);
    f.dissent = ReadPod<float>(g, obj + off::fi::kDissent).value_or(0);
    if (const auto skin = ReadPtr(g, obj + off::fi::kSkinName)) {
        f.skin = ReadString(g, *skin).value_or("");
    }
    f.skin_colour = ReadPod<int32_t>(g, obj + off::fi::kSkinColour).value_or(0);
    return f;
}

// Task of the follower as DefaultCommands sees it (Follower.Brain.CurrentTask): if he is not in
// the scene, there is none. Same check as sleeping() in the helper. in_sleep_task is the question of
// MakeDemandCommands to get_CurrentTaskType: without subtask, in any state.
void ReadFollowerTask(const Guest& g, uint64_t main_base, int32_t id, FollowerCommandState& c) {
    const auto statics = StaticFields(g, main_base, off::kFollowerTypeInfo);
    const auto list = statics ? ReadPtr(g, *statics + off::follower::kFollowers) : std::nullopt;
    const auto scene = list ? ReadRefList(g, *list) : std::nullopt;
    if (!scene) {
        return;
    }
    for (const uint64_t follower : *scene) {
        const auto brain = ReadPtr(g, follower + off::follower::kBrain);
        const auto info = brain ? ReadPtr(g, *brain + off::brain::kInfo) : std::nullopt;
        if (!info || ReadPod<int32_t>(g, *info + off::fi::kId) != id) {
            continue;
        }
        if (const auto machine = ReadPtr(g, follower + off::follower::kState)) {
            c.timed_action = ReadPod<int32_t>(g, *machine + off::player::kStateCurrent) ==
                             off::follower::kStateTimedAction;
        }
        const auto task = ReadPtr(g, *brain + off::brain::kCurrentTask);
        if (!task || ReadPtr(g, *task + off::kTaskChangeLocation)) {
            return;
        }
        const auto klass = ReadPtr(g, *task);
        const bool sleep = klass && klass == ReadPtr(g, main_base + off::kSleepTaskTypeInfo);
        const bool bed_rest = klass && klass == ReadPtr(g, main_base + off::kBedRestTaskTypeInfo);
        c.in_sleep_task = sleep || bed_rest;
        if (ReadPod<int32_t>(g, *task + off::kTaskState) == off::kTaskStateDoing) {
            c.task = sleep      ? FollowerTaskKind::Sleep
                     : bed_rest ? FollowerTaskKind::BedRest
                                : FollowerTaskKind::None;
        }
        return;
    }
}

std::vector<int32_t> IdList(const Guest& g, uint64_t dm, uint64_t field) {
    if (const auto list = ReadPtr(g, dm + field)) {
        return ReadValueList<int32_t>(g, *list).value_or(std::vector<int32_t>{});
    }
    return {};
}

bool Contains(const std::vector<int32_t>& v, int32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

Objective ReadObjective(const Guest& g, uint64_t o) {
    Objective q;
    q.type = ReadPod<int32_t>(g, o + off::obj::kType).value_or(0);
    q.follower_id = ReadPod<int32_t>(g, o + off::obj::kFollower).value_or(-1);
    q.complete = ReadPod<uint8_t>(g, o + off::obj::kIsComplete).value_or(0) != 0;
    q.failed = ReadPod<uint8_t>(g, o + off::obj::kIsFailed).value_or(0) != 0;
    q.expire = ReadPod<float>(g, o + off::obj::kExpireTimestamp).value_or(0);
    q.has_expiry = ReadPod<float>(g, o + off::obj::kExpireDuration).value_or(0) > 0;
    if (q.type == off::obj::kTypeCollectItem) {
        q.item_type = ReadPod<int32_t>(g, o + off::obj::kCollectItemType).value_or(-1);
    } else if (q.type == off::obj::kTypeGiveItem) {
        q.item_type = ReadPod<int32_t>(g, o + off::obj::kGiveItemType).value_or(-1);
    }
    if (const auto gid = ReadPtr(g, o + off::obj::kGroupId)) {
        q.group_id = ReadString(g, *gid).value_or("");
    }
    if (const auto uid = ReadPtr(g, o + off::obj::kUniqueGroupId)) {
        q.unique_group = ReadString(g, *uid).value_or("");
    }
    for (size_t k = 0; k < q.ints.size(); ++k) {
        q.ints[k] = ReadPod<int32_t>(g, o + off::obj::kSubclassFields + k * 4).value_or(0);
    }
    if (q.type == off::obj::kTypeRecruitCursed || q.type == off::obj::kTypeFindFollower) {
        const uint64_t at = q.type == off::obj::kTypeFindFollower ? 0x90 : 0x88;
        if (const auto name = ReadPtr(g, o + at)) {
            q.str2 = ReadString(g, *name).value_or("");
        }
    }
    if (q.type == off::obj::kTypeBedRest || q.type == off::obj::kTypeGiveItem ||
        q.type == off::obj::kTypeKnucklebones || q.type == off::obj::kTypeFlockade) {
        if (const auto name = ReadPtr(g, o + off::obj::kSubclassFields)) {
            q.str78 = ReadString(g, *name).value_or("");
        }
    }
    return q;
}

// Open (Objectives) and completed (CompletedObjectives) steps, like QuestsMenu.OnShowStarted
std::vector<Objective> ReadObjectives(const Guest& g, uint64_t dm) {
    std::vector<Objective> out;
    for (const uint64_t field : {off::dm::kObjectives, off::dm::kCompletedObjectives}) {
        const bool from_completed = field == off::dm::kCompletedObjectives;
        const auto list = ReadPtr(g, dm + field);
        const auto objs = list ? ReadRefList(g, *list) : std::nullopt;
        for (const uint64_t o : objs.value_or(std::vector<uint64_t>{})) {
            out.push_back(ReadObjective(g, o));
            out.back().complete = out.back().complete || from_completed;
        }
    }
    return out;
}

// Followers whose current task is FollowerTask_GetAttention with ComplaintType.GiveQuest
// (the "!" above the head: they want to give a quest).
std::vector<int32_t> ReadOfferedQuests(const Guest& g, uint64_t main_base) {
    std::vector<int32_t> out;
    const auto statics = StaticFields(g, main_base, off::kFollowerBrainTypeInfo);
    const auto attention_class = ReadPtr(g, main_base + off::kGetAttentionTypeInfo);
    const auto list = statics ? ReadPtr(g, *statics + off::kAllBrains) : std::nullopt;
    const auto brains = list ? ReadRefList(g, *list) : std::nullopt;
    if (!brains || !attention_class) {
        return out;
    }
    for (const uint64_t brain : *brains) {
        const auto task = ReadPtr(g, brain + off::brain::kCurrentTask);
        if (!task || ReadPtr(g, *task) != attention_class ||
            ReadPod<int32_t>(g, *task + off::kComplaintType) != off::kComplaintGiveQuest) {
            continue;
        }
        if (const auto info = ReadPtr(g, brain + off::brain::kInfo)) {
            if (const auto id = ReadPod<int32_t>(g, *info + off::fi::kId)) {
                out.push_back(*id);
            }
        }
    }
    return out;
}

// Food and work (tools/cotl/README.md): base buildings for the roles and pub, meals (lying
// Structures_Meal and stocks of kitchens and storage, MealCommands 0x20356E0 checks no reservation),
// free drinks of pubs and storage (DrinkSomething: QuantityReserved <= 0), plus the declaration clock.
void ReadFoodAndWork(const Guest& g, uint64_t main_base, uint64_t dm, FollowerCommandState& c) {
    if (const auto list = ReadPtr(g, dm + off::dm::kBaseStructures)) {
        for (const uint64_t o : ReadRefList(g, *list, 4096).value_or(std::vector<uint64_t>{})) {
            const int32_t type = ReadPod<int32_t>(g, o + off::structure::kType).value_or(0);
            c.structures.push_back(type);
            if (const int32_t meal = MealItemOfStructure(type); meal >= 0) c.meals.push_back(meal);
            // KITCHEN, KITCHEN_II, FOOD_STORAGE, FOOD_STORAGE_2; PUB, PUB_2
            const bool kitchen = type == 10 || type == 265 || type == 73 || type == 74;
            const bool pub = type == 343 || type == 350 || type == 73 || type == 74;
            if (!kitchen && !pub) continue;
            const auto inv = ReadPtr(g, o + off::structure::kInventory);
            for (const uint64_t it : inv ? ReadRefList(g, *inv).value_or(std::vector<uint64_t>{})
                                         : std::vector<uint64_t>{}) {
                const auto item = ReadPod<int32_t>(g, it + off::item::kType);
                if (!item) continue;
                if (kitchen && IsMealItem(*item)) c.meals.push_back(*item);
                if (pub && IsDrinkItem(*item) && ReadPod<int32_t>(g, it + off::item::kQuantityReserved).value_or(1) <= 0) {
                    c.drinks.push_back(*item);
                }
            }
        }
    }
    // TimeManager.TotalElapsedGameTime; unreadable: nothing declared
    const auto day = ReadPod<int32_t>(g, dm + off::dm::kCurrentDayIndex);
    const auto time = ReadPod<float>(g, dm + off::dm::kCurrentGameTime);
    if (day && time) {
        const float now = static_cast<float>(*day) * 1200.f + *time;
        const auto since = [&](uint64_t field, float span) {
            const auto at = ReadPod<float>(g, dm + field);
            return at && now - *at < span;
        };
        c.fasting = since(off::dm::kLastFastDeclared, 3600.f);
        c.holiday = since(off::dm::kLastHolidayDeclared, 1200.f);
        c.nudism = since(off::dm::kLastNudismDeclared, 600.f);
    }
    if (const auto statics = StaticFields(g, main_base, off::kStructuresPubTypeInfo)) {
        c.pub_drinking = ReadPod<uint8_t>(g, *statics + off::kPubIsDrinking).value_or(0) != 0;
    }
}

// What FollowerCommandGroups.DefaultCommands reads (tools/cotl/README.md, Command list)
FollowerCommandState ReadCommandState(const Guest& g, uint64_t main_base, uint64_t dm, uint64_t info,
                                      const Follower& f) {
    FollowerCommandState c;
    c.follower_id = f.id;
    c.cursed_state = f.cursed_state;
    c.own_traits = f.traits;
    c.cult_traits = IdList(g, dm, off::dm::kCultTraits);
    c.is_snowman = ReadPod<uint8_t>(g, info + off::fi::kIsSnowman).value_or(0) != 0;
    c.drunk = ReadPod<float>(g, info + off::fi::kDrunk).value_or(0) > 0;
    c.received_blessing = ReadPod<uint8_t>(g, info + off::fi::kReceivedBlessing).value_or(0) != 0;
    c.necklace = ReadPod<int32_t>(g, info + off::fi::kNecklace).value_or(0);
    c.show_loyalty_bars = ReadPod<uint8_t>(g, dm + off::dm::kShowLoyaltyBars).value_or(0) != 0;
    c.in_tutorial = ReadPod<uint8_t>(g, dm + off::dm::kInTutorial).value_or(1) != 0;
    const auto doctrines = IdList(g, dm, off::dm::kDoctrineUnlockedUpgrades);
    c.inspire_unlocked = Contains(doctrines, off::dm::kDoctrineInspire);
    c.intimidate_unlocked = Contains(doctrines, off::dm::kDoctrineIntimidate);
    c.season = ReadPod<int32_t>(g, dm + off::dm::kCurrentSeason).value_or(0);
    c.weather = ReadPod<int32_t>(g, dm + off::dm::kCurrentWeatherEvent).value_or(0);
    c.phase = ReadPod<int32_t>(g, dm + off::dm::kCurrentPhase).value_or(0);
    ReadFollowerTask(g, main_base, f.id, c);
    const auto flag = [&](uint64_t base, uint64_t field) { return ReadPod<uint8_t>(g, base + field).value_or(0) != 0; };
    c.inspired = flag(info, off::fi::kInspired);
    c.intimidated = flag(info, off::fi::kIntimidated);
    c.bribed = flag(info, off::fi::kBribed);
    c.paid_tithes = flag(info, off::fi::kPaidTithes);
    c.kissed = flag(info, off::fi::kKissed);
    c.petted = flag(info, off::fi::kPetDog);
    c.scared_interacted = flag(info, off::fi::kScaredTraitInteracted);
    c.married = flag(info, off::fi::kMarriedToLeader);
    if (const auto skin = ReadPtr(g, info + off::fi::kSkinName)) {
        const std::string name = ReadString(g, *skin).value_or("");
        c.skin_dog = name.find("Dog") != std::string::npos || name.find("Poppy") != std::string::npos;
    }
    c.bribe_unlocked = Contains(doctrines, off::dm::kDoctrineBribe);
    c.extort_unlocked = Contains(doctrines, off::dm::kDoctrineExtort);
    c.shrine_built = flag(dm, off::dm::kHasBuiltShrine1);
    c.sozo_freed = flag(dm, off::dm::kSozoNoLongerBrainwashed);
    c.onboarding_phase = ReadPod<int32_t>(g, dm + off::dm::kOnboardingPhase).value_or(0);
    const auto day = ReadPod<int32_t>(g, dm + off::dm::kCurrentDayIndex);
    c.work_through_night = day && day == ReadPod<int32_t>(g, info + off::fi::kWorkThroughNightDay);
    // GetItemQuantity 0x1BBD440: the first stack counts. Unreadable means grey, never a wasted trip.
    // Gifts: every type whose first stack has at least 1
    if (const auto items = ReadPtr(g, dm + off::dm::kItems)) {
        std::vector<int32_t> seen;
        for (const uint64_t it : ReadRefList(g, *items).value_or(std::vector<uint64_t>{})) {
            const auto type = ReadPod<int32_t>(g, it + off::item::kType);
            if (!type || Contains(seen, *type)) continue;
            seen.push_back(*type);
            const int32_t quantity = ReadPod<int32_t>(g, it + off::item::kQuantity).value_or(0);
            if (*type == off::dm::kItemBlackGold) c.gold = quantity;
            if (quantity >= 1) c.items.push_back(*type);
        }
    }
    if (const auto statics = StaticFields(g, main_base, off::kDataManagerTypeInfo)) {
        c.all_necklaces = IdList(g, *statics, off::kDataManagerAllNecklaces);
    }
    c.showing_necklace = flag(info, off::fi::kShowingNecklace);
    c.onboarded_wool = flag(dm, off::dm::kOnboardedWool);
    if (const auto pets = ReadPtr(g, info + off::fi::kDlcPets)) {
        c.has_dlc_pet = ReadPod<int32_t>(g, *pets + kListSize).value_or(0) > 0;
    }
    ReadFoodAndWork(g, main_base, dm, c);
    return c;
}

// Seed and fertiliser silos: Structures_SiloSeed/SiloFertiliser.get_Capacity = 250 for
// SEED_BUCKET (334) or POOP_BUCKET (333), otherwise 15. Plus dirt and corpses (StructureBrain.TYPES).
void ReadStorage(const Guest& g, uint64_t dm, GameSnapshot& s) {
    const auto list = ReadPtr(g, dm + off::dm::kBaseStructures);
    if (!list) {
        return;
    }
    for (const uint64_t o : ReadRefList(g, *list, 4096).value_or(std::vector<uint64_t>{})) {
        const int32_t type = ReadPod<int32_t>(g, o + off::structure::kType).value_or(0);
        // Dirt: POOP and variants (GOLD, RAINBOW, MASSIVE, DEVOTION, PET, GLOW, ROTSTONE)
        if (type == 97 || (type >= 337 && type <= 342) || type == 419) {
            ++s.poop;
            continue;
        }
        if (type == 48) { // VOMIT
            ++s.vomit;
            continue;
        }
        if (type == 47) { // DEAD_WORSHIPPER
            ++s.corpses;
            continue;
        }
        int32_t* stored = nullptr;
        int32_t* capacity = nullptr;
        int32_t cap = 0;
        if (type == 169 || type == 334) { // SILO_SEED, SEED_BUCKET
            stored = &s.seed_stored;
            capacity = &s.seed_capacity;
            cap = type == 334 ? 250 : 15;
        } else if (type == 170 || type == 333) { // SILO_FERTILISER, POOP_BUCKET
            stored = &s.fertiliser_stored;
            capacity = &s.fertiliser_capacity;
            cap = type == 333 ? 250 : 15;
        } else {
            continue;
        }
        *capacity += cap;
        if (const auto inv = ReadPtr(g, o + off::structure::kInventory)) {
            for (const uint64_t it : ReadRefList(g, *inv).value_or(std::vector<uint64_t>{})) {
                *stored += ReadPod<int32_t>(g, it + off::item::kQuantity).value_or(0);
            }
        }
    }
}

void ReadRooms(const Guest& g, uint64_t main_base, Run& r) {
    const auto statics = StaticFields(g, main_base, off::kBiomeGeneratorTypeInfo);
    const auto biome = statics ? ReadPtr(g, *statics + off::biome::kInstance) : std::nullopt;
    const auto list = biome ? ReadPtr(g, *biome + off::biome::kRooms) : std::nullopt;
    if (!list) {
        return;
    }
    r.cur_x = ReadPod<int32_t>(g, *biome + off::biome::kCurrentX).value_or(0);
    r.cur_y = ReadPod<int32_t>(g, *biome + off::biome::kCurrentY).value_or(0);
    const auto flag = [&](uint64_t addr) { return ReadPod<uint8_t>(g, addr).value_or(0) != 0; };
    for (const uint64_t o : ReadRefList(g, *list).value_or(std::vector<uint64_t>{})) {
        Room room;
        room.x = ReadPod<int32_t>(g, o + off::room::kX).value_or(0);
        room.y = ReadPod<int32_t>(g, o + off::room::kY).value_or(0);
        room.discovered = flag(o + off::room::kDiscovered);
        room.visited = flag(o + off::room::kVisited);
        if (flag(o + off::room::kIsBoss) || flag(o + off::room::kIsDeathCatRoom) ||
            flag(o + off::room::kIsYngyaRoom)) {
            room.kind = RoomKind::Boss;
        } else if (flag(o + off::room::kIsMysticShopRoom)) {
            room.kind = RoomKind::Shop;
        } else if (flag(o + off::room::kIsRespawnRoom)) {
            room.kind = RoomKind::Start;
        }
        // Doors like MiniMapIcon.SetIcons: RoomConnection.get_Connected (True, Boss or from
        // NextLayer), target room not Hidden, no NextLayer door to the north. Exit, Entrance and
        // DoorRoom are not shown on the in-game map.
        for (size_t d = 0; d < 4; ++d) {
            const auto c = ReadPtr(g, o + off::room::kConnections + d * 8);
            if (!c) {
                continue;
            }
            const int32_t type = ReadPod<int32_t>(g, *c + off::conn::kType).value_or(0);
            const bool connected = type == off::conn::kTypeTrue || type == off::conn::kTypeBoss ||
                                   type >= off::conn::kTypeNextLayer;
            const auto other = ReadPtr(g, *c + off::conn::kRoom);
            const bool hidden = other && flag(*other + off::room::kHidden);
            room.doors[d] = connected && !hidden &&
                            !(d == static_cast<size_t>(kDoorN) && type == off::conn::kTypeNextLayer);
            // Question mark (MiniMapIcon.SetSelfToQuestionMark): connected door to a visited
            // room whose opposite door points back to this room
            if (connected && other && flag(*other + off::room::kVisited)) {
                const auto back = ReadPtr(g, *other + off::room::kConnections + ((d + 2) % 4) * 8);
                if (back && ReadPtr(g, *back + off::conn::kRoom) == o) {
                    room.question = true;
                }
            }
        }
        room.question = room.question && !room.visited && !room.discovered &&
                        !flag(o + off::room::kHidden) && !flag(o + off::room::kIsRespawnRoom);
        if (const auto p = ReadPtr(g, o + off::room::kGameObjectPath)) {
            room.path = ReadString(g, *p, 128).value_or("");
        }
        r.rooms.push_back(std::move(room));
    }
}

// Data only needed by some quest texts; only read while such a quest is open
void ReadQuestData(const Guest& g, uint64_t dm, GameSnapshot& s) {
    const auto wanted = [&](int32_t a, int32_t b = -1) {
        return std::any_of(s.objectives.begin(), s.objectives.end(),
                           [&](const Objective& q) { return q.type == a || q.type == b; });
    };
    // Lists of objects with kind @0x10 and quantity @0x14 (EnemyData, InventoryItem-like)
    const auto pairs = [&](uint64_t field) {
        std::vector<std::pair<int32_t, int32_t>> out;
        if (const auto list = ReadPtr(g, dm + field)) {
            for (const uint64_t o : ReadRefList(g, *list).value_or(std::vector<uint64_t>{})) {
                out.emplace_back(ReadPod<int32_t>(g, o + 0x10).value_or(0),
                                 ReadPod<int32_t>(g, o + 0x14).value_or(0));
            }
        }
        return out;
    };
    if (wanted(off::obj::kTypeKillEnemies)) {
        s.enemies_killed = pairs(off::dm::kEnemiesKilled);
    }
    if (wanted(off::obj::kTypeBlizzard)) {
        s.offerings = pairs(off::dm::kBlizzardOfferings);
    }
    if (wanted(off::obj::kTypeFeedAnimal, off::obj::kTypeWalkAnimal)) {
        for (const uint64_t field : off::dm::kAnimalLists) {
            const auto list = ReadPtr(g, dm + field);
            for (const uint64_t o :
                 (list ? ReadRefList(g, *list) : std::nullopt).value_or(std::vector<uint64_t>{})) {
                AnimalInfo a; // StructuresData.Animal: Type @0x10, ID @0x18, GivenName @0x60
                a.type = ReadPod<int32_t>(g, o + 0x10).value_or(0);
                a.id = ReadPod<int32_t>(g, o + 0x18).value_or(0);
                if (const auto name = ReadPtr(g, o + 0x60)) {
                    a.name = ReadString(g, *name).value_or("");
                }
                s.animals.push_back(std::move(a));
            }
        }
    }
    if (wanted(off::obj::kTypeLegendaryRun)) {
        s.legendary_known = IdList(g, dm, off::dm::kLegendaryKnown);
    }
    if (wanted(off::obj::kTypeSwordReturn)) {
        s.sword_return_day = ReadPod<int32_t>(g, dm + off::dm::kSwordReturnDay).value_or(0);
    }
}

// Entry of an Il2Cpp array with a matching type field (max_length @0x18, elements @0x20)
std::optional<uint64_t> FindData(const Guest& g, uint64_t statics, uint64_t field, int32_t type) {
    const auto array = ReadPtr(g, statics + field);
    const auto length = array ? ReadPod<uint64_t>(g, *array + 0x18) : std::nullopt;
    if (!length || *length > 1024) {
        return std::nullopt;
    }
    for (uint64_t i = 0; i < *length; ++i) {
        const auto entry = ReadPtr(g, *array + 0x20 + i * 8);
        if (entry && ReadPod<int32_t>(g, *entry + off::gear::kType) == type) {
            return entry;
        }
    }
    return std::nullopt;
}

GearData ReadGear(const Guest& g, uint64_t main_base, uint64_t dm, uint64_t player, const Run& r) {
    namespace gear = off::gear;
    GearData d;
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    const auto f32 = [&](uint64_t addr, float fallback) {
        return finite(ReadPod<float>(g, addr).value_or(fallback), fallback);
    };
    if (const auto statics = StaticFields(g, main_base, off::kEquipmentManagerTypeInfo)) {
        if (const auto w = FindData(g, *statics, gear::kWeapons, r.weapon)) {
            d.weapon_speed = f32(*w + gear::kWeaponSpeed, -1);
            const auto list = ReadPtr(g, *w + gear::kWeaponCombos);
            const auto combos = list ? ReadRefList(g, *list) : std::nullopt;
            if (combos && !combos->empty()) {
                float sum = 0;
                for (const uint64_t c : *combos) {
                    sum += f32(c + gear::kComboDamage, 0);
                }
                d.weapon_avg = sum / static_cast<float>(combos->size());
            }
        }
        if (const auto c = FindData(g, *statics, gear::kCurses, r.curse)) {
            d.curse_damage = f32(*c + gear::kCurseDamage, -1);
        }
        if (const auto rel = FindData(g, *statics, gear::kRelics, r.relic)) {
            d.relic_charge = f32(*rel + gear::kRelicCharge, -1);
        }
    }
    d.dm_bonus = f32(dm + gear::kDmDamageBonus, 0);
    d.fleece = ReadPod<int32_t>(g, dm + gear::kDmFleece).value_or(0);
    d.phase = ReadPod<int32_t>(g, dm + gear::kDmPhase).value_or(0);
    if (const auto relic = ReadPtr(g, player + gear::kPlayerRelic)) {
        d.relic_double = ReadPod<uint8_t>(g, *relic + gear::kRelicDouble).value_or(0) != 0;
        d.relic_mult = f32(*relic + gear::kRelicMultiplier, 0);
        d.relic_scale = f32(*relic + gear::kRelicScale, 1);
    }
    if (const auto statics = StaticFields(g, main_base, off::kDifficultyManagerTypeInfo)) {
        if (const auto data = ReadPtr(g, *statics + gear::kCurrentDifficulty)) {
            d.difficulty = f32(*data + gear::kDifficultyDamage, 1);
        }
    }
    if (const auto statics = StaticFields(g, main_base, off::kPlayerFleeceManagerTypeInfo)) {
        d.fleece_damage = f32(*statics + gear::kFleeceDamage, 0);
    }
    return d;
}

std::optional<Run> ReadRun(const Guest& g, uint64_t main_base, uint64_t dm, bool read_gear) {
    const auto statics = StaticFields(g, main_base, off::kPlayerFarmingTypeInfo);
    if (!statics) {
        return std::nullopt;
    }
    const auto player = ReadPtr(g, *statics + off::kPlayerFarmingInstance);
    const auto location = ReadPod<int32_t>(g, *statics + off::player::kLocation);
    if (!player || !location || !IsDungeonLocation(*location)) {
        return std::nullopt;
    }
    Run r;
    if (const auto h = ReadPtr(g, *player + off::player::kHealth)) {
        // Limit invalid read values: each half heart becomes one row
        const auto f = [&](uint64_t field) {
            const float v = ReadPod<float>(g, *h + field).value_or(0);
            return std::isfinite(v) ? std::clamp(v, 0.0f, kMaxHalfHearts) : 0.0f;
        };
        r.hp_red = f(off::health::kHp);
        r.hp_red_max = f(off::health::kTotalHp);
        r.hp_blue = f(off::health::kBlue);
        r.hp_black = f(off::health::kBlack);
        r.hp_fire = f(off::health::kFire);
        r.hp_ice = f(off::health::kIce);
        r.hp_spirit = f(off::health::kSpirit);
        r.hp_spirit_max = f(off::health::kSpiritTotal);
    }
    const auto i = [&](uint64_t field) { return ReadPod<int32_t>(g, *player + field).value_or(0); };
    r.weapon = i(off::player::kWeapon);
    r.curse = i(off::player::kCurse);
    r.weapon_lvl = i(off::player::kWeaponLevel);
    r.curse_lvl = i(off::player::kCurseLevel);
    r.relic = i(off::player::kRelic);
    if (const auto list = ReadPtr(g, *player + off::player::kRunTrinkets)) {
        for (const uint64_t c : ReadRefList(g, *list).value_or(std::vector<uint64_t>{})) {
            if (r.tarot.size() >= kMaxTarot) {
                break; // the manifest has this many templates
            }
            r.tarot.emplace_back(ReadPod<int32_t>(g, c + off::tarot::kCard).value_or(0),
                                 std::clamp(ReadPod<int32_t>(g, c + off::tarot::kUpgrade).value_or(0), 0, 9));
        }
    }
    ReadRooms(g, main_base, r);
    if (read_gear) {
        r.gear = ReadGear(g, main_base, dm, *player, r);
    }
    return r;
}

} // namespace

bool IsDungeonLocation(int32_t location) {
    switch (location) {
    case 7: case 8: case 9: case 10: // Dungeon1_1–1_4
    case 51: case 53:                // Boss_5, Dungeon1_5
    case 81: case 86:                // IntroDungeon, Dungeon1_6
    case 93: case 96:                // Boss_Yngya, Boss_Wolf
        return true;
    default:
        return false;
    }
}

std::optional<GameSnapshot> ReadSnapshot(const Guest& g, uint64_t main_base, int32_t thoughts_for,
                                         bool read_gear, SkinColourCache* colours) {
    const auto dm = DataManager(g, main_base);
    if (!dm) {
        return std::nullopt;
    }
    const auto list = ReadPtr(g, *dm + off::dm::kFollowers);
    const auto objs = list ? ReadRefList(g, *list) : std::nullopt;
    if (!objs) {
        return std::nullopt;
    }

    GameSnapshot s;
    s.followers.reserve(objs->size());
    for (const uint64_t obj : *objs) {
        auto f = ReadFollower(g, obj, thoughts_for);
        if (!f) {
            return std::nullopt;
        }
        f->tints = ReadTints(g, main_base, f->skin, f->skin_colour, colours);
        s.followers.push_back(std::move(*f));
    }

    // Torn-read protection: the list must be unchanged after reading
    const auto again = ReadRefList(g, *list);
    if (!again || *again != *objs) {
        return std::nullopt;
    }

    s.day = ReadPod<int32_t>(g, *dm + off::dm::kCurrentDayIndex).value_or(0);
    s.time = ReadPod<float>(g, *dm + off::dm::kCurrentGameTime).value_or(0);
    s.objectives = ReadObjectives(g, *dm);
    ReadQuestData(g, *dm, s);
    // At the title screen there is already a DataManager object with default values
    const auto save = StaticFields(g, main_base, off::kSaveAndLoadTypeInfo);
    s.save_loaded = save && ReadPod<uint8_t>(g, *save + off::kSaveAndLoadLoaded).value_or(0) != 0;
    // SaveAndLoad.Loaded stays set after quitting; the main menu object only exists at the title
    const auto menu = StaticFields(g, main_base, off::kMainMenuTypeInfo);
    s.at_title = menu && ReadPtr(g, *menu + off::kMainMenuInstance).has_value();
    if (const auto list = ReadPtr(g, *dm + off::dm::kItems)) {
        for (const uint64_t it : ReadRefList(g, *list).value_or(std::vector<uint64_t>{})) {
            s.items.emplace_back(ReadPod<int32_t>(g, it + off::item::kType).value_or(0),
                                 ReadPod<int32_t>(g, it + off::item::kQuantity).value_or(0));
        }
    }
    s.offered_quest_follower_ids = ReadOfferedQuests(g, main_base);
    s.run = ReadRun(g, main_base, *dm, read_gear);
    if (const auto name = ReadPtr(g, *dm + off::dm::kCultName)) {
        s.cult_name = ReadString(g, *name).value_or("");
    }
    s.cult_level = ReadPod<int32_t>(g, *dm + off::dm::kCultLevel).value_or(0);
    ReadStorage(g, *dm, s);

    const auto missionaries = IdList(g, *dm, off::dm::kOnMissionaryIds);
    const auto prisoners = IdList(g, *dm, off::dm::kImprisonedIds);
    for (auto& f : s.followers) {
        // Prisoner takes priority: a prisoner cannot be on a mission
        if (Contains(prisoners, f.id)) {
            f.group = FollowerGroup::Prisoner;
        } else if (Contains(missionaries, f.id)) {
            f.group = FollowerGroup::Missionary;
        }
    }
    for (size_t i = 0; thoughts_for >= 0 && i < s.followers.size(); ++i) {
        if (s.followers[i].id == thoughts_for) {
            s.command_state = ReadCommandState(g, main_base, *dm, (*objs)[i], s.followers[i]);
        }
    }
    return s;
}

} // namespace cotl
