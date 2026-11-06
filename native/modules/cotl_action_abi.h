// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Mailbox between the guest helper (action_tick.c) and the module (module.cpp); shared by C and C++.
// Offsets in the load plan's 4096-byte mailbox. The EPOCH word belongs to the host
// (mailbox_epoch_offset in the plan); neither the helper nor the module writes it.
#ifndef COTL_ACTION_ABI_H
#define COTL_ACTION_ABI_H

// Heartbeat: the helper increments it on every PlayerFarming.Update of the main player
#define COTL_ACT_HEARTBEAT 0x00
#define COTL_ACT_EPOCH 0x04 // Session epoch, maintained by the host

// Request from the module
#define COTL_ACT_REQ_SEQ 0x08
#define COTL_ACT_REQ_FOLLOWER 0x0C
#define COTL_ACT_REQ_COMMAND 0x10
#define COTL_ACT_REQ_EPOCH 0x14
#define COTL_ACT_CANCEL_SEQ 0x18
// Parent of a sub-command (Eat 25, Drink 89, Work 1), else 0. The helper then passes
// [REQ_COMMAND, REQ_PARENT] like the wheel (tools/cotl/README.md, eating and work).
#define COTL_ACT_REQ_PARENT 0x1C

// Status of the helper
#define COTL_ACT_ST_SEQ 0x20
#define COTL_ACT_ST_STATE 0x24
#define COTL_ACT_ST_REASON 0x28
// Sequence of the request that was refused because an action is running (reason always LAMB_BUSY).
// The status words above then still belong to the running action; for its sequence S the module
// reads: ST_SEQ == S -> ST_STATE/ST_REASON, otherwise ST_BUSY_SEQ == S -> refused, LAMB_BUSY.
#define COTL_ACT_ST_BUSY_SEQ 0x2C

// Reserved, unused: the module derives the command list itself (ruling on the command list,
// tools/cotl/README.md). The helper does not evaluate these words.
#define COTL_ACT_Q_FOLLOWER 0x30
#define COTL_ACT_Q_SEQ 0x34
#define COTL_ACT_Q_ACK 0x38
#define COTL_ACT_Q_COUNT 0x3C
#define COTL_ACT_Q_COMMANDS 0x40 // 64 x u32
#define COTL_ACT_Q_AVAILABLE 0x140 // u64 bitmask

// Belong to the running action, written by the helper (the module does not read them). A second
// request overwrites REQ_*, so the run keeps checking against its own copies.
#define COTL_ACT_RUN_FOLLOWER 0x150 // Follower ID of the running action
#define COTL_ACT_RUN_EPOCH 0x154    // Session epoch it was accepted with
#define COTL_ACT_RUN_COMMAND 0x158  // Command of the running action
#define COTL_ACT_RUN_PHASE 0x15C    // Phase of the run (see action_tick.c)
#define COTL_ACT_RUN_FRAMES 0x160   // Frames in the current phase
#define COTL_ACT_RUN_TARGET_X 0x164 // Target of the walk (float bits), as passed to GoToAndStop
#define COTL_ACT_RUN_TARGET_Y 0x168
#define COTL_ACT_RUN_TARGET_Z 0x16C
// Diagnostics until task 8, like act.heartbeat: raw float bits of the last stick reading during the
// walk; 0xFFFFFFFF = not read yet in this walk.
#define COTL_ACT_RUN_AXIS_X 0x170
#define COTL_ACT_RUN_AXIS_Y 0x174
// The run's follower as an object pointer. One that vanishes from Follower.Followers often lives on
// (Follower.OnDisable, _dying) and still has to be released. Not a GC reference: it is only read
// through typed() and alive().
#define COTL_ACT_RUN_OBJECT 0x178
// interaction_FollowerInteraction.Task when the command was called. Close finishes it; on re-entry
// OnInteract sets a new one, which is how the hold is recognised. When waking up, until then the
// sleep task, which the hold does not replace. Pointer like RUN_OBJECT, only compared.
#define COTL_ACT_RUN_TASK 0x180
// Parent of the running action, copy of REQ_PARENT. Behind RUN_TASK, which is 8 bytes wide
#define COTL_ACT_RUN_PARENT 0x188

