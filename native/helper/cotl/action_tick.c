// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

// Companion guest helper, runs on the game thread. Stage 1: heartbeat. Stage 2: check requests from
// the mailbox and accept or refuse them (read only). Stage 3: an accepted request holds the
// follower, sends the Lamb to it and releases it again at the end.
//
// These are the only places with an effect on the game: exactly the COTL_ACT_FN_* functions are
// called and exactly the two fields state/_playerFarming of the interaction are written. A managed
// exception could not propagate through these frames, so before every call we check what would
// otherwise make the function throw (see tools/cotl/README.md, "Walk to the follower").
//
// Hooked at the first instruction of PlayerFarming.Update (0x1CFDCF0, "sub sp, sp, #0x120"),
// which the plan replaces with "b code". The entry below saves x0-x7, calls cotl_action_tick,
// restores everything, executes the replaced instruction and jumps behind the hook.
// x19-x30 and sp are preserved by the C call itself.
#include <stdint.h>

#include "../../modules/cotl_action_abi.h"

typedef uint64_t Addr;

// Entry at offset 0 of guest_code. The placeholders written with .word are replaced by the host's
// loader with the plan's relocations (label names: see gen_action_helper.py).
__asm__(
    ".section .text.entry,\"ax\"\n"
    ".global cotl_action_entry\n"
    "cotl_action_entry:\n"
    "  stp x29, x30, [sp, #-80]!\n"
    "  stp x0, x1, [sp, #16]\n"
    "  stp x2, x3, [sp, #32]\n"
    "  stp x4, x5, [sp, #48]\n"
    "  stp x6, x7, [sp, #64]\n"
    "  mov x29, sp\n"
    // x1 = mailbox, x2 = main (x0 stays self)
    ".global cotl_reloc_mailbox_adrp\n"
    "cotl_reloc_mailbox_adrp:\n"
    "  .word 0x90000001\n" // adrp x1, #0
    "  .word 0x91000021\n" // add x1, x1, #0
    ".global cotl_reloc_main_adrp\n"
    "cotl_reloc_main_adrp:\n"
    "  .word 0x90000002\n" // adrp x2, #0
    "  .word 0x91000042\n" // add x2, x2, #0
    "  bl cotl_action_tick\n"
    "  ldp x6, x7, [sp, #64]\n"
    "  ldp x4, x5, [sp, #48]\n"
    "  ldp x2, x3, [sp, #32]\n"
    "  ldp x0, x1, [sp, #16]\n"
    "  ldp x29, x30, [sp], #80\n"
    "  .word 0xd10483ff\n" // sub sp, sp, #0x120 (the replaced instruction, ff8304d1)
    ".global cotl_reloc_resume\n"
    "cotl_reloc_resume:\n"
    "  .word 0x14000000\n" // b hook+4
    ".text\n");

// Small pieces for everything involving floats: the helper is built with -mgeneral-regs-only, so C
// only sees the bits (uint32_t). FP instructions as .word. All keep sp 16-aligned; s0-s3 belong to
// the caller (AAPCS), and PlayerFarming.Update gets no FP arguments at the hook site.
__asm__(
    ".text\n"
    // uint32_t cotl_call_float(fn, a0, a1, a2): calls fn(a0, a1, a2), returns the bits of s0
    ".type cotl_call_float,%function\n"
    "cotl_call_float:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x1, x2\n"
    "  mov x2, x3\n"
    "  blr x16\n"
    "  .word 0x1E260000\n" // fmov w0, s0
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // void cotl_call_vec3(fn, a0, out): out[0..2] = bits of fn(a0, null) (Vector3 in s0-s2)
    ".type cotl_call_vec3,%function\n"
    "cotl_call_vec3:\n"
    "  stp x29, x30, [sp, #-32]!\n"
    "  str x19, [sp, #16]\n"
    "  mov x29, sp\n"
    "  mov x19, x2\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  mov x1, xzr\n"
    "  blr x16\n"
    "  .word 0x1E260000\n" // fmov w0, s0
    "  str w0, [x19]\n"
    "  .word 0x1E260020\n" // fmov w0, s1
    "  str w0, [x19, #4]\n"
    "  .word 0x1E260040\n" // fmov w0, s2
    "  str w0, [x19, #8]\n"
    "  ldr x19, [sp, #16]\n"
    "  ldp x29, x30, [sp], #32\n"
    "  ret\n"
    // void cotl_call_go_to(fn, self, x, y, z, look, max_duration): GoToAndStop(Vector3) with the
    // game's arguments from interaction_FollowerInteraction.OnInteract (0x2C5A294), without
    // callbacks and with IdleOnEnd 1: OnInteract passes 0 because its callback opens the
    // interaction and Close releases the Lamb later. We have none, and without IdleOnEnd
    // EndGoToAndStop sets InActive (0x1CFCF84). DisableCollider 1, force 0, groupAction 0; stack:
    // 1, 1, 0, empty Nullable<Vector3>, MethodInfo null.
    ".type cotl_call_go_to,%function\n"
    "cotl_call_go_to:\n"
    "  stp x29, x30, [sp, #-16]!\n"
    "  mov x29, sp\n"
    "  sub sp, sp, #0x30\n"
    "  mov x8, #1\n"
    "  stp x8, x8, [sp]\n"
    "  stp xzr, xzr, [sp, #0x10]\n"
    "  stp xzr, xzr, [sp, #0x20]\n"
    "  mov x16, x0\n"
    "  mov x0, x1\n"
    "  .word 0x1E270040\n" // fmov s0, w2
    "  .word 0x1E270061\n" // fmov s1, w3
    "  .word 0x1E270082\n" // fmov s2, w4
    "  .word 0x1E2700C3\n" // fmov s3, w6
    "  mov x1, x5\n"
    "  mov w2, #1\n"
    "  mov w3, #1\n"
    "  mov x4, xzr\n"
    "  mov w5, wzr\n"
    "  mov x6, xzr\n"
    "  mov w7, wzr\n"
    "  blr x16\n"
    "  mov sp, x29\n"
    "  ldp x29, x30, [sp], #16\n"
    "  ret\n"
    // uint32_t cotl_float_add(a, b): bits of a + b
    ".type cotl_float_add,%function\n"
    "cotl_float_add:\n"
    "  .word 0x1E270000\n" // fmov s0, w0
    "  .word 0x1E270021\n" // fmov s1, w1
    "  .word 0x1E212800\n" // fadd s0, s0, s1
    "  .word 0x1E260000\n" // fmov w0, s0
    "  ret\n"
    // int cotl_float_ge(a, b): a >= b (NaN: no)
    ".type cotl_float_ge,%function\n"
    "cotl_float_ge:\n"
    "  .word 0x1E270000\n" // fmov s0, w0
    "  .word 0x1E270021\n" // fmov s1, w1
    "  .word 0x1E212000\n" // fcmp s0, s1
    "  cset w0, ge\n"
    "  ret\n"
    // int cotl_float_near(a, b, tol): |a - b| <= tol (NaN: no)
    ".type cotl_float_near,%function\n"
    "cotl_float_near:\n"
    "  .word 0x1E270000\n" // fmov s0, w0
    "  .word 0x1E270021\n" // fmov s1, w1
    "  .word 0x1E270042\n" // fmov s2, w2
    "  .word 0x1E213800\n" // fsub s0, s0, s1
    "  .word 0x1E20C000\n" // fabs s0, s0
    "  .word 0x1E222000\n" // fcmp s0, s2
    "  cset w0, ls\n"
    "  ret\n");

uint32_t cotl_call_float(Addr fn, Addr a0, Addr a1, Addr a2);
void cotl_call_vec3(Addr fn, Addr a0, uint32_t *out);
void cotl_call_go_to(Addr fn, Addr self, uint32_t x, uint32_t y, uint32_t z, Addr look,
                     uint32_t max_duration);
uint32_t cotl_float_add(uint32_t a, uint32_t b);
int cotl_float_ge(uint32_t a, uint32_t b);
int cotl_float_near(uint32_t a, uint32_t b, uint32_t tol);

// Bits of the float values needed
#define FLOAT_MINUS_2 0xC0000000u
#define FLOAT_PLUS_2 0x40000000u
#define FLOAT_1_5 0x3FC00000u
#define FLOAT_0 0x00000000u
#define FLOAT_30 0x41F00000u
#define STICK_LIMIT 0x3E99999Au // 0.3; the game's dead zone is 0.1. Greater means moved, NaN too

enum {
    // How long to wait for the coroutine that holds the follower. It needs two frame ends; if it
    // does not get its turn in this time, it never will.
    kHoldTimeoutFrames = 60,
    // Emergency brake for the walk. The game itself ends it after maxDuration = 30 s real time or
    // as soon as the Lamb stands still for 1 s. 2400 frames are 40 s at 60 fps and 80 s at 30 fps,
    // so even at 60 fps longer than a walk from corner to corner of the base (which the game
    // gives 30 s).
    kWalkTimeoutFrames = 2400,
    // Emergency brake for the command: if the game's coroutine throws (Unity catches that), Close
    // never comes. Blessing with a level-up takes seconds in the game; 3600 frames are 60 s at 60 fps.
    kRunTimeoutFrames = 3600,
};

// Phases of the run (RUN_PHASE)
enum {
    PHASE_NONE = 0,
    PHASE_HOLD_WAIT = 1,   // CacheAndSetFollower called, task not seen yet
    PHASE_HOLD_SETTLE = 2, // task seen; the caching happens at the end of this frame
    PHASE_WALK = 3,        // GoToAndStop called
    PHASE_RUN = 4,         // handler called; the game executes the command until Close runs
    PHASE_REOPEN_WAIT = 5, // Close restarted OnInteract; its coroutine holds the follower
    PHASE_REOPEN_SETTLE = 6, // task seen; the caching happens at the end of this frame
};

