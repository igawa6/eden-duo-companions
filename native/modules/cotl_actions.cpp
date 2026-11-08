// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_actions.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

#include "cotl_action_abi.h"

namespace cotl {

static_assert(static_cast<uint32_t>(ActionState::Idle) == COTL_ACT_STATE_IDLE);
static_assert(static_cast<uint32_t>(ActionState::Walking) == COTL_ACT_STATE_WALKING);
static_assert(static_cast<uint32_t>(ActionState::Running) == COTL_ACT_STATE_RUNNING);
static_assert(static_cast<uint32_t>(ActionState::Done) == COTL_ACT_STATE_DONE);
static_assert(static_cast<uint32_t>(ActionState::Refused) == COTL_ACT_STATE_REFUSED);
static_assert(static_cast<uint32_t>(ActionState::Cancelled) == COTL_ACT_STATE_CANCELLED);
static_assert(static_cast<uint32_t>(ActionReason::None) == COTL_ACT_REASON_NONE);
static_assert(static_cast<uint32_t>(ActionReason::NotAtBase) == COTL_ACT_REASON_NOT_AT_BASE);
static_assert(static_cast<uint32_t>(ActionReason::LambBusy) == COTL_ACT_REASON_LAMB_BUSY);
static_assert(static_cast<uint32_t>(ActionReason::FollowerMissing) == COTL_ACT_REASON_FOLLOWER_MISSING);
static_assert(static_cast<uint32_t>(ActionReason::FollowerLocked) == COTL_ACT_REASON_FOLLOWER_LOCKED);
static_assert(static_cast<uint32_t>(ActionReason::NotAvailable) == COTL_ACT_REASON_NOT_AVAILABLE);
static_assert(static_cast<uint32_t>(ActionReason::StaleSession) == COTL_ACT_REASON_STALE_SESSION);
static_assert(static_cast<uint32_t>(ActionReason::Timeout) == COTL_ACT_REASON_TIMEOUT);
static_assert(static_cast<uint32_t>(ActionReason::PlayerMoved) == COTL_ACT_REASON_PLAYER_MOVED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyBlessed) == COTL_ACT_REASON_ALREADY_BLESSED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyInspired) == COTL_ACT_REASON_ALREADY_INSPIRED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyIntimidated) == COTL_ACT_REASON_ALREADY_INTIMIDATED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyBribed) == COTL_ACT_REASON_ALREADY_BRIBED);
static_assert(static_cast<uint32_t>(ActionReason::NotEnoughGold) == COTL_ACT_REASON_NOT_ENOUGH_GOLD);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyKissed) == COTL_ACT_REASON_ALREADY_KISSED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyPetted) == COTL_ACT_REASON_ALREADY_PETTED);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyInteracted) == COTL_ACT_REASON_ALREADY_INTERACTED);
static_assert(static_cast<uint32_t>(ActionReason::SleepsUntilSeason) == COTL_ACT_REASON_SLEEPS_UNTIL_SEASON);
static_assert(static_cast<uint32_t>(ActionReason::TooCold) == COTL_ACT_REASON_TOO_COLD);
static_assert(static_cast<uint32_t>(ActionReason::AlreadyExtorted) == COTL_ACT_REASON_ALREADY_EXTORTED);
static_assert(static_cast<uint32_t>(ActionReason::NoBuildSites) == COTL_ACT_REASON_NO_BUILD_SITES);
static_assert(static_cast<uint32_t>(ActionReason::NoMeals) == COTL_ACT_REASON_NO_MEALS);
static_assert(static_cast<uint32_t>(ActionReason::NoDrinks) == COTL_ACT_REASON_NO_DRINKS);
static_assert(static_cast<uint32_t>(ActionReason::Fasting) == COTL_ACT_REASON_FASTING);
static_assert(static_cast<uint32_t>(ActionReason::CantWork) == COTL_ACT_REASON_CANT_WORK);
static_assert(static_cast<uint32_t>(ActionReason::NoGifts) == COTL_ACT_REASON_NO_GIFTS);
static_assert(static_cast<uint32_t>(ActionReason::HasNecklace) == COTL_ACT_REASON_HAS_NECKLACE);
static_assert(static_cast<uint32_t>(ActionReason::HasPet) == COTL_ACT_REASON_HAS_PET);

namespace {
uint32_t Read(MailboxIo& io, uint32_t off, bool* ok) {
    uint32_t v = 0;
    if (!io.Load(off, &v)) *ok = false;
    return v;
}
} // namespace

// The last sent sequence number is acknowledged (or is no longer in the mailbox, e.g. after a
// reset by the host): then REQ_* may be rewritten.
bool ActionClient::Acked() {
    bool ok = true;
    const uint32_t req = Read(*io_, COTL_ACT_REQ_SEQ, &ok);
    const uint32_t st = Read(*io_, COTL_ACT_ST_SEQ, &ok);
    const uint32_t busy = Read(*io_, COTL_ACT_ST_BUSY_SEQ, &ok);
    if (!ok) return false;
    if (!init_) { // first use: a sequence number found there counts as our last one
        init_ = true;
        last_seq_ = req;
    }
    return req != last_seq_ || st == last_seq_ || busy == last_seq_;
}