enum CotlActionState {
    COTL_ACT_STATE_IDLE = 0,
    COTL_ACT_STATE_WALKING = 1,
    COTL_ACT_STATE_RUNNING = 2,
    COTL_ACT_STATE_DONE = 3,
    COTL_ACT_STATE_REFUSED = 4,
    COTL_ACT_STATE_CANCELLED = 5,
};

enum CotlActionReason {
    COTL_ACT_REASON_NONE = 0,
    COTL_ACT_REASON_NOT_AT_BASE = 1,
    COTL_ACT_REASON_LAMB_BUSY = 2,
    COTL_ACT_REASON_FOLLOWER_MISSING = 3,
    COTL_ACT_REASON_FOLLOWER_LOCKED = 4,
    COTL_ACT_REASON_NOT_AVAILABLE = 5,
    COTL_ACT_REASON_STALE_SESSION = 6,
    COTL_ACT_REASON_TIMEOUT = 7,
    COTL_ACT_REASON_PLAYER_MOVED = 8,
    COTL_ACT_REASON_ALREADY_BLESSED = 9,
    COTL_ACT_REASON_ALREADY_INSPIRED = 10,
    COTL_ACT_REASON_ALREADY_INTIMIDATED = 11,
    COTL_ACT_REASON_ALREADY_BRIBED = 12,
    COTL_ACT_REASON_NOT_ENOUGH_GOLD = 13,
    COTL_ACT_REASON_ALREADY_KISSED = 14,
    COTL_ACT_REASON_ALREADY_PETTED = 15,
    COTL_ACT_REASON_ALREADY_INTERACTED = 16,
    COTL_ACT_REASON_SLEEPS_UNTIL_SEASON = 17,
    COTL_ACT_REASON_TOO_COLD = 18,
    COTL_ACT_REASON_ALREADY_EXTORTED = 19,
    COTL_ACT_REASON_NO_BUILD_SITES = 20, // Build without a build site: "NoBuildingSites"
    COTL_ACT_REASON_NO_MEALS = 21,       // module only: Eat without a meal
    COTL_ACT_REASON_NO_DRINKS = 22,      // module only: Drink without a drink
    COTL_ACT_REASON_FASTING = 23,        // module only: the cult is fasting
    COTL_ACT_REASON_CANT_WORK = 24,      // module only: Work blocked (holiday, nudity, trait)
    COTL_ACT_REASON_NO_GIFTS = 25,       // module only: Gift without an entry (only the bell)
    COTL_ACT_REASON_HAS_NECKLACE = 26,   // already wears a necklace: "AlreadyHaveNecklace"
    COTL_ACT_REASON_HAS_PET = 27,        // already has a pet: "AlreadyHavePet"
};

// Game offsets of the helper (build 316A4D48..., game 1.5.27/1.5.28); same as
// native/modules/cotl_offsets_1_5_27.h (backed there by a static_assert in native/modules/cotl_module_test.cpp)
#define COTL_ACT_PLAYER_FARMING_TYPEINFO 0x713FE88 // TypeInfo slot of PlayerFarming in main
#define COTL_ACT_CLASS_STATIC_FIELDS 0xB8          // Il2CppClass.static_fields
#define COTL_ACT_PLAYER_FARMING_INSTANCE 0x10      // PlayerFarming.Instance (static)
#define COTL_ACT_PLAYER_LOCATION 0x20              // PlayerFarming.Location (static, FollowerLocation)
#define COTL_ACT_PLAYER_STATE 0x150                // PlayerFarming._state (StateMachine)
#define COTL_ACT_STATE_CURRENT 0x30                // StateMachine.currentstate (Idle 0, Moving 1)
#define COTL_ACT_STATE_LOCK 0x44                   // StateMachine.<LockStateChanges> (bool)
#define COTL_ACT_LAMB_INACTIVE 13                  // StateMachine.State.InActive
#define COTL_ACT_LOCATION_BASE 1                   // FollowerLocation.Base
#define COTL_ACT_UNITY_CACHED_PTR 0x10             // UnityEngine.Object.m_CachedPtr (0 = destroyed)