#define PTR(p, o) (*(const volatile Addr *)((Addr)(p) + (o)))
#define U32(p, o) (*(const volatile uint32_t *)((Addr)(p) + (o)))
#define U8(p, o) (*(const volatile uint8_t *)((Addr)(p) + (o)))
#define GAME_FN(main, rva) ((Addr)(main) + (rva))

// Il2Cpp list and array: List<T>._items @0x10, _size @0x18; array: length @0x18, data @0x20
#define LIST_ITEMS 0x10
#define LIST_SIZE 0x18
#define ARRAY_LENGTH 0x18
#define ARRAY_DATA 0x20
#define MAX_LIST 4096 // no game list is larger; guards against loops over garbage

// Pointers we may read: aligned and in user address space
static int ptr(Addr p) { return p >= 0x1000000 && p < ((Addr)1 << 39) && !(p & 7); }

// static_fields of a class via its TypeInfo slot; 0 while the class or statics are missing
static Addr statics_of(Addr main, Addr typeinfo) {
    Addr klass = PTR(main, typeinfo);
    if (!ptr(klass)) return 0;
    Addr statics = PTR(klass, COTL_ACT_CLASS_STATIC_FIELDS);
    return ptr(statics) ? statics : 0;
}

// Object of exactly this class: the class pointer in the header equals the TypeInfo slot
static int typed(Addr main, Addr p, Addr typeinfo) {
    if (!ptr(p)) return 0;
    Addr klass = PTR(main, typeinfo);
    return ptr(klass) && PTR(p, 0) == klass;
}

// Unity object is still alive: a destroyed one has m_CachedPtr 0 (the C# object stays, op_Equality
// with null is then true). No call, read only.
static int alive(Addr p) { return ptr(p) && PTR(p, COTL_ACT_UNITY_CACHED_PTR) != 0; }

// PlayerFarming.Instance via the TypeInfo slot; 0 while the class or statics are missing
static Addr main_player(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_PLAYER_FARMING_TYPEINFO);
    return statics ? PTR(statics, COTL_ACT_PLAYER_FARMING_INSTANCE) : 0;
}

// Elements of a List<T> (size checked against the array length): 1 readable, 0 implausible
static int list_of(Addr list, Addr *data, uint32_t *count) {
    if (!ptr(list)) return 0;
    uint32_t n = U32(list, LIST_SIZE);
    if (n > MAX_LIST) return 0;
    *count = n;
    *data = 0;
    if (!n) return 1;
    Addr array = PTR(list, LIST_ITEMS);
    if (!ptr(array) || n > U32(array, ARRAY_LENGTH)) return 0;
    *data = array + ARRAY_DATA;
    return 1;
}

// Does the List<int> or List<enum> contain v? 1 yes, 0 no, -1 list unreadable
static int has_u32(Addr list, uint32_t v) {
    Addr data;
    uint32_t n;
    if (!list_of(list, &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i)
        if (U32(data, i * 4) == v) return 1;
    return 0;
}

// Does the List<reference> contain p? 1 yes, 0 no, -1 list unreadable
static int has_ptr(Addr list, Addr p) {
    Addr data;
    uint32_t n;
    if (!list_of(list, &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i)
        if (PTR(data, i * 8) == p) return 1;
    return 0;
}

// The follower with this ID from Follower.Followers; 0 if it does not exist (alive, with brain and
// info). Used instead of FindFollowerByID, which would execute game code.
static Addr find_follower(Addr main, uint32_t id) {
    Addr statics = statics_of(main, COTL_ACT_FOLLOWER_TYPEINFO);
    if (!statics) return 0;
    Addr data;
    uint32_t n;
    if (!list_of(PTR(statics, COTL_ACT_FOLLOWER_FOLLOWERS), &data, &n)) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        Addr f = PTR(data, i * 8);
        if (!typed(main, f, COTL_ACT_FOLLOWER_TYPEINFO) || !alive(f)) continue;
        if (U8(f, COTL_ACT_FOLLOWER_DYING)) continue; // dying: counts as missing
        Addr brain = PTR(f, COTL_ACT_FOLLOWER_BRAIN);
        if (!typed(main, brain, COTL_ACT_BRAIN_TYPEINFO)) continue;
        Addr info = PTR(brain, COTL_ACT_BRAIN_INFO);
        if (!typed(main, info, COTL_ACT_INFO_TYPEINFO)) continue;
        if (U32(info, COTL_ACT_INFO_ID) == id) return f;
    }
    return 0;
}

// FollowerBrain.HasTrait: own traits or those of the whole cult. 1/0, -1 unreadable
static int has_trait(Addr info, Addr dm, uint32_t trait) {
    Addr own = PTR(info, COTL_ACT_INFO_TRAITS);
    if (own) {  // the game skips a missing list; a garbage pointer, however, is unreadable
        int r = has_u32(own, trait);
        if (r) return r;
    }
    return has_u32(PTR(dm, COTL_ACT_DM_CULT_TRAITS), trait);
}

// FollowerManager.FollowerLocked(ID) with the default values (game: exludeStarving/Child/Freezing
// false, excludeLightningTargeted true, excludeHotTub false), replicated by reading only; the
// order follows the method (RVA 0x29D2500). 1 locked, 0 free, -1 data unreadable. Unreadable
// counts as locked for the caller: better to refuse than to guess.
static int follower_locked(Addr main, Addr f) {
    Addr brain = PTR(f, COTL_ACT_FOLLOWER_BRAIN);
    Addr info = PTR(brain, COTL_ACT_BRAIN_INFO);
    // Lightning planned (only if the interaction is still alive)
    Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
    if (alive(inter) && U8(inter, COTL_ACT_INTERACTION_LIGHTNING)) return 1;
    if (U8(info, COTL_ACT_INFO_LEAVING)) return 1;
    uint32_t thought = U32(info, COTL_ACT_INFO_THOUGHT);
    if (thought == COTL_ACT_THOUGHT_STARVING || thought == COTL_ACT_THOUGHT_FREEZING ||
        thought == COTL_ACT_THOUGHT_CHILD)
        return 1;
    // get_CurrentTaskType: if there is a subtask (ChangeLocation), its type counts; otherwise the
    // task itself. Only FollowerTaskType.Floating (92) is locked, the class for it is
    // FollowerTask_Floating. The virtual call is replaced by the class comparison.
    Addr task = PTR(brain, COTL_ACT_BRAIN_TASK);
    if (task) {
        if (!ptr(task)) return -1;
        if (!PTR(task, COTL_ACT_TASK_CHANGE_LOCATION) &&
            typed(main, task, COTL_ACT_FLOATING_TYPEINFO))
            return 1;
    }
    Addr dm_statics = statics_of(main, COTL_ACT_DATAMANAGER_TYPEINFO);
    Addr dm = dm_statics ? PTR(dm_statics, COTL_ACT_DATAMANAGER_INSTANCE) : 0;
    if (!typed(main, dm, COTL_ACT_DATAMANAGER_TYPEINFO)) return -1;
    int r = has_trait(info, dm, COTL_ACT_TRAIT_EXISTENTIAL_DREAD);
    if (r) return r;
    r = has_trait(info, dm, COTL_ACT_TRAIT_MISSIONARY_TERRIFIED);
    if (r) return r;
    uint32_t id = U32(info, COTL_ACT_INFO_ID);
    static const uint16_t id_lists[] = {
        COTL_ACT_DM_IMPRISONED, COTL_ACT_DM_TRAIT_MANIPULATING, COTL_ACT_DM_ON_MISSIONARY,
        COTL_ACT_DM_DEMONS,     COTL_ACT_DM_TRANSITIONING,      COTL_ACT_DM_LEFT_IN_DUNGEON};
    for (unsigned i = 0; i < sizeof id_lists / sizeof id_lists[0]; ++i) {
        r = has_u32(PTR(dm, id_lists[i]), id);
        if (r) return r;
    }
    return has_ptr(PTR(dm, COTL_ACT_DM_RECRUIT), info);
}

// Is a player carrying this follower? (Follower.PickUp sets PlayerFarming.PickedUpFollower and
// the ManualControl task, which FollowerLocked does not know.) 1 yes, 0 no, -1 data unreadable
static int picked_up(Addr main, Addr f) {
    Addr statics = statics_of(main, COTL_ACT_PLAYER_FARMING_TYPEINFO);
    Addr data;
    uint32_t n;
    if (!statics || !list_of(PTR(statics, COTL_ACT_PLAYER_PLAYERS), &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        Addr p = PTR(data, i * 8);
        if (!typed(main, p, COTL_ACT_PLAYER_FARMING_TYPEINFO)) return -1;
        if (PTR(p, COTL_ACT_PLAYER_PICKED_UP) == f) return 1;
    }
    return 0;
}

// The follower cannot start anything new right now: locked (FollowerLocked) or carried.
// Unreadable counts as unavailable.
static int follower_unavailable(Addr main, Addr f) {
    return follower_locked(main, f) != 0 || picked_up(main, f) != 0;
}

// The Lamb is at the base location (PlayerFarming.Location, static)
static int at_base(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_PLAYER_FARMING_TYPEINFO);
    return statics && (int32_t)U32(statics, COTL_ACT_PLAYER_LOCATION) == COTL_ACT_LOCATION_BASE;
}

// Static bool of a class. If the class is unused (TypeInfo slot still unresolved or without
// static_fields: its code never ran, since it resolves the slot on its first call), it is not
// set.
static int static_flag(Addr main, Addr typeinfo, Addr off) {
    Addr statics = statics_of(main, typeinfo);
    return statics && U8(statics, off);
}

// A menu is open: UIMenuBase.ActiveMenus is not empty (Show adds, Hide removes; the pause menu
// inherits _addToActiveMenus = true). 1 yes, 0 no, -1 list unreadable. Uninitialised class or
// null list: no menu.
static int menu_open(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_UIMENUBASE_TYPEINFO);
    Addr list = statics ? PTR(statics, COTL_ACT_UIMENU_ACTIVE_MENUS) : 0;
    if (!list) return 0;
    Addr data;
    uint32_t n;
    if (!list_of(list, &data, &n)) return -1;
    return n != 0;
}

