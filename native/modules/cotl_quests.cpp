// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_quests.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>

#include "cotl_problems.h"

namespace cotl {

namespace {

// Objectives.TYPES (dump.cs)
constexpr int32_t kCollectItem = 1;
constexpr int32_t kBuildStructure = 2;
constexpr int32_t kRecruitFollower = 3;
constexpr int32_t kCustom = 5;
constexpr int32_t kRemoveStructures = 14;
constexpr int32_t kKillEnemies = 8;
constexpr int32_t kNoDodge = 9;
constexpr int32_t kNoDamage = 10;
constexpr int32_t kNoCurses = 11;
constexpr int32_t kShootDummies = 13;
constexpr int32_t kBedRest = 15;
constexpr int32_t kTalkToFollower = 16;
constexpr int32_t kCookMeals = 17;
constexpr int32_t kPlaceStructures = 18;
constexpr int32_t kPerformRitual = 19;
constexpr int32_t kUnlock = 20;
constexpr int32_t kEatMeal = 21;
constexpr int32_t kRecruitCursed = 22;
constexpr int32_t kFindFollower = 23;
constexpr int32_t kDefeatKnucklebones = 24;
constexpr int32_t kUseRelic = 26;
constexpr int32_t kAssignClothing = 27;
constexpr int32_t kDrink = 28;
constexpr int32_t kMating = 29;
constexpr int32_t kCraftClothing = 30;
constexpr int32_t kBlizzardOffering = 31;
constexpr int32_t kFinishRace = 32;
constexpr int32_t kGiveItem = 33;
constexpr int32_t kGetAnimal = 34;
constexpr int32_t kShowFleece = 35;
constexpr int32_t kLegendaryWeaponRun = 36;
constexpr int32_t kFindChildren = 37;
constexpr int32_t kWinFlockadeBet = 38;
constexpr int32_t kFlowerBaskets = 39;
constexpr int32_t kLegendarySwordReturn = 40;
constexpr int32_t kFeedAnimal = 41;
constexpr int32_t kBuildWinterDecoration = 42;
constexpr int32_t kWalkAnimal = 43;

// Result of IntExtensions.ToNumeral for the animal levels that occur
std::string Roman(int32_t v) {
    constexpr const char* kRoman[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
    return v >= 1 && v <= 10 ? kRoman[v - 1] : std::to_string(v);
}

struct Sorted {
    QuestView view;
    float left = std::numeric_limits<float>::max(); // game minutes until expiry
};

std::string FollowerName(const GameSnapshot& s, int32_t id) {
    for (const auto& f : s.followers) {
        if (f.id == id) {
            return f.name;
        }
    }
    return {};
}

// "PERFORM_RITUAL" → "Perform ritual"
std::string TypeLabel(int32_t type) {
    std::string id = Terms::EnumName("ObjectiveType", type);
    std::string out;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i] == '_' ? ' ' : id[i];
        out += i == 0 ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string Due(float left) {
    const int days = static_cast<int>(std::ceil(std::max(left, 0.0f) / kGameMinutesPerDay));
    if (days <= 1 && left < kGameMinutesPerDay) {
        return "due today";
    }
    return std::to_string(days) + (days == 1 ? " day left" : " days left");
}

// Text following ObjectivesData.get_Text of the subclass (terms in tools/cotl/README.md)
void Describe(const Objective& q, const GameSnapshot& s, const Terms& t, QuestView& v) {
    const auto& n = q.ints; // +0x78, +0x7C, +0x80, +0x84, +0x88
    const auto fmt = [&](std::string_view term, const std::vector<std::string>& args) {
        const auto raw = t.Raw(term);
        return raw ? StripMarkup(FormatTerm(*raw, args)) : std::string{};
    };
    const auto num = [](int32_t v) { return std::to_string(v); };
    // Translate a term; if it is missing, the identifier stays readable
    const auto tr = [&](const std::string& term, const std::string& fallback) {
        return StripMarkup(t.Raw(term).value_or(fallback));
    };
    // AnimalInfo.GivenName, unnamed animals are called by their species
    const auto animal_name = [&](int32_t id) {
        for (const auto& a : s.animals) {
            if (a.id == id) {
                return a.name.empty() ? t.Item(a.type) : a.name;
            }
        }
        return std::string{};
    };
    switch (q.type) {
    case kCollectItem: // ItemType @0x78, Target @0x80, Count @0x88
        v.progress = n[4];
        v.target = n[2];
        v.text = fmt("Objectives/CollectItem", {t.Item(n[0]), "", ""});
        if (v.text.empty()) {
            v.text = "Collect " + t.Item(n[0]);
        }
        break;
    case kBuildStructure: // StructureType @0x78, Target @0x7C, Count @0x80
    case kRemoveStructures:
        if (q.type == kBuildStructure) {
            v.text = fmt(n[1] > 1 ? "Objectives/BuildStructure/Plural" : "Objectives/BuildStructure",
                         {t.Structure(n[0]), "", ""});
        }
        if (v.text.empty()) {
            v.text = (q.type == kBuildStructure ? "Build " : "Clean ") + t.Structure(n[0]);
        }
        if (n[1] > 1) {
            v.progress = n[2];
            v.target = n[1];
        }
        break;
    case kRecruitFollower: // Count @0x78 = requested number
        v.text = fmt("Objectives/RecruitFollower/Plural", {std::to_string(n[0]), "", ""});
        v.target = n[0];
        break;
    case kCustom: { // CustomQuestType @0x78, TargetFollowerID @0x7C
        const std::string term = "Objectives/Custom/" + Terms::EnumName("CustomQuest", n[0]);
        v.text = fmt(term, {FollowerName(s, n[1])});
        break;
    }
    case kBedRest:
        v.text = fmt("Objectives/SendFollowerToBedRest", {q.str78});
        break;
    case kCookMeals: // MealType @0x78, Count @0x7C (Ziel)
        v.text = fmt("Objectives/CookMeals", {t.Meal(n[0]), "", ""});
        v.target = n[1];
        break;
    case kPerformRitual: // Ritual (UpgradeSystem.Type) @0x78, TargetFollowerID_1/_2 @0x7C/0x80
        v.text = fmt("Objectives/PerformRitual/" + Terms::EnumName("UpgradeType", n[0]),
                     {FollowerName(s, n[1]), FollowerName(s, n[2])});
        break;
    case kUnlock: // UnlockType (UpgradeSystem.Type) @0x78
        v.text = fmt("Objectives/Unlock/" + Terms::EnumName("UpgradeType", n[0]), {});
        break;
    case kPlaceStructures: { // DecoType @0x7C, Count @0x80, Target @0x84
        constexpr const char* kDecoTerms[] = {"Objectives/PlaceWoolhavenDecos", "Objectives/PlaceRotDecos",
                                              "Objectives/PlaceEwefallDecos",
                                              "Objectives/PlaceSteampunkDecos"};
        const char* term = n[1] >= 1 && n[1] <= 4 ? kDecoTerms[n[1] - 1] : "Objectives/PlaceDecorations";
        v.text = fmt(term, {std::to_string(n[2]), std::to_string(n[3])});
        break;
    }
    case kGiveItem: { // NPCTerm @0x78, Target @0x84, TargetType @0x88
        int32_t owned = 0;
        for (const auto& [type, qty] : s.items) {
            owned += type == n[4] ? qty : 0;
        }
        v.text = fmt("Objectives/Custom/GiveItem", {StripMarkup(t.Raw(q.str78).value_or(q.str78)), t.Item(n[4]),
                                                    std::to_string(owned), std::to_string(n[3])});
        break;
    }
    case kDefeatKnucklebones: // CharacterNameTerm @0x78
        v.text = fmt("Objectives/Custom/Knucklebones", {StripMarkup(t.Raw(q.str78).value_or(q.str78))});
        break;
    case kTalkToFollower: { // TargetFollower @0x88; without a target, the reward from the quest giver
        const std::string target = FollowerName(s, n[4]);
        v.text = target.empty() ? fmt("Objectives/CollectReward", {FollowerName(s, q.follower_id)})
                                : fmt("Objectives/TalkToFollower", {target});
        break;
    }
    case kEatMeal: { // MealType (StructureBrain.TYPES) @0x78, TargetFollower @0x7C
        const std::string meal = Terms::EnumName("StructureType", n[0]);
        v.text = fmt("Objectives/EatMeal",
                     {FollowerName(s, n[1]), StripMarkup(t.Raw("CookingData/" + meal + "/Name").value_or(meal))});
        break;
    }
    case kDrink: // DrinkType @0x78, TargetFollower @0x7C
        v.text = fmt("Objectives/Drink", {FollowerName(s, n[1]), t.Meal(n[0])});
        break;
    case kMating: // TargetFollower_1 @0x78, TargetFollower_2 @0x7C
        v.text = fmt("Objectives/Mating", {FollowerName(s, n[0]), FollowerName(s, n[1])});
        break;
    case kUseRelic:
        v.text = fmt("Objectives/Custom/UseRelic", {});
        break;
    case kFinishRace: { // float target time in seconds @0x78
        float seconds = 0;
        std::memcpy(&seconds, &n[0], sizeof(seconds));
        v.text = fmt("Objectives/FinishRace", {std::to_string(std::lround(seconds))});
        break;
    }
    case kKillEnemies: { // Enemy @0x78, count at quest start @0x7C, needed @0x80
        int32_t killed = 0;
        for (const auto& [enemy, count] : s.enemies_killed) {
            killed = enemy == n[0] ? count : killed;
        }
        const std::string enemy = Terms::EnumName("Enemy", n[0]);
        v.text = fmt("Objectives/KillEnemies",
                     {tr("Enemies/" + enemy, enemy), num(std::max(killed - n[1], 0)), num(n[2])});
        break;
    }
    case kNoDodge: // RoomsRequired @0x78, RoomsCompleted @0x7C
    case kNoDamage:
    case kNoCurses: {
        const char* term = q.type == kNoDodge    ? "Objectives/NoDodge"
                           : q.type == kNoDamage ? "Objectives/NoDamage"
                                                 : "Objectives/NoCurses";
        v.text = fmt(term, {num(n[1]), num(n[0])});
        break;
    }
    case kShootDummies: { // Target @0x78; the tutorial object RatauGiveSpells holds the counter
        v.text = fmt("Objectives/Custom/ShootCurse", {num(n[0]), "", ""});
        if (v.text.ends_with(" /")) {
            v.text.resize(v.text.size() - 2);
        }
        break;
    }
    case kRecruitCursed: // CursedState @0x78, Target @0x7C, Count @0x80, FollowerName @0x88
        if (!q.str2.empty()) {
            v.text = fmt("Objectives/RecruitCursedFollower/None", {q.str2});
        } else {
            v.text = fmt("Objectives/RecruitCursedFollower/" + Terms::EnumName("Thought", n[0]), {});
            if (!v.text.empty()) {
                v.text += " (" + num(n[2]) + "/" + num(n[1]) + ")";
            }
        }
        break;
    case kFindFollower: { // TargetLocation @0x78, TargetFollowerName @0x90
        const std::string place = Terms::EnumName("FollowerLocation", n[0]);
        v.text = fmt("Objectives/FindFollower", {q.str2, tr("NAMES/Places/" + place, place)});
        break;
    }
    case kAssignClothing: { // ClothingType @0x78, TargetFollower @0x7C
        const std::string cloth = Terms::EnumName("ClothingType", n[0]);
        v.text = fmt("Objectives/AssignClothing",
                     {FollowerName(s, n[1]), tr("Clothing/" + cloth + "/Name", cloth)});
        break;
    }
    case kCraftClothing: { // ClothingType @0x78
        const std::string cloth = Terms::EnumName("ClothingType", n[0]);
        v.text = fmt("Objectives/CraftClothing", {tr("Clothing/" + cloth + "/Name", cloth)});
        break;
    }
    case kBlizzardOffering: { // Target @0x78, ItemType @0x7C
        int32_t offered = 0;
        for (const auto& [item, count] : s.offerings) {
            offered = item == n[1] ? count : offered;
        }
        v.text = fmt("Objectives/Custom/BlizzardOffering", {num(offered), num(n[0]), t.Item(n[1])});
        break;
    }
    case kGetAnimal: // AnimalType @0x78, Level @0x7C
        v.text = fmt("Objectives/GetAnimal/" + Terms::EnumName("ItemType", n[0]), {Roman(n[1])});
        break;
    case kFeedAnimal: // AnimalID @0x78, ItemType @0x7C
        v.text = fmt("Objectives/FeedAnimal", {animal_name(n[0]), t.Item(n[1])});
        break;
    case kWalkAnimal: // AnimalID @0x78
        v.text = fmt("Objectives/WalkAnimal", {animal_name(n[0])});
        break;
    case kShowFleece: // FleeceType @0x78; name like TarotCards/Fleece<n>
        v.text = fmt("Objectives/ShowFleece", {tr("TarotCards/Fleece" + num(n[0]) + "/Name", "Fleece")});
        break;
    case kLegendaryWeaponRun: { // EquipmentType @0x78; unknown weapon only by kind
        const bool known = std::find(s.legendary_known.begin(), s.legendary_known.end(), n[0]) !=
                           s.legendary_known.end();
        const char* group = n[0] == 7     ? "Sword"
                            : n[0] == 107 ? "Axe"
                            : n[0] == 207 ? "Hammer"
                            : n[0] == 307 ? "Dagger"
                            : n[0] == 407 ? "Gauntlets"
                            : n[0] == 457 ? "Blunderbuss"
                            : n[0] == 477 ? "Chain"
                                          : nullptr;
        const std::string name = known || group == nullptr
                                     ? t.Weapon(n[0])
                                     : tr(std::string("Objectives/GroupTitles/Legendary") + group, group);
        v.text = fmt("Objectives/LegendaryDungeonRun", {name});
        break;
    }
    case kFindChildren: { // ChildLocation @0x78
        constexpr const char* kPlaces[] = {"Woolhaven", "Temple", "PilgrimsPassage", "SporeGrotto",
                                           "SmugglersSanctuary", "MidasCave"};
        if (n[0] >= 0 && n[0] < 6) {
            v.text = fmt(std::string("Objectives/FindChildren/") + kPlaces[n[0]], {});
        }
        break;
    }
    case kWinFlockadeBet: // opponent term @0x78, bet @0x80, wins @0x84
        v.text = fmt("Objectives/Flockade/WinBetFinal",
                     {num(n[2]), tr(q.str78, q.str78), num(n[3]), num(n[2])});
        break;
    case kLegendarySwordReturn: // 3 days after DataManager @0x254
        v.text = fmt("Objectives/LegendarySwordReturn", {num(std::max(3 - (s.day - s.sword_return_day), 0))});
        break;
    case kFlowerBaskets: // FilledPots @0x78, PotsToFill @0x7C
        v.text = fmt("Objectives/FlowerBaskets", {std::to_string(n[0]), std::to_string(n[1])});
        break;
    case kBuildWinterDecoration: // Target @0x78, Count @0x7C
        v.text = fmt("Objectives/BuildStructure/WinterDecorations", {std::to_string(n[1]), std::to_string(n[0])});
        break;
    default:
        break;
    }
    if (v.text.empty()) {
        v.text = TypeLabel(q.type);
    }
}

} // namespace

std::string FormatTerm(std::string_view fmt, const std::vector<std::string>& args) {
    std::string out(fmt);
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string ph = "{" + std::to_string(i) + "}";
        const size_t at = out.find(ph);
        if (at == std::string::npos) {
            continue;
        }
        if (!args[i].empty()) {
            out.replace(at, ph.size(), args[i]);
            continue;
        }
        // Empty argument: remove the surrounding parenthesis and the space before it
        const size_t open = out.rfind('(', at);
        const size_t close = out.find(')', at);
        if (open != std::string::npos && close != std::string::npos) {
            const size_t from = open > 0 && out[open - 1] == ' ' ? open - 1 : open;
            out.erase(from, close - from + 1);
        } else {
            out.erase(at, ph.size());
        }
    }
    return out;
}

std::vector<QuestView> BuildQuests(const GameSnapshot& s, const Terms& terms) {
    std::vector<QuestView> out;
    for (const int32_t id : s.offered_quest_follower_ids) {
        QuestView v;
        v.offered = true;
        v.key = "offer:" + std::to_string(id);
        v.giver = FollowerName(s, id);
        v.title = "New quest";
        v.text = (v.giver.empty() ? std::string("A follower") : v.giver) +
                 " wants to give you a quest";
        out.push_back(std::move(v));
    }

    // One quest = all objectives with the same UniqueGroupID, in order of first appearance
    // (QuestsMenu.OnShowStarted). Without a group ID, each objective is its own quest.
    std::vector<std::vector<const Objective*>> groups;
    std::vector<std::string> keys;
    for (size_t i = 0; i < s.objectives.size(); ++i) {
        const auto& q = s.objectives[i];
        if (q.failed) {
            continue;
        }
        const std::string key = q.unique_group.empty() ? "#" + std::to_string(i) : q.unique_group;
        const auto it = std::find(keys.begin(), keys.end(), key);
        if (it == keys.end()) {
            keys.push_back(key);
            groups.push_back({&q});
        } else {
            groups[static_cast<size_t>(it - keys.begin())].push_back(&q);
        }
    }

    const float now = static_cast<float>(s.day) * kGameMinutesPerDay + s.time;
    std::vector<Sorted> accepted;
    for (size_t g = 0; g < groups.size(); ++g) {
        const auto& group = groups[g];
        Sorted e;
        e.view.key = keys[g];
        std::vector<QuestStep> open, done;
        for (const Objective* q : group) {
            QuestView part;
            Describe(*q, s, terms, part);
            (q->complete ? done : open).push_back({part.text, q->complete, part.progress, part.target});
            if (e.view.title.empty()) {
                e.view.title = StripMarkup(terms.Raw(q->group_id).value_or(""));
            }
            if (e.view.giver.empty() && q->follower_id >= 0) {
                e.view.giver = FollowerName(s, q->follower_id);
            }
            if (!q->complete && q->has_expiry) {
                e.left = std::min(e.left, q->expire - now);
            }
        }
        if (open.empty()) {
            continue; // all steps done
        }
        if (e.view.title.empty()) {
            e.view.title = "Quest";
        }
        e.view.text = open.front().text;
        e.view.progress = open.front().progress;
        e.view.target = open.front().target;
        if (e.left != std::numeric_limits<float>::max()) {
            e.view.due = Due(e.left);
        }
        e.view.steps = std::move(open);
        e.view.steps.insert(e.view.steps.end(), done.begin(), done.end());
        accepted.push_back(std::move(e));
    }
    std::stable_sort(accepted.begin(), accepted.end(),
                     [](const Sorted& a, const Sorted& b) { return a.left < b.left; });
    for (auto& e : accepted) {
        out.push_back(std::move(e.view));
    }
    return out;
}

void PublishQuests(Publisher& p, const std::vector<QuestView>& quests, int64_t selected) {
    p.I64("quest.count", static_cast<int64_t>(quests.size()));
    for (size_t i = 0; i < quests.size(); ++i) {
        const std::string n = std::to_string(i);
        p.Text("quest.title" + n, quests[i].title);
        p.Text("quest.text" + n, quests[i].text);
        p.Text("quest.giver" + n, quests[i].giver);
        p.Text("quest.due" + n, quests[i].due);
        p.I64("quest.offered" + n, quests[i].offered ? 1 : 0);
        const size_t more = quests[i].steps.size() > 1 ? quests[i].steps.size() - 1 : 0;
        p.Text("quest.more" + n, more == 0 ? std::string{}
                                           : "+" + std::to_string(more) +
                                                 (more == 1 ? " more step" : " more steps"));
    }
    if (selected < 0 || static_cast<size_t>(selected) >= quests.size()) {
        return;
    }
    const auto& q = quests[static_cast<size_t>(selected)];
    p.Text("quest.sel_title", q.title);
    p.Text("quest.sel_giver", q.giver.empty() ? std::string{} : "From " + q.giver);
    p.Text("quest.sel_due", q.due);
    // All steps, open ones first; progress without parentheses
    p.I64("quest.step_count", static_cast<int64_t>(q.steps.size()));
    for (size_t i = 0; i < q.steps.size(); ++i) {
        const auto& st = q.steps[i];
        std::string text = st.text;
        if (st.progress >= 0 && st.target > 0) {
            text += "  " + std::to_string(st.progress) + "/" + std::to_string(st.target);
        }
        p.Text("quest.step" + std::to_string(i), text);
        p.I64("quest.step_done" + std::to_string(i), st.done ? 1 : 0);
    }
}

} // namespace cotl