// Follower, FollowerBrain, FollowerInfo, DataManager, FollowerTask_Floating: TypeInfo slots in main
#define COTL_ACT_FOLLOWER_TYPEINFO 0x713C060
#define COTL_ACT_BRAIN_TYPEINFO 0x713C068
#define COTL_ACT_INFO_TYPEINFO 0x713C098
#define COTL_ACT_DATAMANAGER_TYPEINFO 0x713AC50
#define COTL_ACT_FLOATING_TYPEINFO 0x713C390
#define COTL_ACT_FOLLOWER_FOLLOWERS 0x0   // Follower.Followers (static, List<Follower>)
#define COTL_ACT_DATAMANAGER_INSTANCE 0xE0 // DataManager.instance (static)

// Follower -> FollowerBrain -> FollowerInfo, current task
#define COTL_ACT_FOLLOWER_STATE 0x98       // Follower.State (StateMachine)
#define COTL_ACT_FOLLOWER_SPINE 0xA0       // Follower.Spine
#define COTL_ACT_FOLLOWER_TIMED_ACTION 16  // StateMachine.State.TimedAction, set by TimedAnimation
#define COTL_ACT_FOLLOWER_INTERACTION 0xC8 // Follower.Interaction_FollowerInteraction
#define COTL_ACT_FOLLOWER_BRAIN 0x130      // Follower.<Brain>k__BackingField
#define COTL_ACT_INTERACTION_LIGHTNING 0x1DC // interaction_FollowerInteraction.lightningIncoming
#define COTL_ACT_BRAIN_INFO 0x20           // FollowerBrain._directInfoAccess
#define COTL_ACT_BRAIN_TASK 0xC8           // FollowerBrain._currentTask
#define COTL_ACT_TASK_CHANGE_LOCATION 0x28 // FollowerTask.<ChangeLocationTask> (subtask)
#define COTL_ACT_TASK_STATE 0x20           // FollowerTask.State
#define COTL_ACT_TASK_STATE_DOING 4        // FollowerTaskState.Doing
#define COTL_ACT_INFO_ID 0x10              // FollowerInfo.ID
#define COTL_ACT_INFO_THOUGHT 0x20         // FollowerInfo.CursedState (Thought)
#define COTL_ACT_INFO_LEAVING 0xC0         // FollowerInfo.LeavingCult
#define COTL_ACT_INFO_TRAITS 0x1E8         // FollowerInfo.Traits (List<TraitType>)

// FollowerManager.FollowerLocked liest diese DataManager-Felder
#define COTL_ACT_DM_CULT_TRAITS 0x5C0 // List<TraitType>
#define COTL_ACT_DM_RECRUIT 0xCE8     // Followers_Recruit (List<FollowerInfo>)
#define COTL_ACT_DM_TRAIT_MANIPULATING 0xDC0 // List<int>
#define COTL_ACT_DM_IMPRISONED 0xDC8
#define COTL_ACT_DM_ON_MISSIONARY 0xDD8
#define COTL_ACT_DM_LEFT_IN_DUNGEON 0xDE0
#define COTL_ACT_DM_TRANSITIONING 0xDE8
#define COTL_ACT_DM_DEMONS 0xDF0