// PlayerFarming.AnyPlayerInConversation replicated: wasInConversation (@0x378) of any player.
// 1 yes, 0 no, -1 unreadable
static int player_in_conversation(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_PLAYER_FARMING_TYPEINFO);
    Addr data;
    uint32_t n;
    if (!statics || !list_of(PTR(statics, COTL_ACT_PLAYER_PLAYERS), &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        Addr p = PTR(data, i * 8);
        if (!typed(main, p, COTL_ACT_PLAYER_FARMING_TYPEINFO)) return -1;
        if (U8(p, COTL_ACT_PLAYER_IN_CONVERSATION)) return 1;
    }
    return 0;
}

// A menu, conversation or scene change is running (unreadable counts). Also applies during the walk:
// then the helper aborts instead of letting the Lamb walk into a menu or conversation.
static int game_busy(Addr main) {
    return menu_open(main) != 0 || player_in_conversation(main) != 0 ||
           static_flag(main, COTL_ACT_GAMEMANAGER_TYPEINFO, COTL_ACT_GAMEMANAGER_INMENU) ||
           static_flag(main, COTL_ACT_MMTRANSITION_TYPEINFO, COTL_ACT_MMTRANSITION_ISPLAYING) ||
           static_flag(main, COTL_ACT_MMCONVERSATION_TYPEINFO, COTL_ACT_MMCONVERSATION_ISPLAYING);
}

// The Lamb can be sent off: state Idle or Moving, and not game_busy. Unreadable counts as busy.
// Only before the walk: during the walk the game sets the state itself.
static int lamb_free(Addr main, Addr player) {
    if (game_busy(main)) return 0;
    Addr state = PTR(player, COTL_ACT_PLAYER_STATE);
    if (!alive(state)) return 0;
    uint32_t s = U32(state, COTL_ACT_STATE_CURRENT);
    return s == 0 || s == 1; // StateMachine.State.Idle, Moving
}

// Task is FollowerTask_ManualControl or a variant of it: the game holds the follower
static int manual_task(Addr main, Addr task) {
    return typed(main, task, COTL_ACT_MANUAL_TYPEINFO) ||
           typed(main, task, COTL_ACT_MANUAL_HARD_TYPEINFO) ||
           typed(main, task, COTL_ACT_MANUAL_LIGHT_TYPEINFO) ||
           typed(main, task, COTL_ACT_MANUAL_ENFORCING_TYPEINFO);
}

// Is the follower asleep? OnInteract (0x2C59C7C..0x2C59D98) holds sleepers without ManualControl;
// the helper always passes setManualControl = 1, so HardSwapToTask would interrupt the sleep. The
// game's condition: no subtask (get_CurrentTaskType would otherwise take its type), state Doing,
// and for FollowerTask_Sleep (type 7) not get_isAwake. get_isAwake throws in winter without _brain
// (0x2A3A708); a sleep task without a brain therefore counts as sleeping, without the call.
static int sleeping(Addr main, Addr task) {
    if (!ptr(task) || PTR(task, COTL_ACT_TASK_CHANGE_LOCATION) ||
        U32(task, COTL_ACT_TASK_STATE) != COTL_ACT_TASK_STATE_DOING)
        return 0;
    if (typed(main, task, COTL_ACT_BED_REST_TYPEINFO)) return 1;
    if (!typed(main, task, COTL_ACT_SLEEP_TYPEINFO)) return 0;
    if (!ptr(PTR(task, COTL_ACT_TASK_BRAIN))) return 1;
    typedef uint64_t (*IsAwake)(Addr self, Addr method);
    return !(((IsAwake)GAME_FN(main, COTL_ACT_FN_SLEEP_IS_AWAKE))(task, 0) & 1);
}

// A cached part of the Lamb is null (the getter then looks it up) or alive
static int part_ok(Addr player, Addr off) {
    Addr part = PTR(player, off);
    return !part || alive(part);
}

// Interaction_Daycare.IsInDaycare (0x2BAF130) replicated: it throws on missing data. 1 yes, 0 no,
// -1 data unreadable. If StructureManager is still unused (class constructor has not run), there
// are no buildings; calling it would only trigger the constructor.
static int in_daycare(Addr main, uint32_t id) {
    Addr klass = PTR(main, COTL_ACT_STRUCTURE_MANAGER_TYPEINFO);
    if (!ptr(klass) || !U32(klass, COTL_ACT_CLASS_CCTOR_FINISHED)) return 0;
    typedef Addr (*GetAll)(uint32_t type, uint32_t a, uint32_t b, Addr method);
    Addr list = ((GetAll)GAME_FN(main, COTL_ACT_FN_ALL_STRUCTURES))(COTL_ACT_STRUCTURE_DAYCARE, 0, 0, 0);
    Addr data;
    uint32_t n;
    if (!list_of(list, &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        Addr brain = PTR(data, i * 8);
        if (!ptr(brain)) return -1;
        Addr structure = PTR(brain, COTL_ACT_STRUCTURE_BRAIN_DATA);
        if (!ptr(structure)) return -1;
        int r = has_u32(PTR(structure, COTL_ACT_STRUCTURE_FOLLOWERS), id);
        if (r) return r;
    }
    return 0;
}

// DataManager.instance, 0 if unreadable
static Addr data_manager(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_DATAMANAGER_TYPEINFO);
    Addr dm = statics ? PTR(statics, COTL_ACT_DATAMANAGER_INSTANCE) : 0;
    return typed(main, dm, COTL_ACT_DATAMANAGER_TYPEINFO) ? dm : 0;
}

// Inventory.GetItemQuantity (0x1BBD440) replicated, it throws on missing data: the first stack of
// the type counts. -1 unreadable
static int32_t item_quantity(Addr dm, uint32_t type) {
    Addr data;
    uint32_t n;
    if (!list_of(PTR(dm, COTL_ACT_DM_ITEMS), &data, &n)) return -1;
    for (uint32_t i = 0; i < n; ++i) {
        Addr item = PTR(data, i * 8);
        if (!ptr(item)) return -1;
        if (U32(item, COTL_ACT_ITEM_TYPE) == type) return (int32_t)U32(item, COTL_ACT_ITEM_QUANTITY);
    }
    return 0;
}

static int32_t gold_quantity(Addr dm) { return item_quantity(dm, COTL_ACT_ITEM_GOLD); }