bool ActionClient::Submit(int32_t follower_id, uint32_t command, uint32_t parent) {
    if (!seen_ || stalled_ >= kHelperDeadPolls) return false; // helper not running: do not wait
    bool ok = true;
    const uint32_t epoch = Read(*io_, COTL_ACT_EPOCH, &ok);
    if (!ok || !Acked()) return false;
    const uint32_t st = Read(*io_, COTL_ACT_ST_SEQ, &ok);
    const uint32_t busy = Read(*io_, COTL_ACT_ST_BUSY_SEQ, &ok);
    if (!ok) return false;
    // New sequence number: never 0, and different from anything the helper regards as done
    uint32_t seq = last_seq_ + 1;
    while (seq == 0 || seq == st || seq == busy) ++seq;
    if (!io_->Store(COTL_ACT_REQ_FOLLOWER, static_cast<uint32_t>(follower_id)) ||
        !io_->Store(COTL_ACT_REQ_COMMAND, command) || !io_->Store(COTL_ACT_REQ_PARENT, parent) ||
        !io_->Store(COTL_ACT_REQ_EPOCH, epoch) ||
        !io_->Store(COTL_ACT_REQ_SEQ, seq)) { // written last: only then is the request valid
        return false;
    }
    last_seq_ = seq;
    last_follower_ = follower_id;
    sent_ = true;
    return true;
}

bool ActionClient::Cancel() {
    return sent_ && io_->Store(COTL_ACT_CANCEL_SEQ, last_seq_);
}

ActionStatus ActionClient::Poll(int32_t selected_follower) {
    ActionStatus out;
    uint32_t hb = 0;
    if (!io_->Load(COTL_ACT_HEARTBEAT, &hb)) { // no hook
        seen_ = have_hb_ = false;
        stalled_ = kHelperDeadPolls;
        return out;
    }
    if (!have_hb_) {
        have_hb_ = true; // the first reading is only the baseline
        stalled_ = 0;
    } else if (hb != last_hb_) {
        seen_ = true;
        stalled_ = 0;
    } else if (stalled_ < kHelperDeadPolls) {
        ++stalled_;
    }
    last_hb_ = hb;
    out.helper_alive = seen_ && stalled_ < kHelperDeadPolls;
    if (!sent_ || (selected_follower != kAnyFollower && selected_follower != last_follower_)) return out;

    // Status of our own sequence number: ST_SEQ first, then once more after the rest (the helper writes the
    // sequence number last); otherwise ST_BUSY_SEQ == sequence number means rejected, Lamb busy
    bool ok = true;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t s1 = Read(*io_, COTL_ACT_ST_SEQ, &ok);
        const uint32_t state = Read(*io_, COTL_ACT_ST_STATE, &ok);
        const uint32_t reason = Read(*io_, COTL_ACT_ST_REASON, &ok);
        const uint32_t busy = Read(*io_, COTL_ACT_ST_BUSY_SEQ, &ok);
        const uint32_t s2 = Read(*io_, COTL_ACT_ST_SEQ, &ok);
        if (!ok) return out;
        if (s1 != s2) continue;
        if (s1 == last_seq_) {
            out.state = static_cast<ActionState>(state);
            out.reason = static_cast<ActionReason>(reason);
        } else if (busy == last_seq_) {
            // Rejected because another action is running: show the rejection briefly, then the helper's status
            if (busy_seen_ != last_seq_) {
                busy_seen_ = last_seq_;
                busy_polls_ = 0;
            }
            if (busy_polls_ < kBusyShowPolls) {
                ++busy_polls_;
                out.state = ActionState::Refused;
                out.reason = ActionReason::LambBusy;
            } else {
                out.state = static_cast<ActionState>(state);
                out.reason = static_cast<ActionReason>(reason);
            }
        }
        break;
    }
    return out;
}