// Lamb busy: menus, conversations, scene changes (all read only)
#define COTL_ACT_UIMENUBASE_TYPEINFO 0x7142908    // Lamb.UI.UIMenuBase
#define COTL_ACT_UIMENU_ACTIVE_MENUS 0x0          // UIMenuBase.ActiveMenus (static, List)
#define COTL_ACT_GAMEMANAGER_TYPEINFO 0x713C8A0
#define COTL_ACT_GAMEMANAGER_INMENU 0x24          // GameManager.InMenu (static, bool)
#define COTL_ACT_MMTRANSITION_TYPEINFO 0x713EBF0  // MMTools.MMTransition
#define COTL_ACT_MMTRANSITION_ISPLAYING 0x9       // static, bool
#define COTL_ACT_MMCONVERSATION_TYPEINFO 0x713EBD8 // MMTools.MMConversation
#define COTL_ACT_MMCONVERSATION_ISPLAYING 0x38    // static, bool
#define COTL_ACT_PLAYER_PLAYERS 0x0               // PlayerFarming.players (static, List)
#define COTL_ACT_PLAYER_IN_CONVERSATION 0x378     // PlayerFarming.wasInConversation
#define COTL_ACT_PLAYER_PICKED_UP 0x2C0           // PlayerFarming.PickedUpFollower
#define COTL_ACT_FOLLOWER_DYING 0x150             // Follower._dying

// Walk: PlayerFarming
#define COTL_ACT_PLAYER_CONTROLLER 0xF0           // PlayerFarming._playerController
#define COTL_ACT_PLAYER_UNIT 0xF8                 // PlayerFarming._unitObject
#define COTL_ACT_PLAYER_GOTO_FLAG 0x2E8           // PlayerFarming.GoToAndStopping
#define COTL_ACT_PLAYER_GOTO_CALLBACK 0x2F8       // PlayerFarming.GoToCallback
#define COTL_ACT_PLAYER_ABORT_CALLBACK 0x300      // PlayerFarming.AbortGoToCallback
#define COTL_ACT_PLAYER_GOTO_TARGET 0x310         // PlayerFarming.forcePositionOnTimeoutTarget (Vector3)
#define COTL_ACT_PLAYER_LAST_POSITION 0x320       // PlayerFarming.lastPosition (Vector3)

// Hold: Interaction and interaction_FollowerInteraction (Activated, Task)
#define COTL_ACT_INTERACT_STATE 0x48              // Interaction.state (StateMachine)
#define COTL_ACT_INTERACT_INTERACTABLE 0x79       // Interaction.Interactable
#define COTL_ACT_INTERACT_PLAYER 0x98             // Interaction._playerFarming
#define COTL_ACT_INTERACT_ACTIVATED 0x129         // interaction_FollowerInteraction.Activated
#define COTL_ACT_INTERACT_TASK 0x138              // interaction_FollowerInteraction.Task
#define COTL_ACT_INTERACT_ANIMATOR 0x140          // ...: SimpleSpineAnimator, switched off by the coroutine

// FollowerTask_ManualControl and its siblings: TypeInfo slots in main
#define COTL_ACT_MANUAL_TYPEINFO 0x713C478
#define COTL_ACT_MANUAL_HARD_TYPEINFO 0x713C3D8
#define COTL_ACT_MANUAL_LIGHT_TYPEINFO 0x713C468
#define COTL_ACT_MANUAL_ENFORCING_TYPEINFO 0x713C348
// Sleep: for these OnInteract passes setManualControl = 0 (0x2C59D98)
#define COTL_ACT_SLEEP_TYPEINFO 0x713C5B0      // FollowerTask_Sleep
#define COTL_ACT_BED_REST_TYPEINFO 0x713C5B8   // FollowerTask_SleepBedRest