// Blizzard, and the follower cannot work (FollowerBrain.CanWork 0x2998ED0)
static uint32_t cold_refusal(Addr info, Addr dm) {
    if (U32(dm, COTL_ACT_DM_WEATHER) != COTL_ACT_WEATHER_BLIZZARD) return COTL_ACT_REASON_NONE;
    int r = has_u32(PTR(dm, COTL_ACT_DM_CULT_TRAITS), COTL_ACT_TRAIT_WORK_COLD_CULT);
    if (!r) r = has_trait(info, dm, COTL_ACT_TRAIT_WORK_COLD);
    if (!r) r = has_trait(info, dm, COTL_ACT_TRAIT_MUTATED);
    if (r < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
    return r || U8(info, COTL_ACT_INFO_IS_SNOWMAN) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_TOO_COLD;
}

// WakeUp branch of the handler (0x2C5CF2C): hibernation and aestivation, unless the follower works through today
static uint32_t wake_up_refusal(Addr info, Addr dm) {
    if (U32(info, COTL_ACT_INFO_WORK_THROUGH_NIGHT) != U32(dm, COTL_ACT_DM_CURRENT_DAY)) {
        uint32_t season = U32(dm, COTL_ACT_DM_SEASON);
        int r = 0;
        if (season == COTL_ACT_SEASON_WINTER)
            r = has_trait(info, dm, COTL_ACT_TRAIT_HIBERNATION);
        else if (season == COTL_ACT_SEASON_SPRING)
            r = has_trait(info, dm, COTL_ACT_TRAIT_AESTIVATION);
        if (r < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
        if (r) return COTL_ACT_REASON_SLEEPS_UNTIL_SEASON;
    }
    return cold_refusal(info, dm);
}

// Sleep and Worship end in the same block (0x2C5ED0C): with Drowsy, chance < 0.9 and a matching role
// it synchronously calls DoDrowsySleepAfterCommandGiven (0x2C5F07C), which throws without
// NotificationCentre.Instance (0x2C61C1C). Role 0, which Worship sets, is included (0x2C5F074).
// The helper does not know the chance, so this applies to everyone with the trait.
static uint32_t drowsy_refusal(Addr main, Addr info) {
    Addr dm = data_manager(main);
    int r = dm ? has_trait(info, dm, COTL_ACT_TRAIT_DROWSY) : -1;
    if (r < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
    if (!r) return COTL_ACT_REASON_NONE;
    Addr statics = statics_of(main, COTL_ACT_NOTIFICATION_CENTRE_TYPEINFO);
    return statics && alive(PTR(statics, COTL_ACT_NOTIFICATION_INSTANCE)) ? COTL_ACT_REASON_NONE
                                                                          : COTL_ACT_REASON_NOT_AVAILABLE;
}

// AudioManager.get_Instance creates it itself, and only if that fails does the branch throw on our
// stack (0x2C6036C). If it already exists, as it always does during play, that does not happen.
static int audio_alive(Addr main) {
    Addr statics = statics_of(main, COTL_ACT_AUDIO_MANAGER_TYPEINFO);
    return statics && alive(PTR(statics, COTL_ACT_AUDIO_INSTANCE));
}

static uint32_t extort_refusal(Addr main, Addr info) {
    if (U8(info, COTL_ACT_INFO_PAID_TITHES)) return COTL_ACT_REASON_ALREADY_EXTORTED;
    return audio_alive(main) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NOT_AVAILABLE;
}

// Submenus (tools/cotl/README.md, eating and work). As a switch, not a table: the helper has only .text.
static int meal_command(uint32_t c) {
    switch (c) {
    case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 60: case 61: case 65:
    case 66: case 67: case 68: case 69: case 70: case 71: case 72: case 84: case 87: case 105: case 112:
    case 133: case 134: case 135:
        return 1;
    default:
        return 0;
    }
}

static int drink_command(uint32_t c) {
    switch (c) {
    case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 127: case 128: case 129: case 130:
    case 177:
        return 1;
    default:
        return 0;
    }
}

// Roles the handler treats together (0x2C5C848: TooColdToWork, then set the role)
static int role_command(uint32_t c) {
    switch (c) {
    case 14: case 17: case 27: case 30: case 47: case 58: case 64: case 81: case 88: case 113: case 116:
    case 117: case 124: case 126: case 175:
        return 1;
    default:
        return 0;
    }
}

// StructureManager is in use (class constructor has run); otherwise a call would trigger it first
static int structures_ready(Addr main) {
    Addr klass = PTR(main, COTL_ACT_STRUCTURE_MANAGER_TYPEINFO);
    return ptr(klass) && U32(klass, COTL_ACT_CLASS_CCTOR_FINISHED);
}

// Number of buildings of the type (GetAllStructuresOfType(type, 0, 0)), -1 unreadable
static int32_t structure_count(Addr main, uint32_t type) {
    typedef Addr (*GetAll)(uint32_t type, uint32_t a, uint32_t b, Addr method);
    Addr list = ((GetAll)GAME_FN(main, COTL_ACT_FN_ALL_STRUCTURES))(type, 0, 0, 0);
    Addr data;
    uint32_t n;
    return list_of(list, &data, &n) ? (int32_t)n : -1;
}

// Build (0x2C5D398): without Structures_BuildSite and Structures_BuildSiteProject the handler would say
// "NoBuildingSites". The call by type counts at most as many as the game's generic one.
static uint32_t build_refusal(Addr main) {
    if (!structures_ready(main)) return COTL_ACT_REASON_NO_BUILD_SITES;
    int32_t sites = structure_count(main, COTL_ACT_STRUCTURE_BUILD_SITE);
    if (sites < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
    if (sites > 0) return COTL_ACT_REASON_NONE;
    int32_t projects = structure_count(main, COTL_ACT_STRUCTURE_BUILD_SITE_PROJECT);
    if (projects < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
    return projects > 0 ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NO_BUILD_SITES;
}

// Gifts (tools/cotl/README.md, gifts): ITEM_TYPE of the command from GiftCommands 0x2037CA0, else 0
static uint32_t gift_item(uint32_t c) {
    switch (c) {
    case 51: return 0x2B;
    case 52: return 0x2C;
    case 53: return 0x2D;
    case 54: return 0x2E;
    case 55: return 0x2F;
    case 56: return 0x30;
    case 57: return 0x31;
    case 76: return 0x7D;
    case 77: return 0x7C;
    case 78: return 0x7E;
    case 79: return 0x7B;
    case 80: return 0x7A;
    case 82: return 0x7F;
    case 119: return 0xB4;
    case 120: return 0xB5;
    case 121: return 0xB6;
    case 122: return 0xB7;
    case 123: return 0xB8;
    default: return 0;
    }
}

// ... and from GiveAnimalPetCommands 0x20395B0
static uint32_t pet_item(uint32_t c) {
    switch (c) {
    case 141: return 0xAA;
    case 142: return 0xBC;
    case 143: return 0xB2;
    case 144: return 0xBD;
    case 145: return 0xB1;
    case 146: return 0xB3;
    case 147: return 0xB0;
    default: return 0;
    }
}

// In the base inventory (first stack, like GetItemQuantity), otherwise NOT_AVAILABLE. After that the
// coroutines play sound via AudioManager.get_Instance, which first creates an instance if there is
// none.
static uint32_t stock_refusal(Addr main, uint32_t item) {
    Addr dm = data_manager(main);
    int32_t n = dm ? item_quantity(dm, item) : -1;
    if (n < 1) return COTL_ACT_REASON_NOT_AVAILABLE;
    return audio_alive(main) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NOT_AVAILABLE;
}

// GiveItemRoutine 0x2C7E640, first step (runs synchronously in StartCoroutine): with a necklace it
// throws on AllNecklaces null and says "AlreadyHaveNecklace" if the gift is a necklace. Unity
// catches an exception there, so the command would hang until the timeout.
static uint32_t gift_refusal(Addr main, Addr info, uint32_t item) {
    if (U32(info, COTL_ACT_INFO_NECKLACE)) {
        Addr statics = statics_of(main, COTL_ACT_DATAMANAGER_TYPEINFO);
        int r = statics ? has_u32(PTR(statics, COTL_ACT_DM_ALL_NECKLACES), item) : -1;
        if (r < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
        if (r) return COTL_ACT_REASON_HAS_NECKLACE;
    }
    return stock_refusal(main, item);
}

// GivePetRoutine 0x2C7FAA0: throws on DLCPets null, says "AlreadyHavePet" with one entry
static uint32_t pet_refusal(Addr main, Addr info, uint32_t item) {
    Addr data;
    uint32_t n;
    if (!list_of(PTR(info, COTL_ACT_INFO_DLC_PETS), &data, &n)) return COTL_ACT_REASON_NOT_AVAILABLE;
    if (n) return COTL_ACT_REASON_HAS_PET;
    return stock_refusal(main, item);
}

// Sub-command with its parent. With two entries the handler closes with Close(1, 1, 0) and then goes
// through the Drowsy block like Sleep and Worship. Gift and Give Pet only start their coroutine.
static uint32_t sub_command_refusal(Addr main, Addr info, uint32_t command, uint32_t parent) {
    uint32_t refusal;
    if (parent == COTL_ACT_COMMAND_GIFT && gift_item(command)) return gift_refusal(main, info, gift_item(command));
    if (parent == COTL_ACT_COMMAND_GIVE_PET && pet_item(command)) return pet_refusal(main, info, pet_item(command));
    if (parent == COTL_ACT_COMMAND_EAT && meal_command(command)) {
        // CancelTargetedMeal goes through StructureManager.AccumulateOnAllStructures
        refusal = structures_ready(main) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NOT_AVAILABLE;
    } else if (parent == COTL_ACT_COMMAND_DRINK && drink_command(command)) {
        refusal = structures_ready(main) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NOT_AVAILABLE;
    } else if (parent == COTL_ACT_COMMAND_WORK && command == COTL_ACT_COMMAND_BUILD) {
        refusal = build_refusal(main);
    } else if (parent == COTL_ACT_COMMAND_WORK && role_command(command)) {
        Addr dm = data_manager(main);
        refusal = dm ? cold_refusal(info, dm) : COTL_ACT_REASON_NOT_AVAILABLE;
    } else {
        return COTL_ACT_REASON_NOT_AVAILABLE;
    }
    return refusal ? refusal : drowsy_refusal(main, info);
}

// What the command's branch in the handler refused. The game would do it only after the walk, mostly
// with a conversation that waits for the player's A (CloseAndSpeak, which closes with Close(0, 0, 0)):
// that must not happen on a tap. Checked before the walk and once more before the call.
static uint32_t command_refusal(Addr main, Addr info, uint32_t command, uint32_t parent) {
    if (parent) return sub_command_refusal(main, info, command, parent);
    switch (command) {
    case COTL_ACT_COMMAND_BLESS:
        return U8(info, COTL_ACT_INFO_RECEIVED_BLESSING) ? COTL_ACT_REASON_ALREADY_BLESSED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_EXTORT:
        return extort_refusal(main, info);
    case COTL_ACT_COMMAND_DANCE:
        return U8(info, COTL_ACT_INFO_INSPIRED) ? COTL_ACT_REASON_ALREADY_INSPIRED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_INTIMIDATE:
        return U8(info, COTL_ACT_INFO_INTIMIDATED) ? COTL_ACT_REASON_ALREADY_INTIMIDATED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_ROMANCE:
        return U8(info, COTL_ACT_INFO_KISSED) ? COTL_ACT_REASON_ALREADY_KISSED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_PET_DOG:
        return U8(info, COTL_ACT_INFO_PET_DOG) ? COTL_ACT_REASON_ALREADY_PETTED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_BULLY:
    case COTL_ACT_COMMAND_REASSURE:
        return U8(info, COTL_ACT_INFO_SCARED_INTERACTED) ? COTL_ACT_REASON_ALREADY_INTERACTED : COTL_ACT_REASON_NONE;
    case COTL_ACT_COMMAND_BRIBE: {
        if (U8(info, COTL_ACT_INFO_BRIBED)) return COTL_ACT_REASON_ALREADY_BRIBED;
        Addr dm = data_manager(main);
        int32_t gold = dm ? gold_quantity(dm) : -1;
        if (gold < 0) return COTL_ACT_REASON_NOT_AVAILABLE;
        return gold < COTL_ACT_BRIBE_COST ? COTL_ACT_REASON_NOT_ENOUGH_GOLD : COTL_ACT_REASON_NONE;
    }
    case COTL_ACT_COMMAND_WORSHIP: {
        Addr dm = data_manager(main);
        uint32_t cold = dm ? cold_refusal(info, dm) : COTL_ACT_REASON_NOT_AVAILABLE;
        return cold ? cold : drowsy_refusal(main, info);
    }
    case COTL_ACT_COMMAND_WAKE_UP: {
        Addr dm = data_manager(main);
        return dm ? wake_up_refusal(info, dm) : COTL_ACT_REASON_NOT_AVAILABLE;
    }
    case COTL_ACT_COMMAND_SLEEP:
        return drowsy_refusal(main, info);
    case COTL_ACT_COMMAND_WORK:
    case COTL_ACT_COMMAND_EAT:
    case COTL_ACT_COMMAND_DRINK:
    case COTL_ACT_COMMAND_GIFT:     // "NoGifts"
    case COTL_ACT_COMMAND_GIVE_PET: // falls through to Unhandled
        return COTL_ACT_REASON_NOT_AVAILABLE; // on its own every parent ends in CloseAndSpeak
    case 3:  // GiveItem: no wheel offers it
    case 83: // GiveLeaderItem: ends in MMConversation.Play, which waits for the A (ruling on gifts)
    // Show/Hide: the branch 0x2C5CA64 closes with Close(0, 0, 1) and calls OnInteract on our stack;
    // replicated, the helper would call SetFollowerCostume, which looks up skins and throws in
    // Skin.AddSkin if one is missing (ruling on show/hide, fix round 1)
    case COTL_ACT_COMMAND_HIDE_NECKLACE:
    case COTL_ACT_COMMAND_SHOW_NECKLACE:
        return COTL_ACT_REASON_NOT_AVAILABLE;
    case COTL_ACT_COMMAND_REMOVE_NECKLACE:
        return U32(info, COTL_ACT_INFO_NECKLACE) ? COTL_ACT_REASON_NONE : COTL_ACT_REASON_NOT_AVAILABLE;
    case COTL_ACT_COMMAND_WINTER_NECKLACE: // Snowman and mutants: at the top, with one entry
    case COTL_ACT_COMMAND_WEIRD_NECKLACE:
        return gift_refusal(main, info, gift_item(command));
    default:
        // Sub-commands only with their parent: alone they would take a path through the handler that the wheel never takes
        if (meal_command(command) || drink_command(command) || role_command(command) ||
            command == COTL_ACT_COMMAND_BUILD || gift_item(command) || pet_item(command))
            return COTL_ACT_REASON_NOT_AVAILABLE;
        return COTL_ACT_REASON_NONE;
    }
}

// The follower can be held and the Lamb sent off: NONE, otherwise the refusal reason.
// The check that comes last calls the game. The interaction is switched off here only if the
// task reports DisablePickUpInteraction (imprisoned, on a mission, ...) or its GameObject is inactive:
// the game then offers no interaction, and HardSwapToTask would abort the task (re-notes).
// Sleepers are only for waking up; no other command holds them.
static uint32_t check_hold(Addr main, Addr player, Addr f, uint32_t command) {
    if (U8(player, COTL_ACT_PLAYER_GOTO_FLAG)) return COTL_ACT_REASON_LAMB_BUSY;
    if (!part_ok(player, COTL_ACT_PLAYER_CONTROLLER) || !part_ok(player, COTL_ACT_PLAYER_UNIT))
        return COTL_ACT_REASON_LAMB_BUSY;
    Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
    if (!alive(inter) || !U8(inter, COTL_ACT_INTERACT_INTERACTABLE) ||
        U8(inter, COTL_ACT_INTERACT_ACTIVATED))
        return COTL_ACT_REASON_FOLLOWER_LOCKED;
    // Follower.TimedAnimation is running (0x2993D30 sets 0x10): after waking up the sleep task stays
    // until the animation ends, and a second tap would otherwise run through the handler again.
    Addr fstate = PTR(f, COTL_ACT_FOLLOWER_STATE);
    if (!alive(fstate)) return COTL_ACT_REASON_NOT_AVAILABLE;
    if (U32(fstate, COTL_ACT_STATE_CURRENT) == COTL_ACT_FOLLOWER_TIMED_ACTION)
        return COTL_ACT_REASON_FOLLOWER_LOCKED;
    Addr brain = PTR(f, COTL_ACT_FOLLOWER_BRAIN);
    if (manual_task(main, PTR(brain, COTL_ACT_BRAIN_TASK))) return COTL_ACT_REASON_FOLLOWER_LOCKED;
    if (command == COTL_ACT_COMMAND_WAKE_UP) {
        if (!sleeping(main, PTR(brain, COTL_ACT_BRAIN_TASK))) return COTL_ACT_REASON_NOT_AVAILABLE;
    } else if (sleeping(main, PTR(brain, COTL_ACT_BRAIN_TASK))) {
        return COTL_ACT_REASON_FOLLOWER_LOCKED;
    }
    // OnInteract pulls daycare followers out. A tap in the list should not do that on the side.
    int daycare = in_daycare(main, U32(PTR(brain, COTL_ACT_BRAIN_INFO), COTL_ACT_INFO_ID));
    if (daycare < 0) return COTL_ACT_REASON_NOT_AVAILABLE; // building data broken, "busy" would be wrong
    if (daycare) return COTL_ACT_REASON_FOLLOWER_LOCKED;
    typedef uint64_t (*IsActive)(Addr self, Addr method);
    if (!(((IsActive)GAME_FN(main, COTL_ACT_FN_IS_ACTIVE))(inter, 0) & 1))
        return COTL_ACT_REASON_FOLLOWER_LOCKED;
    return COTL_ACT_REASON_NONE;
}

// NONE if the request may start, otherwise the refusal reason
static uint32_t check_request(Addr main, Addr player, uint32_t follower, uint32_t command, uint32_t parent,
                              uint32_t req_epoch, uint32_t epoch) {
    if (req_epoch != epoch) return COTL_ACT_REASON_STALE_SESSION;
    if (!at_base(main)) return COTL_ACT_REASON_NOT_AT_BASE;
    if (!lamb_free(main, player)) return COTL_ACT_REASON_LAMB_BUSY;
    Addr f = find_follower(main, follower);
    if (!f) return COTL_ACT_REASON_FOLLOWER_MISSING;
    if (command == COTL_ACT_COMMAND_WALK_ONLY) {
        // Walk only: locks apply to commands, not to walking there; a carried one is already there
        if (picked_up(main, f) != 0) return COTL_ACT_REASON_FOLLOWER_LOCKED;
        Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
        if ((alive(inter) && U8(inter, COTL_ACT_INTERACT_ACTIVATED)) || U8(player, COTL_ACT_PLAYER_GOTO_FLAG))
            return COTL_ACT_REASON_LAMB_BUSY;
        return COTL_ACT_REASON_NONE;
    }
    if (follower_unavailable(main, f)) return COTL_ACT_REASON_FOLLOWER_LOCKED;
    uint32_t refusal =
        command_refusal(main, PTR(PTR(f, COTL_ACT_FOLLOWER_BRAIN), COTL_ACT_BRAIN_INFO), command, parent);
    if (refusal) return refusal;
    return check_hold(main, player, f, command);
}

#define MAILBOX_U32(mailbox, off) ((volatile uint32_t *)((mailbox) + (off)))

// Write the status: reason and state first, the sequence last (release), so a reader that sees the
// sequence also sees the rest
static void publish(Addr mailbox, uint32_t seq, uint32_t state, uint32_t reason) {
    *MAILBOX_U32(mailbox, COTL_ACT_ST_REASON) = reason;
    __atomic_store_n(MAILBOX_U32(mailbox, COTL_ACT_ST_STATE), state, __ATOMIC_RELEASE);
    __atomic_store_n(MAILBOX_U32(mailbox, COTL_ACT_ST_SEQ), seq, __ATOMIC_RELEASE);
}

// The follower's task is still the one our coroutine set. When waking up it sets none
// (OnInteract: setManualControl = 0 for sleepers, 0x2C59D98): the follower counts as held as long
// as it keeps sleeping in the sleep task it had at acceptance.
static int hold_ours(Addr main, Addr mailbox, Addr f) {
    Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
    if (!alive(inter)) return 0;
    Addr brain = PTR(f, COTL_ACT_FOLLOWER_BRAIN);
    if (!ptr(brain)) return 0;
    Addr task = PTR(brain, COTL_ACT_BRAIN_TASK);
    if (*MAILBOX_U32(mailbox, COTL_ACT_RUN_COMMAND) == COTL_ACT_COMMAND_WAKE_UP)
        return task && task == *(const volatile Addr *)(mailbox + COTL_ACT_RUN_TASK) && sleeping(main, task);
    return task && task == PTR(inter, COTL_ACT_INTERACT_TASK) &&
           typed(main, task, COTL_ACT_MANUAL_TYPEINFO);
}

// The running GoToAndStop is ours: same target, no callbacks. If the game started one of its own,
// it belongs to the game, even if the target happens to be the same (the game always sets callbacks).
static int walk_ours(Addr player, Addr mailbox) {
    return U8(player, COTL_ACT_PLAYER_GOTO_FLAG) && !PTR(player, COTL_ACT_PLAYER_GOTO_CALLBACK) &&
           !PTR(player, COTL_ACT_PLAYER_ABORT_CALLBACK) &&
           U32(player, COTL_ACT_PLAYER_GOTO_TARGET) == *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_X) &&
           U32(player, COTL_ACT_PLAYER_GOTO_TARGET + 4) == *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_Y) &&
           U32(player, COTL_ACT_PLAYER_GOTO_TARGET + 8) == *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_Z);
}

// Only after the coroutine has cached: ResetFollower writes its copies back. f also comes from
// RUN_OBJECT and so may be anything. If the game runs an interaction of its own (Activated),
// the follower including its animator belongs to it.
// If the task is still ours, ResetFollower releases the follower, even a disabled one
// (otherwise it would stay locked after OnEnable until reload). Otherwise only give back the
// animator, which the coroutine switches off without checking again: ResetFollower would finish
// the game's task (0x2C5C08C) and overwrite the death animation of a dying follower.
static void release_follower(Addr main, Addr mailbox, Addr f) {
    if (!typed(main, f, COTL_ACT_FOLLOWER_TYPEINFO) || !alive(f)) return;
    Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
    if (!alive(inter) || U8(inter, COTL_ACT_INTERACT_ACTIVATED)) return;
    if (hold_ours(main, mailbox, f) && !U8(f, COTL_ACT_FOLLOWER_DYING)) {
        typedef void (*Reset)(Addr self, Addr method);
        ((Reset)GAME_FN(main, COTL_ACT_FN_RESET_FOLLOWER))(inter, 0);
        return;
    }
    Addr animator = PTR(inter, COTL_ACT_INTERACT_ANIMATOR);
    if (alive(animator)) {
        typedef void (*SetEnabled)(Addr self, uint32_t value, Addr method);
        ((SetEnabled)GAME_FN(main, COTL_ACT_FN_SET_ENABLED))(animator, 1, 0);
    }
}

// End our walk. AbortGoTo always writes Idle (0x1CFC1FC). At the hook the state during the walk
// is otherwise Moving (PlayerFarming.Update only forces it afterwards), so any other state comes
// from whoever took over the Lamb, such as the opener of a conversation with InActive. Idle on
// top of that would release the stick (PlayerController reads it only outside InActive,
// 0x1CD8970). So set <LockStateChanges> for the call: set_CURRENT_STATE then returns at once
// (0x1ED6BF4), without a delegate.
static void stop_lamb(Addr player, Addr main) {
    typedef void (*Abort)(Addr self, uint32_t invoke_callback, Addr method);
    Abort abort_walk = (Abort)GAME_FN(main, COTL_ACT_FN_ABORT_GO_TO);
    Addr state = PTR(player, COTL_ACT_PLAYER_STATE);
    uint32_t s = alive(state) ? U32(state, COTL_ACT_STATE_CURRENT) : 0;
    if (s == 0 || s == 1) {
        abort_walk(player, 0, 0);
        return;
    }
    volatile uint8_t *lock = (volatile uint8_t *)(state + COTL_ACT_STATE_LOCK);
    uint8_t before = *lock;
    *lock = 1;
    abort_walk(player, 0, 0);
    *lock = before;
}

// End the run: first the status (if a game function calls the update again, the run is already
// over), then stop the Lamb if the walk is ours, then release the follower.
static void end_run(Addr player, Addr mailbox, Addr main, Addr f, uint32_t seq, uint32_t state,
                    uint32_t reason) {
    int stop = *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) == PHASE_WALK && walk_ours(player, mailbox);
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_NONE;
    publish(mailbox, seq, state, reason);
    if (stop) stop_lamb(player, main);
    if (*MAILBOX_U32(mailbox, COTL_ACT_RUN_COMMAND) != COTL_ACT_COMMAND_WALK_ONLY) // nothing held
        release_follower(main, mailbox, f);
}

// Class of the command array. The handler only reads length and data and does not check the type;
// FollowerCommands[] has no TypeInfo slot, int[] has the same shape. Unresolved, the slot holds a
// token: then resolve it as the game itself does. 0: cannot be obtained.
static Addr command_array_class(Addr main) {
    Addr klass = PTR(main, COTL_ACT_INT_ARRAY_TYPEINFO);
    if (ptr(klass)) return klass;
    typedef void (*InitMetadata)(Addr slot);
    ((InitMetadata)GAME_FN(main, COTL_ACT_FN_INIT_METADATA))(main + COTL_ACT_INT_ARRAY_TYPEINFO);
    klass = PTR(main, COTL_ACT_INT_ARRAY_TYPEINFO);
    return ptr(klass) ? klass : 0;
}

// The component's GameObject is active. Only StartCoroutine depends on that (0xB146A4, otherwise it
// logs and starts nothing); the component itself being off is the normal case during the hold: the
// task switch to ManualControl turns it off (Follower.OnTaskChanged 0x298C758, in the game too).
static int game_object_active(Addr main, Addr component) {
    typedef Addr (*GetGameObject)(Addr self, Addr method);
    typedef uint64_t (*ActiveInHierarchy)(Addr self, Addr method);
    Addr go = ((GetGameObject)GAME_FN(main, COTL_ACT_FN_GET_GAME_OBJECT))(component, 0);
    return ptr(go) && (((ActiveInHierarchy)GAME_FN(main, COTL_ACT_FN_GO_ACTIVE))(go, 0) & 1);
}

// The command runs through the game's handler, without OnInteract and wheel: otherwise level-up,
// quests and sin would stay in front of it. Pause and OnConversationNew are otherwise done by
// <OnInteract>b__4 (0x2C6E0A0) before the wheel; Close undoes both. NONE means: running. Otherwise
// nothing has happened and the run ends. An exception would arrive at us, so first check
// everything the calls throw on (re-notes).
static uint32_t run_command(Addr mailbox, Addr main, Addr f, uint32_t seq) {
    uint32_t command = *MAILBOX_U32(mailbox, COTL_ACT_RUN_COMMAND);
    uint32_t parent = *MAILBOX_U32(mailbox, COTL_ACT_RUN_PARENT);
    Addr brain = PTR(f, COTL_ACT_FOLLOWER_BRAIN);
    uint32_t refusal = command_refusal(main, PTR(brain, COTL_ACT_BRAIN_INFO), command, parent);
    if (refusal) return refusal;
    // The handler's getters (Inspired, Bribed, SkinName, ...) read through these two and throw on null
    Addr brain_info = PTR(brain, COTL_ACT_BRAIN_BRAIN_INFO);
    Addr stats = PTR(brain, COTL_ACT_BRAIN_STATS);
    if (!ptr(brain_info) || !ptr(PTR(brain_info, COTL_ACT_BRAIN_INFO_INFO)) || !ptr(stats) ||
        !ptr(PTR(stats, COTL_ACT_STATS_INFO)))
        return COTL_ACT_REASON_NOT_AVAILABLE;
    // The wake-up branch calls TimedAnimation: it throws on null Spine or State (0x2993D50, 0x2993D64)
    // and runs a pending callback on our stack (0x2993DA0) if it is already running
    Addr fstate = PTR(f, COTL_ACT_FOLLOWER_STATE);
    if (!alive(fstate) || !alive(PTR(f, COTL_ACT_FOLLOWER_SPINE))) return COTL_ACT_REASON_NOT_AVAILABLE;
    if (command == COTL_ACT_COMMAND_WAKE_UP &&
        U32(fstate, COTL_ACT_STATE_CURRENT) == COTL_ACT_FOLLOWER_TIMED_ACTION)
        return COTL_ACT_REASON_FOLLOWER_LOCKED;
    Addr inter = PTR(f, COTL_ACT_FOLLOWER_INTERACTION);
    Addr gm_statics = statics_of(main, COTL_ACT_GAMEMANAGER_TYPEINFO);
    Addr gm = gm_statics ? PTR(gm_statics, COTL_ACT_GAMEMANAGER_INSTANCE) : 0;
    if (!alive(PTR(inter, COTL_ACT_INTERACT_LISTENER)) || PTR(inter, COTL_ACT_INTERACT_FOLLOWER) != f ||
        !alive(gm) || !game_object_active(main, inter))
        return COTL_ACT_REASON_NOT_AVAILABLE;
    Addr klass = command_array_class(main);
    if (!klass) return COTL_ACT_REASON_NOT_AVAILABLE;

    *(volatile Addr *)(mailbox + COTL_ACT_RUN_TASK) = PTR(inter, COTL_ACT_INTERACT_TASK);
    *(volatile uint8_t *)(inter + COTL_ACT_INTERACT_PREVENT_CLOSE) = 0; // OnInteract 0x2C593D8
    // Blocks the player's A (OnInteract 0x2C5939C) and is the signal for the end: the game itself
    // never sets it, Close clears it (0x2C606E0)
    *(volatile uint8_t *)(inter + COTL_ACT_INTERACT_ACTIVATED) = 1;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) = 0;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_RUN;
    publish(mailbox, seq, COTL_ACT_STATE_RUNNING, COTL_ACT_REASON_NONE);

    typedef Addr (*ArrayNew)(Addr klass, uint32_t length);
    // Like the wheel: the chosen entry first, then its parent (OnFollowerCommandFinalized reads [0])
    Addr array = ((ArrayNew)GAME_FN(main, COTL_ACT_FN_ARRAY_NEW))(klass, parent ? 2 : 1);
    *(volatile uint32_t *)(array + ARRAY_DATA) = command;
    if (parent) *(volatile uint32_t *)(array + ARRAY_DATA + 4) = parent;
    typedef void (*Pause)(Addr method);
    ((Pause)GAME_FN(main, COTL_ACT_FN_SIM_PAUSE))(0);
    typedef void (*ConversationNew)(Addr self, uint32_t set_inactive, uint32_t snap, Addr player_arg,
                                    Addr method);
    ((ConversationNew)GAME_FN(main, COTL_ACT_FN_CONVERSATION_NEW))(gm, 1, 1, 0, 0);
    typedef void (*Finalized)(Addr self, Addr commands, Addr method);
    ((Finalized)GAME_FN(main, COTL_ACT_FN_COMMAND_FINALIZED))(inter, array, 0);
    return COTL_ACT_REASON_NONE;
}

// Arrival: the Lamb is already Idle (IdleOnEnd), the follower still held
static void walk_arrived(Addr player, Addr mailbox, Addr main, Addr f, uint32_t seq) {
    uint32_t reason = run_command(mailbox, main, f, seq);
    if (reason) end_run(player, mailbox, main, f, seq, COTL_ACT_STATE_CANCELLED, reason);
}

// Close(1, 1, 0): pause off, Lamb Idle, follower free, conversation closed; reopens nothing
static void close_interaction(Addr main, Addr inter) {
    typedef void (*Close)(Addr self, uint32_t reset, uint32_t unpause, uint32_t reopen, Addr method);
    ((Close)GAME_FN(main, COTL_ACT_FN_CLOSE))(inter, 1, 1, 0, 0);
}

// Close() without arguments (normal case after blessing) restarted OnInteract: this walk otherwise
// leads to the wheel. Recognised by the abort callback b__5 with this interaction.
static int reopen_walk(Addr main, Addr player, Addr inter) {
    if (!U8(player, COTL_ACT_PLAYER_GOTO_FLAG)) return 0;
    Addr abort_cb = PTR(player, COTL_ACT_PLAYER_ABORT_CALLBACK);
    if (!ptr(abort_cb) ||
        PTR(abort_cb, COTL_ACT_DELEGATE_METHOD_PTR) != GAME_FN(main, COTL_ACT_FN_REOPEN_ABORT))
        return 0;
    Addr closure = PTR(abort_cb, COTL_ACT_DELEGATE_TARGET);
    return ptr(closure) && PTR(closure, COTL_ACT_CLOSURE_THIS) == inter;
}

// End of the command, before any further call: if a game function calls the update again, the
// run is already over (like end_run, but here the follower belongs to Close)
static void finish_command(Addr mailbox, uint32_t seq, uint32_t state, uint32_t reason) {
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_NONE;
    publish(mailbox, seq, state, reason);
}

// The command runs, once per frame. The game's coroutine cannot be aborted; epoch, stick and
// CANCEL_SEQ do not count here, the game releases both itself at the end (sim paused, Lamb
// InActive).
static void command_tick(Addr player, Addr mailbox, Addr main, uint32_t epoch, uint32_t seq,
                         uint32_t phase, uint32_t frames) {
    Addr f = *(const volatile Addr *)(mailbox + COTL_ACT_RUN_OBJECT);
    Addr inter = typed(main, f, COTL_ACT_FOLLOWER_TYPEINFO) && alive(f) ? PTR(f, COTL_ACT_FOLLOWER_INTERACTION) : 0;
    if (!alive(inter)) {
        // With the object the coroutine is gone, Close would never come. The pause is static and survives
        // the scene; everything else (conversation, camera) hangs on objects that may no longer exist.
        finish_command(mailbox, seq, COTL_ACT_STATE_CANCELLED,
                       *MAILBOX_U32(mailbox, COTL_ACT_RUN_EPOCH) != epoch ? COTL_ACT_REASON_STALE_SESSION
                                                                          : COTL_ACT_REASON_FOLLOWER_MISSING);
        typedef void (*UnPause)(Addr method);
        ((UnPause)GAME_FN(main, COTL_ACT_FN_SIM_UNPAUSE))(0);
        return;
    }
    if (phase == PHASE_RUN) {
        if (U8(inter, COTL_ACT_INTERACT_ACTIVATED)) {
            if (frames >= kRunTimeoutFrames) {
                finish_command(mailbox, seq, COTL_ACT_STATE_CANCELLED, COTL_ACT_REASON_TIMEOUT);
                close_interaction(main, inter);
            }
            return;
        }
        // Close ran. Without a walk to the wheel everything is free (level-up: Close(1, 1, 0)), or the
        // game continues a conversation of its own accord, which belongs to the player.
        if (!reopen_walk(main, player, inter)) {
            finish_command(mailbox, seq, COTL_ACT_STATE_DONE, COTL_ACT_REASON_NONE);
            return;
        }
        // The callback b__4 opened the wheel. The hook runs before the walk's step, and the Lamb stands
        // near the target: remove it now, EndGoToAndStop skips null (0x1CFCFCC)
        *(volatile Addr *)(player + COTL_ACT_PLAYER_GOTO_CALLBACK) = 0;
        *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) = 0;
        frames = 0;
        phase = PHASE_REOPEN_WAIT;
        *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = phase;
    }
    // ResetFollower before caching the new coroutine would be wrong (like PHASE_HOLD_*): first see the
    // new task, then wait one frame. Close then runs as b__5 would do it.
    if (phase == PHASE_REOPEN_WAIT) {
        Addr task = PTR(inter, COTL_ACT_INTERACT_TASK);
        if ((task != *(const volatile Addr *)(mailbox + COTL_ACT_RUN_TASK) && hold_ours(main, mailbox, f)) ||
            frames >= kHoldTimeoutFrames)
            *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_REOPEN_SETTLE;
        return;
    }
    finish_command(mailbox, seq, COTL_ACT_STATE_DONE, COTL_ACT_REASON_NONE);
    if (U8(player, COTL_ACT_PLAYER_GOTO_FLAG)) {
        typedef void (*Abort)(Addr self, uint32_t invoke_callback, Addr method);
        ((Abort)GAME_FN(main, COTL_ACT_FN_ABORT_GO_TO))(player, 0, 0);
    }
    close_interaction(main, inter);
}