namespace {

constexpr uint32_t kBless = COTL_ACT_COMMAND_BLESS, kDance = COTL_ACT_COMMAND_DANCE,
                   kIntimidate = COTL_ACT_COMMAND_INTIMIDATE, kBribe = COTL_ACT_COMMAND_BRIBE,
                   kExtort = COTL_ACT_COMMAND_EXTORT,
                   kReassure = COTL_ACT_COMMAND_REASSURE, kBully = COTL_ACT_COMMAND_BULLY,
                   kRomance = COTL_ACT_COMMAND_ROMANCE, kPetDog = COTL_ACT_COMMAND_PET_DOG,
                   kWakeUp = COTL_ACT_COMMAND_WAKE_UP, kSleep = COTL_ACT_COMMAND_SLEEP,
                   kWorship = COTL_ACT_COMMAND_WORSHIP;
// Not released yet, only in the list so they stay in their place
constexpr uint32_t kMakeDemand = 5, kMurder = 7, kSurveillance = 46, kReeducate = 63, kBaby = 97;


// Submenus (tools/cotl/README.md, Food and work)
constexpr uint32_t kWork = COTL_ACT_COMMAND_WORK, kEat = COTL_ACT_COMMAND_EAT, kDrink = COTL_ACT_COMMAND_DRINK,
                   kBuild = COTL_ACT_COMMAND_BUILD;
// In the spy wheel before Eat, not supported
constexpr uint32_t kSpyCommand = 29;

// Meal or drink: command, ITEM_TYPE, building type of a meal lying around (Structures_Meal), name
struct FoodEntry {
    uint32_t command;
    int32_t item;
    int32_t structure;
    const char* name;
};
// MealCommands 0x20356E0: fixed order by ITEM_TYPE, one entry per kind
constexpr FoodEntry kMeals[] = {
    {31, 0x20, 52, "Meal"},          {32, 0x39, 131, "Grass Meal"},   {33, 0x45, 145, "Poop Meal"},
    {37, 0x50, 143, "Mushrooms"},    {35, 0x41, 133, "Cultist Meat"}, {34, 0x3C, 132, "Fish Meal"},
    {36, 0x3B, 130, "Great Meal"},   {133, 0xC6, 430, "Bad Milk"},    {134, 0xC7, 431, "Good Milk"},
    {135, 0xC8, 432, "Great Milk"},  {38, 0x3A, 129, "Meat Meal"},    {60, 0x63, 246, "Great Fish"},
    {87, 0xA1, 329, "Egg Meal"},     {61, 0x64, 247, "Bad Fish"},     {65, 0x6A, 254, "Berries"},
    {66, 0x6B, 255, "Veg Meal"},     {71, 0x70, 260, "Bad Meat"},     {72, 0x71, 261, "Great Meat"},
    {70, 0x6F, 259, "Deadly Meal"},  {67, 0x6C, 256, "Bad Mixed"},    {68, 0x6D, 257, "Mixed Meal"},
    {69, 0x6E, 258, "Great Mixed"},  {105, 0x86, 388, "Spicy Meal"},  {112, 0xAB, 395, "Snow Fruit"},
    {84, 0x73, 267, "Burned Meal"},
};
// DrinkCommands: fixed order
constexpr FoodEntry kDrinks[] = {
    {90, 0x91, -1, "Beer"},          {92, 0x93, -1, "Cocktail"},     {96, 0x94, -1, "Eggnog"},
    {91, 0x9C, -1, "Gin"},           {94, 0x9E, -1, "Shroom Juice"}, {95, 0x95, -1, "Poop Juice"},
    {93, 0x92, -1, "Wine"},          {127, 0xBE, -1, "Chilli Drink"}, {128, 0xBF, -1, "Lightning"},
    {129, 0xC0, -1, "Sin Drink"},    {130, 0xC1, -1, "Grass Drink"}, {177, 0xEC, -1, "Milkshake"},
};

// Roles in the order of GiveWorkerCommands 0x203CDC0 after Worship, wood, rubble and build: command,
// name and the buildings (StructureBrain.TYPES) of which one must exist
struct RoleEntry {
    uint32_t command;
    const char* name;
    int32_t gate[3];
};
constexpr RoleEntry kRoles[] = {
    {175, "Rotstone", {498, 499, -1}},  {47, "Farm", {8, 266, -1}},          {30, "Cook", {265, -1, -1}},
    {58, "Janitor", {216, 335, -1}},     {113, "Medic", {396, -1, -1}},       {117, "Logistics", {407, -1, -1}},
    {124, "Handyman", {416, -1, -1}},    {126, "Traits", {420, 421, 422}},    {116, "Rancher", {397, -1, -1}},
    {64, "Refine", {148, 176, -1}},      {81, "Undertaker", {304, 305, -1}},  {88, "Brew", {343, 350, -1}},
};
constexpr uint32_t kChop = 14, kRubble = 17, kFarm = 47;
constexpr int32_t kFarmStations[] = {8, 266};
constexpr int32_t kBuildSites[] = {31, 118};
constexpr int32_t kPubs[] = {343, 350};

// Gifts (tools/cotl/README.md, Gifts)
constexpr uint32_t kGift = COTL_ACT_COMMAND_GIFT, kGivePet = COTL_ACT_COMMAND_GIVE_PET,
                   kRemoveNecklace = COTL_ACT_COMMAND_REMOVE_NECKLACE, kWinterNecklace = COTL_ACT_COMMAND_WINTER_NECKLACE,
                   kWeirdNecklace = COTL_ACT_COMMAND_WEIRD_NECKLACE;
// Gift or pet: command, ITEM_TYPE, name (necklaces without "Necklace", as in the inventory; too long otherwise)
struct ItemEntry {
    uint32_t command;
    int32_t item;
    const char* name;
};
// GiftCommands 0x2037CA0: fixed order, one entry per item held
constexpr ItemEntry kGifts[] = {
    {51, 0x2B, "Small Gift"}, {52, 0x2C, "Big Gift"},    {53, 0x2D, "Flower"},     {54, 0x2E, "Feather"},
    {55, 0x2F, "Skull"},      {56, 0x30, "Natures"},     {57, 0x31, "Moon"},       {77, 0x7C, "Dark"},
    {82, 0x7F, "Gold Skull"}, {79, 0x7B, "Demonic"},     {76, 0x7D, "Light"},      {80, 0x7A, "Loyalty"},
    {78, 0x7E, "Missionary"}, {119, 0xB4, "Regrowth"},   {120, 0xB5, "Winter"},    {121, 0xB6, "Warming"},
    {122, 0xB7, "Abnormal"},  {123, 0xB8, "Unlucky"},
};
// HasGift 0x1BBE710 also counts the bell, which GiftCommands does not list
constexpr int32_t kItemBell = 0xA3;
// GiveAnimalPetCommands 0x20395B0; HasAnimal checks the same ones
constexpr ItemEntry kPets[] = {
    {141, 0xAA, "Yakish"},   {142, 0xBC, "Cowish"},    {143, 0xB2, "Spiderish"}, {144, 0xBD, "Llamaish"},
    {146, 0xB3, "Snailish"}, {147, 0xB0, "Turtleish"}, {145, 0xB1, "Crabish"},
};
constexpr int32_t kItemWinterNecklace = 0xB5, kItemWeirdNecklace = 0xB7;

// Commands the module offers. Anything missing here is Hidden; Task 7 adds them group by group.
struct SupportEntry {
    uint32_t command;
    const char* name;
    ActionSupport support;
    ActionReason locked_reason; // message when CommandItem.IsAvailable is false
};
constexpr SupportEntry kSupport[] = {
    {kBless, "Bless", ActionSupport::Supported, ActionReason::AlreadyBlessed},
    {kDance, "Dance", ActionSupport::Supported, ActionReason::AlreadyInspired},
    {kIntimidate, "Intimidate", ActionSupport::Supported, ActionReason::AlreadyIntimidated},
    {kExtort, "Extort", ActionSupport::Supported, ActionReason::AlreadyExtorted},
    {kBribe, "Bribe", ActionSupport::Supported, ActionReason::AlreadyBribed},
    {kReassure, "Reassure", ActionSupport::Supported, ActionReason::AlreadyInteracted},
    {kBully, "Bully", ActionSupport::Supported, ActionReason::AlreadyInteracted},
    {kRomance, "Romance", ActionSupport::Supported, ActionReason::AlreadyKissed},
    {kPetDog, "Pet Dog", ActionSupport::Supported, ActionReason::AlreadyPetted},
    {kWakeUp, "Wake Up", ActionSupport::Supported, ActionReason::NotAvailable},
    {kSleep, "Sleep", ActionSupport::Supported, ActionReason::NotAvailable},
    {kWorship, "Worship", ActionSupport::Supported, ActionReason::NotAvailable},
    // Food and work: the parents open their submenu; meals, drinks and roles from their tables
    {kWork, "Work", ActionSupport::Supported, ActionReason::CantWork},
    {kEat, "Eat", ActionSupport::Supported, ActionReason::NoMeals},
    {kDrink, "Drink", ActionSupport::Supported, ActionReason::NoDrinks},
    {kChop, "Chop Trees", ActionSupport::Supported, ActionReason::TooCold},
    {kRubble, "Clear Rubble", ActionSupport::Supported, ActionReason::TooCold},
    {kBuild, "Build", ActionSupport::Supported, ActionReason::NoBuildSites},
    // Gifts: Gift and Give Pet open their submenu; gifts and pets from their tables
    {kGift, "Gift", ActionSupport::Supported, ActionReason::NoGifts},
    {kGivePet, "Give Pet", ActionSupport::Supported, ActionReason::HasPet},
    {kRemoveNecklace, "Remove Charm", ActionSupport::Supported, ActionReason::NotAvailable},
};

// Entry of the command; Hidden without an entry
SupportEntry FindSupport(uint32_t command) {
    for (const SupportEntry& e : kSupport) {
        if (e.command == command) return e;
    }
    for (const FoodEntry& e : kMeals) {
        if (e.command == command) return {command, e.name, ActionSupport::Supported, ActionReason::NotAvailable};
    }
    for (const FoodEntry& e : kDrinks) {
        if (e.command == command) return {command, e.name, ActionSupport::Supported, ActionReason::NotAvailable};
    }
    for (const RoleEntry& e : kRoles) {
        if (e.command == command) return {command, e.name, ActionSupport::Supported, ActionReason::TooCold};
    }
    for (const ItemEntry& e : kGifts) {
        if (e.command == command) return {command, e.name, ActionSupport::Supported, ActionReason::HasNecklace};
    }
    for (const ItemEntry& e : kPets) {
        if (e.command == command) return {command, e.name, ActionSupport::Supported, ActionReason::NotAvailable};
    }
    return {command, "", ActionSupport::Hidden, ActionReason::NotAvailable};
}

// FollowerInfo.CursedState
constexpr int32_t kCursedChild = 0x188, kCursedDissenter = 0x42, kCursedMakeDemand = 0x44,
                  kCursedOldAge = 0x92;
// TraitType
constexpr int32_t kTraitHibernation = 0x5E, kTraitAestivation = 0x5F, kTraitZombie = 0x48,
                  kTraitDread = 0x4E, kTraitMissionaryTerrified = 0x43, kTraitScared = 0x25,
                  kTraitTerrified = 0x28, kTraitCriminalScarred = 0x3D, kTraitPoet = 0x3C,
                  kTraitFluteLover = 0x77, kTraitSpy = 0x50, kTraitMutated = COTL_ACT_TRAIT_MUTATED,
                  kTraitInsomniac = 0x3B, kTraitBullied = 0x24, kTraitBullied2 = 0x3F,
                  kTraitNoWorkCommands = 0x67, kTraitWorkColdCult = COTL_ACT_TRAIT_WORK_COLD_CULT,
                  kTraitWorkCold = COTL_ACT_TRAIT_WORK_COLD;
constexpr int32_t kFollowerSozo = 99996;
constexpr int32_t kBribeCost = COTL_ACT_BRIBE_COST;
constexpr int32_t kSeasonSpring = 0, kSeasonWinter = 1, kWeatherBlizzard = 1, kPhaseNight = 4;
constexpr int32_t kNecklaceAwake = 0x31; // ITEM_TYPE, compared by get_isAwake

bool Contains(const std::vector<int32_t>& v, int32_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// FollowerBrain.HasTrait 0x2999000: own or cult trait
bool HasTrait(const FollowerCommandState& s, int32_t trait) {
    return Contains(s.own_traits, trait) || Contains(s.cult_traits, trait);
}

// FollowerTask_Sleep.get_isAwake 0x2A3A6C0
bool SleepIsAwake(const FollowerCommandState& s) {
    if (s.weather != kWeatherBlizzard || HasTrait(s, kTraitHibernation)) return false;
    if (HasTrait(s, kTraitInsomniac) == (s.phase == kPhaseNight)) return true;
    return s.necklace == kNecklaceAwake;
}

// FollowerBrain.CanWork 0x2998ED0
bool CanWork(const FollowerCommandState& s) {
    return s.weather != kWeatherBlizzard || Contains(s.cult_traits, kTraitWorkColdCult) ||
           HasTrait(s, kTraitWorkCold) || HasTrait(s, kTraitMutated) || s.is_snowman;
}

// What the WakeUp branch of the handler rejects (0x2C5CF2C); action_tick.c checks the same
ActionReason WakeUpRefusal(const FollowerCommandState& s) {
    if (!s.work_through_night && ((s.season == kSeasonWinter && HasTrait(s, kTraitHibernation)) ||
                                  (s.season == kSeasonSpring && HasTrait(s, kTraitAestivation)))) {
        return ActionReason::SleepsUntilSeason;
    }
    return CanWork(s) ? ActionReason::None : ActionReason::TooCold;
}

template <size_t N>
bool ContainsAny(const std::vector<int32_t>& v, const int32_t (&xs)[N]) {
    return std::any_of(std::begin(xs), std::end(xs), [&](int32_t x) { return x >= 0 && Contains(v, x); });
}

// GiveWorkerCommands 0x203CDC0, by Onboarding.CurrentPhase and the buildings. The wheel locks the roles via
// FollowerBrain.IsTaskAvailable, which the module does not replicate (Ruling): they are free except for what
// the handler rejects. Roles in a blizzard without CanWork (0x2C5C848 "TooColdToWork"), Build without a site
// (0x2C5D398 "NoBuildingSites"); Build ignores the cold.
std::vector<OfferedCommand> WorkRoles(const FollowerCommandState& s) {
    std::vector<OfferedCommand> out;
    const bool can_work = CanWork(s);
    const auto role = [&](uint32_t command) { out.push_back({command, can_work, ActionReason::TooCold}); };
    if (s.onboarding_phase == 3) {
        role(kWorship);
        return out;
    }
    if (s.onboarding_phase == 1 || s.onboarding_phase == 4) {
        role(kChop);
        role(kRubble);
        if (ContainsAny(s.structures, kFarmStations)) role(kFarm);
        return out;
    }
    // Worship: DM.HasBuiltShrine1 and not mutated itself
    if (s.shrine_built && !Contains(s.own_traits, kTraitMutated)) role(kWorship);
    role(kChop);
    role(kRubble);
    out.push_back({kBuild, ContainsAny(s.structures, kBuildSites), ActionReason::NoBuildSites});
    for (const RoleEntry& r : kRoles) {
        if (ContainsAny(s.structures, r.gate)) role(r.command);
    }
    return out;
}

// Builds the flat list in the order of the wheel. The conditions are in the group functions of
// FollowerCommandGroups, the locks in CommandItem.IsAvailable (tools/cotl/README.md, Simple commands).
struct Offers {
    const FollowerCommandState& s;
    std::vector<OfferedCommand> out;

    void Add(uint32_t command, bool available = true, ActionReason reason = ActionReason::None) {
        out.push_back({command, available, reason});
    }
    void Dance() {
        if (s.inspire_unlocked) Add(kDance, !s.inspired);
    }
    void Intimidate() {
        if (s.intimidate_unlocked) Add(kIntimidate, !s.intimidated);
    }
    // BlessCommandItem.IsAvailable 0x203EF00. The module does not read CheatConsole.ForceBlessEnabled
    // (Ruling command list), nor the other cheat switches.
    void Bless() {
        if (s.show_loyalty_bars) Add(kBless, !s.received_blessing);
    }
    // Drunk and OldAge (0x20427B8: doctrine 7 only asks OldAge if 6 is missing; both branches end in the same
    // append 0x204291C). In Make Demand, Dance and Intimidate sit next to each other instead.
    void DanceOrIntimidateOrBless() {
        if (s.inspire_unlocked) {
            Dance();
        } else if (s.intimidate_unlocked) {
            Intimidate();
        } else {
            Bless();
        }
    }
    void Romance() {
        if (s.married) Add(kRomance, !s.kissed);
    }
    void PetDog() {
        if (s.skin_dog) Add(kPetDog, !s.petted);
    }
    void BullyReassure() {
        Add(kBully, !s.scared_interacted);
        Add(kReassure, !s.scared_interacted);
    }
    // GiveWorkerCommand 0x203CC90: with Cursed or trait 0x67 no submenu, and [1] alone was rejected by the
    // handler. GiverWorkerCommandItem.IsAvailable 0x203FCC0: the snowman always, otherwise ShouldWork
    // (no holiday, no nudism; the module does not read winter warmth), not 0x67, then CanWork.
    void Work() {
        OfferedCommand c{kWork, true};
        const bool no_menu = s.cursed_state != 0 || HasTrait(s, kTraitNoWorkCommands);
        if (!no_menu) c.sub = WorkRoles(s);
        if (!s.is_snowman && (s.holiday || s.nudism || HasTrait(s, kTraitNoWorkCommands))) {
            c.available = false;
            c.reason = ActionReason::CantWork;
        } else if (!s.is_snowman && !CanWork(s)) {
            c.available = false;
            c.reason = ActionReason::TooCold;
        } else if (no_menu) {
            c.available = false;
            c.reason = ActionReason::CantWork;
        }
        out.push_back(std::move(c));
    }
    // EatSomething 0x2035100: while fasting no submenu (the handler would say "CantEatFasting"), otherwise one
    // entry per kind held (MealCommands 0x20356E0); without an entry it would say "NoMeals"
    void Eat() {
        OfferedCommand c{kEat, true};
        if (s.fasting) {
            c.available = false;
            c.reason = ActionReason::Fasting;
        } else {
            for (const FoodEntry& m : kMeals) {
                if (Contains(s.meals, m.item)) c.sub.push_back({m.command, true});
            }
            if (c.sub.empty()) {
                c.available = false;
                c.reason = ActionReason::NoMeals;
            }
        }
        out.push_back(std::move(c));
    }
    // Only with a pub and while nobody is drinking there (0x203C3B4); without a free drink "NoDrinks"
    void Drink() {
        if (!ContainsAny(s.structures, kPubs) || s.pub_drinking) return;
        OfferedCommand c{kDrink, true};
        for (const FoodEntry& d : kDrinks) {
            if (Contains(s.drinks, d.item)) c.sub.push_back({d.command, true});
        }
        if (c.sub.empty()) {
            c.available = false;
            c.reason = ActionReason::NoDrinks;
        }
        out.push_back(std::move(c));
    }
    void WakeUp() {
        const ActionReason refusal = WakeUpRefusal(s);
        Add(kWakeUp, refusal == ActionReason::None, refusal);
    }
    bool Has(int32_t item) const { return Contains(s.items, item); }
    // GiveItemRoutine 0x2C7E640 says "AlreadyHaveNecklace" with a necklace if the gift is one
    OfferedCommand GiftItem(uint32_t command, int32_t item) const {
        const bool worn = s.necklace != 0 && Contains(s.all_necklaces, item);
        return {command, !worn, worn ? ActionReason::HasNecklace : ActionReason::None};
    }
    // HasGift 0x1BBE710, then GiftCommands; with only the bell the submenu stays empty ("NoGifts")
    void Gift() {
        const bool any = Has(kItemBell) ||
                         std::any_of(std::begin(kGifts), std::end(kGifts), [&](const ItemEntry& g) { return Has(g.item); });
        if (!any) return;
        OfferedCommand c{kGift, true};
        for (const ItemEntry& g : kGifts) {
            if (Has(g.item)) c.sub.push_back(GiftItem(g.command, g.item));
        }
        if (c.sub.empty()) {
            c.available = false;
            c.reason = ActionReason::NoGifts;
        }
        out.push_back(std::move(c));
    }
    // HasAnimal and DM.OnboardedWool; GivePetRoutine 0x2C7FAA0 says "AlreadyHavePet" with a pet
    void GivePet() {
        if (!s.onboarded_wool) return;
        OfferedCommand c{kGivePet, true};
        for (const ItemEntry& p : kPets) {
            if (Has(p.item)) c.sub.push_back({p.command, true});
        }
        if (c.sub.empty()) return;
        if (s.has_dlc_pet) {
            c.available = false;
            c.reason = ActionReason::HasPet;
        }
        out.push_back(std::move(c));
    }
    // With necklace: Remove (Make Demand only). The wheel then offers Hide 98 and Show 99, the
    // module does not (Ruling Show/Hide, fix round 1)
    void RemoveNecklace() {
        if (s.necklace != 0) Add(kRemoveNecklace);
    }
    // Snowman and mutated followers offer their necklace at the top, without a worn one
    void TopNecklace(uint32_t command, int32_t item) {
        if (s.necklace == 0 && Has(item)) Add(command);
    }
    // MakeDemandCommands 0x203B6C0
    void MakeDemand() {
        // Cursed 0x44 gets bed rest (command 6) at this point instead of Sleep
        if (!s.in_sleep_task && s.cursed_state != kCursedMakeDemand) Add(kSleep);
        // In between the wheel has Murder (command 7, not supported)
        if (s.extort_unlocked) Add(kExtort, !s.paid_tithes);
        if (s.bribe_unlocked) {
            if (s.bribed) {
                Add(kBribe, false);
            } else {
                Add(kBribe, s.gold >= kBribeCost, ActionReason::NotEnoughGold);
            }
        }
        Dance();
        Intimidate();
        if (!s.inspire_unlocked && !s.intimidate_unlocked) Bless();
        Eat(); // 0x203C2CC
        Drink();
        Gift();
        GivePet();
        // Prison 12/29 (not supported)
        Romance();
        PetDog();
        // 104 (not supported)
        RemoveNecklace();
    }
};

} // namespace

CommandGroup SelectCommandGroup(const FollowerCommandState& s) {
    if (s.is_snowman) {
        return s.cursed_state == kCursedChild ? CommandGroup::SnowmanChild : CommandGroup::Snowman;
    }
    // The game still asks WorkThroughNight here; see Ruling command list for why it changes nothing
    if (s.task == FollowerTaskKind::Sleep &&
        ((HasTrait(s, kTraitHibernation) && s.season == kSeasonWinter) ||
         (HasTrait(s, kTraitAestivation) && s.season == kSeasonSpring))) {
        return CommandGroup::WakeUp;
    }
    if (s.cursed_state == kCursedChild) return CommandGroup::Baby;
    if (HasTrait(s, kTraitZombie)) return CommandGroup::Zombie;
    if (s.task == FollowerTaskKind::BedRest) return CommandGroup::WakeUp;
    if (s.task == FollowerTaskKind::Sleep) {
        return SleepIsAwake(s) ? CommandGroup::Normal : CommandGroup::WakeUp;
    }
    if (s.cursed_state == kCursedDissenter) return CommandGroup::Dissenter;
    if (s.cursed_state == kCursedMakeDemand) return CommandGroup::MakeDemand;
    if (s.drunk) return CommandGroup::Drunk;
    if (HasTrait(s, kTraitDread) || HasTrait(s, kTraitMissionaryTerrified)) {
        return CommandGroup::ExistentialDread;
    }
    if (s.cursed_state == kCursedOldAge) return CommandGroup::OldAge;
    if (HasTrait(s, kTraitScared) || HasTrait(s, kTraitTerrified) ||
        Contains(s.own_traits, kTraitCriminalScarred)) {
        return CommandGroup::Scared;
    }
    if (HasTrait(s, kTraitPoet) || HasTrait(s, kTraitFluteLover)) return CommandGroup::Poet;
    if (HasTrait(s, kTraitSpy)) return CommandGroup::Spy;
    if (HasTrait(s, kTraitMutated)) return CommandGroup::Mutated;
    return CommandGroup::Normal;
}

std::vector<OfferedCommand> OfferedCommands(const FollowerCommandState& s) {
    Offers o{s, {}};
    // Both skip Make Demand with its submenu without DM.InTutorial (0x2041248, 0x20436B0)
    const auto demands_if_tutorial = [&] {
        if (s.in_tutorial) {
            o.Add(kMakeDemand);
            o.MakeDemand();
        }
    };
    switch (SelectCommandGroup(s)) {
    case CommandGroup::Normal:
        // Sozo only works after his liberation (NormalCommands 0x2041248)
        if (s.follower_id != kFollowerSozo || s.sozo_freed) o.Work(); // GiveWorkerCommand in place of Worship
        demands_if_tutorial();
        o.Add(kSurveillance);
        if (HasTrait(s, kTraitBullied) || HasTrait(s, kTraitBullied2)) o.BullyReassure();
        break;
    case CommandGroup::Poet:
        demands_if_tutorial();
        o.Add(kSurveillance);
        break;
    case CommandGroup::MakeDemand: o.MakeDemand(); break;
    case CommandGroup::OldAge:
        o.Add(kSleep);
        o.Add(kMurder);
        o.DanceOrIntimidateOrBless();
        o.Add(kSurveillance);
        if (Contains(s.own_traits, kTraitScared) || Contains(s.own_traits, kTraitTerrified) ||
            HasTrait(s, kTraitCriminalScarred) || HasTrait(s, kTraitBullied) ||
            Contains(s.own_traits, kTraitMissionaryTerrified)) {
            o.BullyReassure();
        }
        o.Eat(); // 0x2042C1C
        o.Drink();
        o.Romance();
        o.PetDog(); // then Show/Hide (not supported)
        break;
    case CommandGroup::WakeUp:
        o.WakeUp();
        o.Add(kSurveillance);
        break;
    case CommandGroup::Baby:
        o.Add(kBaby);
        o.Add(kSurveillance);
        break;
    case CommandGroup::SnowmanChild: o.Add(kBaby); break;
    case CommandGroup::Snowman:
        o.Work();
        o.TopNecklace(kWinterNecklace, kItemWinterNecklace);
        break;
    case CommandGroup::Zombie:
        o.Add(kSurveillance);
        o.Romance();
        break;
    case CommandGroup::Dissenter:
        o.Add(kReeducate);
        o.Add(kMurder);
        o.Add(kSurveillance);
        o.Eat(); // 0x20419F4
        break;
    case CommandGroup::Drunk:
        o.Add(kSleep);
        o.Add(kMurder);
        if (HasTrait(s, kTraitScared) || HasTrait(s, kTraitTerrified) || HasTrait(s, kTraitCriminalScarred) ||
            HasTrait(s, kTraitBullied) || HasTrait(s, kTraitMissionaryTerrified)) {
            o.BullyReassure();
        } else {
            o.DanceOrIntimidateOrBless(); // only without the traits (0x2041D70 after 0x20422A4)
        }
        o.Add(kSurveillance);
        o.Eat(); // 0x2041F70
        o.Drink();
        o.Romance();
        o.PetDog();
        break;
    case CommandGroup::ExistentialDread:
        o.Add(kMurder);
        o.Add(kSurveillance);
        break;
    case CommandGroup::Scared:
        o.Add(kSleep);
        o.Add(kMurder);
        o.BullyReassure();
        o.Add(kSurveillance);
        break;
    case CommandGroup::Spy:
        o.Add(kSleep);
        o.Add(kMurder);
        o.Add(kSurveillance);
        o.Add(kSpyCommand);
        o.Eat(); // 0x2043D48
        break;
    case CommandGroup::Mutated:
        o.Add(kSurveillance);
        o.Romance();
        o.Add(kMurder);
        o.TopNecklace(kWeirdNecklace, kItemWeirdNecklace);
        o.Work();
        break;
    }
    // The helper rejects while TimedAnimation is running (check_hold). Its daily reason comes first.
    // This also applies in the submenus.
    if (s.timed_action) {
        const auto lock = [](OfferedCommand& c) {
            if (c.available) {
                c.available = false;
                c.reason = ActionReason::FollowerLocked;
            }
        };
        for (OfferedCommand& c : o.out) {
            lock(c);
            for (OfferedCommand& sub : c.sub) lock(sub);
        }
    }
    return std::move(o.out);
}

ActionSupport SupportOf(uint32_t command) {
    return FindSupport(command).support;
}

std::vector<ActionRow> VisibleActions(const std::vector<OfferedCommand>& offered) {
    std::vector<ActionRow> rows;
    for (const OfferedCommand& c : offered) {
        const SupportEntry e = FindSupport(c.command);
        if (e.support == ActionSupport::Hidden) continue;
        const ActionReason locked = c.reason != ActionReason::None ? c.reason : e.locked_reason;
        // A locked parent opens nothing, as in the wheel
        rows.push_back({e.name, e.command, c.available ? ActionReason::None : locked, 0,
                        c.available && !c.sub.empty()});
    }
    // Bless in the first slot: its button keeps its position, whatever else is offered
    std::stable_partition(rows.begin(), rows.end(), [](const ActionRow& r) { return r.command == kBless; });
    return rows;
}

std::vector<ActionRow> MenuActions(const std::vector<OfferedCommand>& offered, const ActionMenu& menu) {
    const auto parent = std::find_if(offered.begin(), offered.end(), [&](const OfferedCommand& c) {
        return menu.parent != 0 && c.command == menu.parent && !c.sub.empty();
    });
    const size_t slots = static_cast<size_t>(kActionSlots);
    if (parent == offered.end()) {
        // Top level: if it does not fit, slots - 1 entries per page and More last, without Back. Page 0
        // starts with Bless, as without paging (Ruling Paging at the top).
        std::vector<ActionRow> top = VisibleActions(offered);
        if (top.size() <= slots) return top;
        const size_t per = slots - 1;
        const size_t pages = (top.size() + per - 1) / per;
        const size_t page = static_cast<size_t>(menu.parent == 0 && menu.page > 0 ? menu.page : 0) % pages;
        const size_t first = page * per;
        const size_t last = std::min(top.size(), first + per);
        std::vector<ActionRow> rows(top.begin() + static_cast<std::ptrdiff_t>(first),
                                    top.begin() + static_cast<std::ptrdiff_t>(last));
        rows.push_back({"More", kMenuMore, ActionReason::None});
        return rows;
    }
    std::vector<ActionRow> items = VisibleActions(parent->sub);
    for (ActionRow& r : items) {
        // Worship stays with the one-entry array that Task 7 verified in the game (Ruling Worship)
        r.parent = r.command == kWorship ? 0 : menu.parent;
    }
    std::vector<ActionRow> rows = {{"Back", kMenuBack, ActionReason::None}};
    if (items.size() + 1 <= slots) {
        rows.insert(rows.end(), items.begin(), items.end());
        return rows;
    }
    // Back first, More last, slots - 2 entries per page in between; after the last page the first
    const size_t per = slots - 2;
    const size_t pages = (items.size() + per - 1) / per;
    const size_t page = static_cast<size_t>(menu.page < 0 ? 0 : menu.page) % pages;
    const size_t first = page * per;
    const size_t last = std::min(items.size(), first + per);
    rows.insert(rows.end(), items.begin() + static_cast<std::ptrdiff_t>(first),
                items.begin() + static_cast<std::ptrdiff_t>(last));
    rows.push_back({"More", kMenuMore, ActionReason::None});
    return rows;
}

bool IsMealItem(int32_t item_type) {
    return std::any_of(std::begin(kMeals), std::end(kMeals), [&](const FoodEntry& e) { return e.item == item_type; });
}

bool IsDrinkItem(int32_t item_type) {
    return std::any_of(std::begin(kDrinks), std::end(kDrinks),
                       [&](const FoodEntry& e) { return e.item == item_type; });
}

int32_t MealItemOfStructure(int32_t structure_type) {
    for (const FoodEntry& e : kMeals) {
        if (e.structure == structure_type) return e.item;
    }
    return -1;
}

const char* ActionMessage(ActionReason reason) {
    switch (reason) {
    case ActionReason::None: return "Ready";
    case ActionReason::NotAtBase: return "Only possible at the base";
    case ActionReason::LambBusy: return "Lamb is busy";
    case ActionReason::FollowerMissing: return "Follower is not here";
    case ActionReason::FollowerLocked: return "Follower is busy";
    case ActionReason::NotAvailable: return "Not available right now";
    case ActionReason::StaleSession: return "Game changed, try again";
    case ActionReason::Timeout: return "Took too long";
    case ActionReason::PlayerMoved: return "Stopped, Lamb moved";
    case ActionReason::AlreadyBlessed: return "Already blessed today";
    case ActionReason::AlreadyInspired: return "Already inspired today";
    case ActionReason::AlreadyIntimidated: return "Already intimidated today";
    case ActionReason::AlreadyBribed: return "Already bribed today";
    case ActionReason::AlreadyExtorted: return "Already extorted today";
    case ActionReason::NotEnoughGold: return "Not enough gold";
    case ActionReason::AlreadyKissed: return "Already kissed today";
    case ActionReason::AlreadyPetted: return "Already petted today";
    case ActionReason::AlreadyInteracted: return "Already done today";
    case ActionReason::SleepsUntilSeason: return "Sleeps until the season ends";
    case ActionReason::TooCold: return "Too cold to work";
    case ActionReason::NoBuildSites: return "Nothing to build";
    case ActionReason::NoMeals: return "No meals at the base";
    case ActionReason::NoDrinks: return "No drinks at the pub";
    case ActionReason::Fasting: return "The cult is fasting";
    case ActionReason::CantWork: return "Cannot work right now";
    case ActionReason::NoGifts: return "No gifts in stock";
    case ActionReason::HasNecklace: return "Already wears a necklace";
    case ActionReason::HasPet: return "Already has a pet";
    }
    return "Something went wrong";
}

const char* ActionMessage(const ActionStatus& s) {
    if (!s.helper_alive) return "Game is paused";
    switch (s.state) {
    case ActionState::Idle: return "";
    case ActionState::Walking: return "Walking over";
    case ActionState::Running: return "In progress";
    case ActionState::Done: return "Done";
    case ActionState::Refused:
    case ActionState::Cancelled: return ActionMessage(s.reason);
    }
    return "";
}

const char* WalkMessage(const ActionStatus& s) {
    return s.helper_alive && s.state == ActionState::Done ? "Arrived" : ActionMessage(s);
}

} // namespace cotl