// Game functions the helper calls (RVA in main)
#define COTL_ACT_FN_CACHE_AND_SET 0x2C5A570  // interaction_FollowerInteraction.CacheAndSetFollower
#define COTL_ACT_FN_RESET_FOLLOWER 0x2C5BE40 // interaction_FollowerInteraction.ResetFollower
#define COTL_ACT_FN_GO_TO 0x1CFBAA0          // PlayerFarming.GoToAndStop(Vector3, ...)
#define COTL_ACT_FN_ABORT_GO_TO 0x1CFC150    // PlayerFarming.AbortGoTo
#define COTL_ACT_FN_GET_GAME_OBJECT 0x531DA60 // Component.get_gameObject
#define COTL_ACT_FN_GET_TRANSFORM 0x531DA10  // Component.get_transform
#define COTL_ACT_FN_GET_POSITION 0x5333280   // Transform.get_position
#define COTL_ACT_FN_IS_ACTIVE 0x531CE80      // Behaviour.get_isActiveAndEnabled
#define COTL_ACT_FN_GO_ACTIVE 0x53221D0      // GameObject.get_activeInHierarchy
#define COTL_ACT_FN_SET_ENABLED 0x531CE10    // Behaviour.set_enabled
#define COTL_ACT_FN_SLEEP_IS_AWAKE 0x2A3A6C0 // FollowerTask_Sleep.get_isAwake
#define COTL_ACT_FN_GAMEPLAY 0x2B3E420       // InputManager.get_Gameplay
#define COTL_ACT_FN_AXIS_H 0x2B3B900         // RewiredGameplayInputSource.GetHorizontalAxis
#define COTL_ACT_FN_AXIS_V 0x2B3BC70         // RewiredGameplayInputSource.GetVerticalAxis
#define COTL_ACT_FN_COMMAND_FINALIZED 0x2C5C200 // interaction_FollowerInteraction.OnFollowerCommandFinalized
#define COTL_ACT_FN_CLOSE 0x2C60370          // interaction_FollowerInteraction.Close(reset, unpause, reopen)
#define COTL_ACT_FN_CONVERSATION_NEW 0x2A1B090 // GameManager.OnConversationNew(bool, bool, PlayerFarming)
#define COTL_ACT_FN_SIM_PAUSE 0x1EB0040      // SimulationManager.Pause
#define COTL_ACT_FN_SIM_UNPAUSE 0x1EB00E0    // SimulationManager.UnPause
#define COTL_ACT_FN_CURRENT_TASK_TYPE 0x299B6F0 // FollowerBrain.get_CurrentTaskType
#define COTL_ACT_FN_ALL_STRUCTURES 0x1F2FD80 // StructureManager.GetAllStructuresOfType
#define COTL_ACT_FN_ARRAY_NEW 0x4806F0       // Il2Cpp: il2cpp_array_new_specific (class, length)
#define COTL_ACT_FN_INIT_METADATA 0x4805F0   // Il2Cpp: resolves the TypeInfo slot (x0: slot address)
// <OnInteract>b__5, never called by the helper: this is how it recognises the walk OnInteract starts for the wheel
#define COTL_ACT_FN_REOPEN_ABORT 0x2C70B30