// Position of a live component as float bits
static void position_of(Addr main, Addr component, uint32_t *out) {
    typedef Addr (*GetTransform)(Addr self, Addr method);
    Addr transform = ((GetTransform)GAME_FN(main, COTL_ACT_FN_GET_TRANSFORM))(component, 0);
    cotl_call_vec3(GAME_FN(main, COTL_ACT_FN_GET_POSITION), transform, out);
}

// Once per request, after holding: compute the target and walk. As in the game, the target is
// 2 units beside the follower, on the side facing the Lamb, not on it.
static void start_walk(Addr player, Addr mailbox, Addr main, Addr f) {
    uint32_t from[3], to[3];
    position_of(main, f, to);
    position_of(main, player, from);
    uint32_t x = cotl_float_add(to[0], cotl_float_ge(to[0], from[0]) ? FLOAT_MINUS_2 : FLOAT_PLUS_2);
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_X) = x;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_Y) = to[1];
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_Z) = to[2];
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) = 0;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_WALK;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_AXIS_X) = 0xFFFFFFFFu;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_AXIS_Y) = 0xFFFFFFFFu;
    typedef Addr (*GetGameObject)(Addr self, Addr method);
    Addr look = ((GetGameObject)GAME_FN(main, COTL_ACT_FN_GET_GAME_OBJECT))(f, 0);
    cotl_call_go_to(GAME_FN(main, COTL_ACT_FN_GO_TO), player, x, to[1], to[2], look, FLOAT_30);
}

