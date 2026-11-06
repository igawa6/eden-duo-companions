// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Module side of the follower actions: sends requests through the mailbox to the guest helper
// (native/helper/cotl/action_tick.c) and reads its status. The ABI is in guest/action_abi.h.
#pragma once

#include <climits>
#include <cstdint>
#include <vector>

namespace cotl {

// Access to the 4096-byte mailbox (host's load_u32/store_u32); false on error or without a hook
struct MailboxIo {
    virtual ~MailboxIo() = default;
    virtual bool Load(uint32_t offset, uint32_t* value) = 0;
    virtual bool Store(uint32_t offset, uint32_t value) = 0;
};

// Values like CotlActionState / CotlActionReason (guest/action_abi.h, via static_assert in actions.cpp)
enum class ActionState : uint32_t { Idle, Walking, Running, Done, Refused, Cancelled };
enum class ActionReason : uint32_t {
    None,
    NotAtBase,
    LambBusy,
    FollowerMissing,
    FollowerLocked,
    NotAvailable,
    StaleSession,
    Timeout,
    PlayerMoved,
    AlreadyBlessed,
    AlreadyInspired,
    AlreadyIntimidated,
    AlreadyBribed,
    NotEnoughGold,
    AlreadyKissed,
    AlreadyPetted,
    AlreadyInteracted,
    SleepsUntilSeason,
    TooCold,
    AlreadyExtorted,
    NoBuildSites,
    NoMeals,
    NoDrinks,
    Fasting,
    CantWork,
    NoGifts,
    HasNecklace,
    HasPet,
};

struct ActionStatus {
    ActionState state = ActionState::Idle;
    ActionReason reason = ActionReason::None;
    bool helper_alive = false; // heartbeat moved within the last kHelperDeadPolls polls
};

// Current task as the wheel asks: Doing and without subtask
enum class FollowerTaskKind { None, Sleep, BedRest };

// What the game reads to choose the command group, read from memory (ReadSnapshot)
struct FollowerCommandState {
    int32_t follower_id = -1;
    int32_t cursed_state = 0;
    std::vector<int32_t> own_traits;
    std::vector<int32_t> cult_traits;
    bool is_snowman = false;
    bool drunk = false;
    bool received_blessing = false;
    int32_t necklace = 0; // ITEM_TYPE
    bool show_loyalty_bars = false;
    // DM.InTutorial @0x850 (the name fits poorly): only if set do NormalCommands and
    // PoetCommands offer Make Demand with Bless. Unreadable counts as set: Bless stays.
    bool in_tutorial = true;
    bool inspire_unlocked = false;    // doctrine WorkWorship_Inspire
    bool intimidate_unlocked = false; // doctrine WorkWorship_Intimidate
    int32_t season = 0;               // SeasonsManager.Season
    int32_t weather = 0;              // SeasonsManager.WeatherEvent
    int32_t phase = 0;                // TimeManager.CurrentPhase
    FollowerTaskKind task = FollowerTaskKind::None;
    // get_CurrentTaskType is 7 or 0x31, in any state (MakeDemandCommands, Sleep)
    bool in_sleep_task = false;
    // Follower.State is TimedAction: Follower.TimedAnimation is running, the helper rejects everything.
    // Unreadable: no lock, the helper checks on tap.
    bool timed_action = false;
    // Simple commands (tools/cotl/README.md): daily flags, wheel conditions, handler rejections
    bool inspired = false;
    bool intimidated = false;
    bool bribed = false;
    bool paid_tithes = false;        // FollowerInfo +0x2D0: already extorted today
    bool kissed = false;
    bool petted = false;
    bool scared_interacted = false;
    bool married = false;            // MarriedToLeader
    bool skin_dog = false;           // SkinName contains Dog or Poppy
    bool bribe_unlocked = false;     // doctrine Possessions_Bribe
    bool extort_unlocked = false;    // doctrine Possessions_ExtortTithes
    bool shrine_built = false;       // DM.HasBuiltShrine1
    bool sozo_freed = false;         // DM.SozoNoLongerBrainwashed
    bool work_through_night = false; // worked through today: hibernation does not count
    int32_t onboarding_phase = 0;    // DM.OnboardingPhase
    int32_t gold = 0;                // first stack in the base inventory, like GetItemQuantity
    // Food and work (tools/cotl/README.md): base buildings (StructureBrain.TYPES, DM.BaseStructures),
    // meals (ITEM_TYPE: lying Structures_Meal and kitchen stocks), free drinks of pubs
    std::vector<int32_t> structures;
    std::vector<int32_t> meals;
    std::vector<int32_t> drinks;
    bool fasting = false;      // FollowerBrainStats.Fasting: less than 3600 since DM.LastFastDeclared
    bool pub_drinking = false; // Structures_Pub.IsDrinking (static)
    bool holiday = false;      // less than 1200 since DM.LastHolidayDeclared
    bool nudism = false;       // less than 600 since DM.LastNudismDeclared
    // Gifts (tools/cotl/README.md): ITEM_TYPEs whose first stack in the base inventory has at least 1 (like
    // GetItemQuantity), DataManager.AllNecklaces, the necklace and the pet. Unreadable: no lock,
    // the helper checks on tap.
    std::vector<int32_t> items;
    std::vector<int32_t> all_necklaces;
    bool showing_necklace = false; // FollowerInfo.ShowingNecklace
    bool has_dlc_pet = false;      // FollowerInfo.DLCPets has an entry
    bool onboarded_wool = false;   // DM.OnboardedWool: Give Pet only after that
};

// The sublists of FollowerCommandGroups, without calling the game (Ruling Command list in
// tools/cotl/README.md). Replica of DefaultCommands 0x203FF80.
enum class CommandGroup {
    Normal,
    WakeUp,
    Baby,
    Snowman,
    SnowmanChild,
    Zombie,
    Dissenter,
    MakeDemand,
    Drunk,
    ExistentialDread,
    OldAge,
    Scared,
    Poet,
    Spy,
    Mutated,
};
CommandGroup SelectCommandGroup(const FollowerCommandState& state);

struct OfferedCommand {
    uint32_t command;
    bool available; // CommandItem.IsAvailable; false: the wheel shows the command locked
    // Reason, if it is not the command's usual one: Bribe without gold, Wake and Worship can only be
    // rejected by the handler, the wheel shows them free (Lock in tools/cotl/README.md)
    ActionReason reason = ActionReason::None;
    // Submenu (Eat, Drink, Work, Gift, Give Pet) in the wheel's order; empty: the wheel opens none
    std::vector<OfferedCommand> sub = {};
};
// Commands the wheel shows the follower. Make Demand flat, Eat, Drink and Work with their submenus.
std::vector<OfferedCommand> OfferedCommands(const FollowerCommandState& state);

enum class ActionSupport { Hidden, NeedsConfirm, Supported };
ActionSupport SupportOf(uint32_t command);

// Button in the follower window
struct ActionRow {
    const char* name;
    uint32_t command;
    ActionReason locked; // None: free; otherwise grey, and this is the reason
    uint32_t parent = 0; // Parent the command is sent with (Submit), 0: none
    bool opens = false;  // opens a submenu instead of sending
    bool enabled() const { return locked == ActionReason::None; }
};
std::vector<ActionRow> VisibleActions(const std::vector<OfferedCommand>& offered);
// Slots for buttons in the follower window (tools/cotl/gen_manifest.py)
inline constexpr int kActionSlots = 12;

// Rows that live only in the module: back to the top level, next page of a long submenu
inline constexpr uint32_t kMenuBack = 0xFFFF0001, kMenuMore = 0xFFFF0002;
// Open submenu in the follower window; parent 0: top level
struct ActionMenu {
    uint32_t parent = 0;
    int page = 0;
};
// Buttons of the level: top level as VisibleActions, and if it does not fit kActionSlots, kActionSlots - 1
// entries per page and More last (without Back). A submenu with Back in the first slot. If it does
// not fit with Back in kActionSlots, kActionSlots - 2 entries per page and More last; the page
// after the last one is the first again. If the submenu does not exist (any more), the top level.
std::vector<ActionRow> MenuActions(const std::vector<OfferedCommand>& offered, const ActionMenu& menu);

// Meals and drinks the wheel knows (MealCommands, DrinkCommands)
bool IsMealItem(int32_t item_type);
bool IsDrinkItem(int32_t item_type);
// ITEM_TYPE of the meal lying around as a building of this type (Structures_Meal), otherwise -1
int32_t MealItemOfStructure(int32_t structure_type);

// Polls without a heartbeat change after which the helper counts as not running (game paused,
// title screen, hook gone). Poll is called once per sample.
inline constexpr int kHelperDeadPolls = 60;
// This many polls the status shows the message after a "Lamb busy" rejection, then
// the helper's state again (running action)
inline constexpr int kBusyShowPolls = 90;
inline constexpr int32_t kAnyFollower = INT32_MIN;

class ActionClient {
public:
    explicit ActionClient(MailboxIo& io) : io_(&io) {}