// Command: handler and what it reads
#define COTL_ACT_COMMAND_BLESS 59                 // FollowerCommands.Bless
// Not a game command: just walk there (Walk over in the quest window). The follower is not held;
// holding would replace its task, for the quest giver FollowerTask_GetAttention with its offer.
#define COTL_ACT_COMMAND_WALK_ONLY 0x10000
#define COTL_ACT_INFO_RECEIVED_BLESSING 0x2D1     // FollowerInfo.ReceivedBlessing (bool)
// Simple commands (re-notes): FollowerCommands and the conditions at which their branch aborts
#define COTL_ACT_COMMAND_EXTORT 8
#define COTL_ACT_AUDIO_MANAGER_TYPEINFO 0x7139908 // Extort: get_Instance on our stack (0x2C5F400), flag and coroutine already run there
#define COTL_ACT_AUDIO_INSTANCE 0x0               // AudioManager._instance (static)
#define COTL_ACT_COMMAND_DANCE 9
#define COTL_ACT_COMMAND_ROMANCE 22
#define COTL_ACT_COMMAND_WAKE_UP 23
#define COTL_ACT_COMMAND_SLEEP 26
#define COTL_ACT_TRAIT_DROWSY 0x44                // Sleep and Worship end in DoDrowsySleepAfterCommandGiven with this
#define COTL_ACT_NOTIFICATION_CENTRE_TYPEINFO 0x713F5C8
#define COTL_ACT_NOTIFICATION_INSTANCE 0x8        // NotificationCentre.Instance (static)
#define COTL_ACT_COMMAND_WORSHIP 27
#define COTL_ACT_COMMAND_INTIMIDATE 43
#define COTL_ACT_COMMAND_BRIBE 44
#define COTL_ACT_COMMAND_PET_DOG 75
#define COTL_ACT_COMMAND_BULLY 85
#define COTL_ACT_COMMAND_REASSURE 86
// Submenus (eating and work): parents and Build, the only role that needs build sites
#define COTL_ACT_COMMAND_WORK 1                   // GiveWorkerCommand_2
#define COTL_ACT_COMMAND_EAT 25                   // EatSomething
#define COTL_ACT_COMMAND_DRINK 89                 // DrinkSomething
#define COTL_ACT_COMMAND_BUILD 28                 // Build
// Gifts (tools/cotl/README.md, gifts): parents, necklaces and what the coroutines read
#define COTL_ACT_COMMAND_GIFT 11                  // Gift: submenu GiftCommands
#define COTL_ACT_COMMAND_GIVE_PET 148             // Give_Animal_Pet: submenu GiveAnimalPetCommands
#define COTL_ACT_COMMAND_REMOVE_NECKLACE 62       // RemoveNecklace
#define COTL_ACT_COMMAND_HIDE_NECKLACE 98         // HideNecklace
#define COTL_ACT_COMMAND_SHOW_NECKLACE 99         // ShowNecklace
#define COTL_ACT_COMMAND_WINTER_NECKLACE 120      // also at the top in the snowman wheel
#define COTL_ACT_COMMAND_WEIRD_NECKLACE 122       // also at the top in the mutant wheel
#define COTL_ACT_INFO_SHOWING_NECKLACE 0x60       // FollowerInfo.ShowingNecklace (bool)
#define COTL_ACT_INFO_NECKLACE 0x8C               // FollowerInfo.Necklace (ITEM_TYPE)
#define COTL_ACT_INFO_DLC_PETS 0xA0               // FollowerInfo.DLCPets (List); null makes GivePetRoutine throw
#define COTL_ACT_DM_ALL_NECKLACES 0x50            // DataManager.AllNecklaces (static, List<ITEM_TYPE>)
#define COTL_ACT_STRUCTURE_BUILD_SITE 31          // StructureBrain.TYPES.BUILD_SITE (Structures_BuildSite)
#define COTL_ACT_STRUCTURE_BUILD_SITE_PROJECT 118 // ...BUILDSITE_BUILDINGPROJECT (Structures_BuildSiteProject)
#define COTL_ACT_INFO_PAID_TITHES 0x2D0           // FollowerInfo.PaidTithes
#define COTL_ACT_INFO_KISSED 0x2D3                // FollowerInfo.KissedAction
#define COTL_ACT_INFO_INSPIRED 0x2D4              // ...Inspired
#define COTL_ACT_INFO_PET_DOG 0x2D5               // ...PetDog
#define COTL_ACT_INFO_INTIMIDATED 0x2D7           // ...Intimidated
#define COTL_ACT_INFO_BRIBED 0x2D8                // ...Bribed
#define COTL_ACT_INFO_SCARED_INTERACTED 0x2D9     // ...ScaredTraitInteracted
#define COTL_ACT_INFO_IS_SNOWMAN 0x210            // ...IsSnowman
#define COTL_ACT_INFO_WORK_THROUGH_NIGHT 0x2E4    // ...: day; equal to DM.CurrentDayIndex means WorkThroughNight
#define COTL_ACT_DM_SEASON 0x16C                  // SeasonsManager.Season
#define COTL_ACT_DM_WEATHER 0x174                 // SeasonsManager.WeatherEvent
#define COTL_ACT_DM_CURRENT_DAY 0x610             // TimeManager.CurrentDay
#define COTL_ACT_DM_ITEMS 0xE80                   // Base inventory, List<InventoryItem>
#define COTL_ACT_ITEM_TYPE 0x10                   // InventoryItem.type
#define COTL_ACT_ITEM_QUANTITY 0x14               // InventoryItem.quantity
#define COTL_ACT_ITEM_GOLD 0x14                   // InventoryItem.ITEM_TYPE.BLACK_GOLD
#define COTL_ACT_BRIBE_COST 3                     // ChangeItemQuantity(0x14, -3); <= 2 is refused by the game
#define COTL_ACT_SEASON_SPRING 0
#define COTL_ACT_SEASON_WINTER 1
#define COTL_ACT_WEATHER_BLIZZARD 1
#define COTL_ACT_TRAIT_HIBERNATION 0x5E           // sleeps in winter
#define COTL_ACT_TRAIT_AESTIVATION 0x5F           // sleeps in spring
#define COTL_ACT_TRAIT_WORK_COLD_CULT 0x57        // CanWork: only as a cult trait
#define COTL_ACT_TRAIT_WORK_COLD 0x79             // CanWork
#define COTL_ACT_TRAIT_MUTATED 0x5A               // CanWork
#define COTL_ACT_BRAIN_BRAIN_INFO 0x10            // FollowerBrain.Info (FollowerBrainInfo), getters read through it
#define COTL_ACT_BRAIN_STATS 0x18                 // FollowerBrain.Stats (FollowerBrainStats), likewise
#define COTL_ACT_BRAIN_INFO_INFO 0x10             // FollowerBrainInfo._info
#define COTL_ACT_STATS_INFO 0x18                  // FollowerBrainStats._info
#define COTL_ACT_INTERACT_FOLLOWER 0x130          // interaction_FollowerInteraction.follower
#define COTL_ACT_INTERACT_LISTENER 0x1D0          // ...eventListener; Bless throws on null (0x2C5F764)
#define COTL_ACT_INTERACT_PREV_TASK 0x1F8         // ...previousTaskType, otherwise set by OnInteract
#define COTL_ACT_INTERACT_PREVENT_CLOSE 0x230     // ...preventCloseOnRemovingPlayer, otherwise Close returns early
#define COTL_ACT_INT_ARRAY_TYPEINFO 0x7138528     // int[]: same shape as FollowerCommands[] (no slot of its own)
#define COTL_ACT_GAMEMANAGER_INSTANCE 0x0         // GameManager.instance (static)