// Is the main player's stick deflected? During the walk PlayerController skips the input and zeroes
// xDir/yDir; so the same calls as the game makes every frame in normal operation.
static int stick_moved(Addr player, Addr mailbox, Addr main) {
    typedef Addr (*GetGameplay)(Addr method);
    Addr source = ((GetGameplay)GAME_FN(main, COTL_ACT_FN_GAMEPLAY))(0);
    if (!ptr(source)) return 0;
    uint32_t h = cotl_call_float(GAME_FN(main, COTL_ACT_FN_AXIS_H), source, player, 0);
    uint32_t v = cotl_call_float(GAME_FN(main, COTL_ACT_FN_AXIS_V), source, player, 0);
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_AXIS_X) = h;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_AXIS_Y) = v;
    return (h & 0x7FFFFFFFu) > STICK_LIMIT || (v & 0x7FFFFFFFu) > STICK_LIMIT;
}

// The game has put the Lamb in InActive. Without aborting, PlayerFarming.Update would set Moving
// over it right after the hook.
static int lamb_taken(Addr player) {
    Addr state = PTR(player, COTL_ACT_PLAYER_STATE);
    return alive(state) && U32(state, COTL_ACT_STATE_CURRENT) == COTL_ACT_LAMB_INACTIVE;
}