    // Sends a request. false: not sent (helper not there, previous sequence number not yet
    // acknowledged, mailbox not readable/writable). Writes REQ_* only when the last sequence number is
    // acknowledged (ST_SEQ or ST_BUSY_SEQ == sequence number) and REQ_SEQ last; the helper evaluates
    // fields only when the sequence number has changed. parent: parent of a subcommand (REQ_PARENT), else 0.
    bool Submit(int32_t follower_id, uint32_t command, uint32_t parent = 0);
    // Asks to cancel the last sequence number (CANCEL_SEQ; the helper evaluates it from Task 4)
    bool Cancel();
    // Reads the status of our own last sequence number and runs the heartbeat monitoring. With
    // `selected_follower`: if the sequence number belongs to another follower, the status stays empty (no
    // stale message for another follower). kAnyFollower: do not filter.
    ActionStatus Poll(int32_t selected_follower = kAnyFollower);
    // The helper has reported since startup (heartbeat moved). Stays true during
    // a pause so the buttons do not flicker; false without a hook or without a heartbeat.
    bool Available() const { return seen_; }

private:
    bool Acked();

    MailboxIo* io_;
    bool seen_ = false;      // heartbeat has ever moved
    bool have_hb_ = false;
    uint32_t last_hb_ = 0;
    int stalled_ = 0;        // polls since the last change
    bool init_ = false;
    uint32_t last_seq_ = 0;  // last sequence number sent by us (or found at startup)
    bool sent_ = false;      // we sent a sequence number ourselves
    int32_t last_follower_ = 0;
    uint32_t busy_seen_ = 0; // sequence number whose Busy message is being counted
    int busy_polls_ = 0;
};

// Short English text in the tone of the companion UI
const char* ActionMessage(ActionReason reason);
// Line for the UI: pause, running action, reason for a rejection/cancellation; empty when idle
const char* ActionMessage(const ActionStatus& status);
// The same for Walk over (walking there only): done means arrived
const char* WalkMessage(const ActionStatus& status);

} // namespace cotl