// Delegate and closure of <OnInteract>: read only
#define COTL_ACT_DELEGATE_METHOD_PTR 0x10         // Delegate.method_ptr
#define COTL_ACT_DELEGATE_TARGET 0x20             // Delegate.m_target
#define COTL_ACT_CLOSURE_THIS 0x10                // <>c__DisplayClass52_0.<>4__this

// Daycare: Interaction_Daycare.IsInDaycare replicated, the original throws on missing data
#define COTL_ACT_STRUCTURE_MANAGER_TYPEINFO 0x7141768
#define COTL_ACT_CLASS_CCTOR_FINISHED 0xE0        // Il2CppClass.cctor_finished
#define COTL_ACT_STRUCTURE_DAYCARE 0x177          // StructureBrain.TYPES.DAYCARE
#define COTL_ACT_STRUCTURE_BRAIN_DATA 0x10        // StructureBrain.Data
#define COTL_ACT_STRUCTURE_FOLLOWERS 0x140        // StructuresData: List<int> of the followers

#define COTL_ACT_TASK_BRAIN 0x18                  // FollowerTask._brain; get_isAwake throws on null

// Thought and TraitType values at which FollowerLocked blocks
#define COTL_ACT_THOUGHT_STARVING 0x83   // Thought.BecomeStarving
#define COTL_ACT_THOUGHT_FREEZING 0x1E0
#define COTL_ACT_THOUGHT_CHILD 0x188
#define COTL_ACT_TRAIT_EXISTENTIAL_DREAD 0x4E
#define COTL_ACT_TRAIT_MISSIONARY_TERRIFIED 0x43

#endif