// The game ended the walk (EndGoToAndStop): also on timeout or if the Lamb is stuck.
// Arrived only if the last position is at the target, at most 1.5 units per axis. The game itself
// checks no range (its callback runs after every walk end); in the real run the walk ended about
// 1 unit before the target, which itself lies 2 beside the follower.
static int near_target(Addr player, Addr mailbox) {
    return cotl_float_near(U32(player, COTL_ACT_PLAYER_LAST_POSITION),
                           *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_X), FLOAT_1_5) &&
           cotl_float_near(U32(player, COTL_ACT_PLAYER_LAST_POSITION + 4),
                           *MAILBOX_U32(mailbox, COTL_ACT_RUN_TARGET_Y), FLOAT_1_5);
}

// The running run, once per frame. The host changes the epoch on pause and resume of the
// emulator and on debugger halts (core.cpp); game memory and the scene stand still meanwhile.
// So the abort runs like any other; without calls the follower would stay held.
static void run_tick(Addr player, Addr mailbox, Addr main, uint32_t epoch, uint32_t seq) {
    int stale = *MAILBOX_U32(mailbox, COTL_ACT_RUN_EPOCH) != epoch;
    Addr f = find_follower(main, *MAILBOX_U32(mailbox, COTL_ACT_RUN_FOLLOWER));
    // Vanished does not mean gone: Follower.OnDisable and _dying keep the object alive
    Addr held = f ? f : *(const volatile Addr *)(mailbox + COTL_ACT_RUN_OBJECT);
    uint32_t phase = *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE);
    uint32_t frames = *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) + 1;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) = frames;
    uint32_t reason = COTL_ACT_REASON_NONE;

    if (phase >= PHASE_RUN) {
        command_tick(player, mailbox, main, epoch, seq, phase, frames);
        return;
    }

    if (phase == PHASE_HOLD_WAIT) {
        // No abort yet, not even on a changed epoch: ResetFollower would be wrong before caching, the
        // coroutine might still set the task after a release and switch the animator off afterwards.
        // Only a destroyed follower ends it immediately.
        int live = typed(main, held, COTL_ACT_FOLLOWER_TYPEINFO) && alive(held);
        if (live && hold_ours(main, mailbox, held))
            *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_HOLD_SETTLE;
        else if (!live || frames >= kHoldTimeoutFrames)
            end_run(player, mailbox, main, held, seq, COTL_ACT_STATE_CANCELLED,
                    stale ? COTL_ACT_REASON_STALE_SESSION
                          : f ? COTL_ACT_REASON_TIMEOUT : COTL_ACT_REASON_FOLLOWER_MISSING);
        return;
    }
    // From here on the coroutine has cached: the task was visible at the last hook, its second step
    // ran at the end of that frame
    if (stale || !f) {
        end_run(player, mailbox, main, held, seq, COTL_ACT_STATE_CANCELLED,
                stale ? COTL_ACT_REASON_STALE_SESSION : COTL_ACT_REASON_FOLLOWER_MISSING);
        return;
    }
    if (phase == PHASE_HOLD_SETTLE) {
        if (!hold_ours(main, mailbox, f) || follower_unavailable(main, f))
            reason = COTL_ACT_REASON_FOLLOWER_LOCKED;
        else if (U8(PTR(f, COTL_ACT_FOLLOWER_INTERACTION), COTL_ACT_INTERACT_ACTIVATED) ||
                 !lamb_free(main, player) || U8(player, COTL_ACT_PLAYER_GOTO_FLAG))
            reason = COTL_ACT_REASON_LAMB_BUSY;
        if (reason)
            end_run(player, mailbox, main, f, seq, COTL_ACT_STATE_CANCELLED, reason);
        else
            start_walk(player, mailbox, main, f);
        return;
    }

    // In the walk. The state check of lamb_free does not apply here: the game sets Moving itself.
    int flag = U8(player, COTL_ACT_PLAYER_GOTO_FLAG);
    int ours = walk_ours(player, mailbox);
    int walk_only = *MAILBOX_U32(mailbox, COTL_ACT_RUN_COMMAND) == COTL_ACT_COMMAND_WALK_ONLY;
    if (walk_only ? picked_up(main, f) != 0 : follower_unavailable(main, f) || !hold_ours(main, mailbox, f))
        reason = COTL_ACT_REASON_FOLLOWER_LOCKED; // locked, carried, other task
    else if (U8(PTR(f, COTL_ACT_FOLLOWER_INTERACTION), COTL_ACT_INTERACT_ACTIVATED) ||
             (flag && !ours) || game_busy(main) || (flag && lamb_taken(player)))
        reason = COTL_ACT_REASON_LAMB_BUSY; // interaction, foreign walk, menu, conversation, scene change
    else if (!flag) {
        if (near_target(player, mailbox) && walk_only)
            end_run(player, mailbox, main, f, seq, COTL_ACT_STATE_DONE, COTL_ACT_REASON_NONE);
        else if (near_target(player, mailbox))
            walk_arrived(player, mailbox, main, f, seq);
        else
            end_run(player, mailbox, main, f, seq, COTL_ACT_STATE_CANCELLED, COTL_ACT_REASON_TIMEOUT);
        return;
    } else if (*MAILBOX_U32(mailbox, COTL_ACT_CANCEL_SEQ) == seq || stick_moved(player, mailbox, main))
        reason = COTL_ACT_REASON_PLAYER_MOVED;
    else if (frames >= kWalkTimeoutFrames)
        reason = COTL_ACT_REASON_TIMEOUT;
    if (reason) end_run(player, mailbox, main, f, seq, COTL_ACT_STATE_CANCELLED, reason);
}

static void step(Addr player, Addr mailbox, Addr main) {
    uint32_t epoch = __atomic_load_n(MAILBOX_U32(mailbox, COTL_ACT_EPOCH), __ATOMIC_ACQUIRE);
    uint32_t state = *MAILBOX_U32(mailbox, COTL_ACT_ST_STATE);
    uint32_t st_seq = *MAILBOX_U32(mailbox, COTL_ACT_ST_SEQ);

    // If an action is running, it only continues while session and follower match. The check uses
    // its own copies, since a second request overwrites REQ_*.
    if (state == COTL_ACT_STATE_WALKING || state == COTL_ACT_STATE_RUNNING) {
        run_tick(player, mailbox, main, epoch, st_seq);
        state = *MAILBOX_U32(mailbox, COTL_ACT_ST_STATE);
    }

    // New request? Sequence 0 does not exist; the same sequence is processed only once, however it
    // ended (accepted/refused: ST_SEQ, refused because busy: ST_BUSY_SEQ)
    uint32_t seq = __atomic_load_n(MAILBOX_U32(mailbox, COTL_ACT_REQ_SEQ), __ATOMIC_ACQUIRE);
    if (!seq || seq == st_seq || seq == *MAILBOX_U32(mailbox, COTL_ACT_ST_BUSY_SEQ)) return;
    uint32_t follower = *MAILBOX_U32(mailbox, COTL_ACT_REQ_FOLLOWER);
    uint32_t req_epoch = *MAILBOX_U32(mailbox, COTL_ACT_REQ_EPOCH);
    // If the module wrote a new sequence meanwhile, what was read may be mixed. The module keeps a
    // rule for this: no REQ_* field is written before the last sequence is acknowledged (ST_SEQ or
    // ST_BUSY_SEQ == sequence), and REQ_SEQ comes last. So a sequence the helper is currently
    // reading does not change; the check only catches a rule violation.
    if (__atomic_load_n(MAILBOX_U32(mailbox, COTL_ACT_REQ_SEQ), __ATOMIC_ACQUIRE) != seq) return;

    if (state == COTL_ACT_STATE_WALKING || state == COTL_ACT_STATE_RUNNING) {
        __atomic_store_n(MAILBOX_U32(mailbox, COTL_ACT_ST_BUSY_SEQ), seq, __ATOMIC_RELEASE);
        return;
    }
    uint32_t command = *MAILBOX_U32(mailbox, COTL_ACT_REQ_COMMAND);
    uint32_t parent = *MAILBOX_U32(mailbox, COTL_ACT_REQ_PARENT);
    uint32_t reason = check_request(main, player, follower, command, parent, req_epoch, epoch);
    if (reason) {
        publish(mailbox, seq, COTL_ACT_STATE_REFUSED, reason);
        return;
    }
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_FOLLOWER) = follower;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_EPOCH) = req_epoch;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_COMMAND) = command;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PARENT) = parent;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_FRAMES) = 0;
    *MAILBOX_U32(mailbox, COTL_ACT_RUN_PHASE) = PHASE_HOLD_WAIT;
    Addr held = find_follower(main, follower);
    *(volatile Addr *)(mailbox + COTL_ACT_RUN_OBJECT) = held;
    publish(mailbox, seq, COTL_ACT_STATE_WALKING, COTL_ACT_REASON_NONE);
    if (command == COTL_ACT_COMMAND_WALK_ONLY) { // without holding, go right away
        start_walk(player, mailbox, main, held);
        return;
    }

    // Hold like the game, once per request. The coroutine reads state and _playerFarming, which
    // Interaction.OnInteract would otherwise set; we do not want its other effects (prompts, events).
    Addr inter = PTR(held, COTL_ACT_FOLLOWER_INTERACTION);
    *(volatile Addr *)(inter + COTL_ACT_INTERACT_STATE) = PTR(player, COTL_ACT_PLAYER_STATE);
    *(volatile Addr *)(inter + COTL_ACT_INTERACT_PLAYER) = player;
    // Like OnInteract, while the task is still the old one: the game reads it at the end of the command
    typedef uint32_t (*TaskType)(Addr self, Addr method);
    *(volatile uint32_t *)(inter + COTL_ACT_INTERACT_PREV_TASK) =
        ((TaskType)GAME_FN(main, COTL_ACT_FN_CURRENT_TASK_TYPE))(PTR(held, COTL_ACT_FOLLOWER_BRAIN), 0);
    typedef void (*CacheAndSet)(Addr self, Addr attention_task, uint32_t set_manual, Addr method);
    int wake_up = command == COTL_ACT_COMMAND_WAKE_UP;
    if (wake_up)
        *(volatile Addr *)(mailbox + COTL_ACT_RUN_TASK) = PTR(PTR(held, COTL_ACT_FOLLOWER_BRAIN), COTL_ACT_BRAIN_TASK);
    ((CacheAndSet)GAME_FN(main, COTL_ACT_FN_CACHE_AND_SET))(inter, 0, wake_up ? 0 : 1, 0);
}

void cotl_action_tick(Addr self, Addr mailbox, Addr main) {
    // PlayerFarming.Update runs once per player; only the main player counts
    if (!self || self != main_player(main)) return;
    volatile uint32_t *heartbeat = (volatile uint32_t *)(mailbox + COTL_ACT_HEARTBEAT);
    __atomic_store_n(heartbeat, __atomic_load_n(heartbeat, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
    step(self, mailbox, main);
}
