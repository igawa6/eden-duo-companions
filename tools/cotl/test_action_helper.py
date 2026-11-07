#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 AlterDigga
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the action helper: plan shape, hook patch and the behaviour of the guest code under
Unicorn (heartbeat, request checks, walk to the follower against stubs of the game functions).

  PYTHONPATH=~/cotl-deps/py/unicorn python3 tools/cotl/test_action_helper.py

Pure tests run on a fake image; only the comparison with the real main (work/exefs/main)
is skipped when it is missing.
"""
import json
import os
import re
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, HERE)

import gen_action_helper as gen  # noqa: E402  (fails as long as the generator is missing)

try:
    from unicorn import (UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_ARM, Uc)
    from unicorn import arm64_const as A
except ImportError as e:  # pragma: no cover
    # Without Unicorn the guest tests would not run; that must not count as a green run
    raise SystemExit("unicorn is missing (tools/setup_wsl.sh; PYTHONPATH=~/cotl-deps/py/unicorn): %s" % e)

HOOK = 0x1CFDCF0
BUILD = "316A4D4881A9F1E077A07262E917917C" + "00" * 16
PLAN_PATH = os.path.join(ROOT, "packages", "CultOfTheLamb", "dualscreen", "guest", BUILD,
                         "load-plan.json")
REAL_MAIN = os.environ.get("COTL_MAIN", os.path.join(ROOT, "work", "exefs", "main"))
ABI_H = os.path.join(ROOT, "native", "modules", "cotl_action_abi.h")

# From the brief: mailbox offsets
ABI_OFFSETS = {
    "HEARTBEAT": 0x00, "EPOCH": 0x04, "REQ_SEQ": 0x08, "REQ_FOLLOWER": 0x0C, "REQ_COMMAND": 0x10,
    "REQ_EPOCH": 0x14, "CANCEL_SEQ": 0x18, "ST_SEQ": 0x20, "ST_STATE": 0x24, "ST_REASON": 0x28,
    "Q_FOLLOWER": 0x30, "Q_SEQ": 0x34, "Q_ACK": 0x38, "Q_COUNT": 0x3C, "Q_COMMANDS": 0x40,
    "Q_AVAILABLE": 0x140,
}
# Additions of stage 2: status "refused because of a running action" and the helper's run
ABI_OFFSETS_STAGE2 = {"ST_BUSY_SEQ": 0x2C, "RUN_FOLLOWER": 0x150, "RUN_EPOCH": 0x154,
                      "RUN_COMMAND": 0x158, "RUN_PHASE": 0x15C, "RUN_FRAMES": 0x160,
                      "RUN_TARGET_X": 0x164, "RUN_TARGET_Y": 0x168, "RUN_TARGET_Z": 0x16C,
                      "RUN_AXIS_X": 0x170, "RUN_AXIS_Y": 0x174, "RUN_OBJECT": 0x178, "RUN_TASK": 0x180,
                      "REQ_PARENT": 0x1C, "RUN_PARENT": 0x188}
ABI_STATES = ["IDLE", "WALKING", "RUNNING", "DONE", "REFUSED", "CANCELLED"]
ABI_REASONS = ["NONE", "NOT_AT_BASE", "LAMB_BUSY", "FOLLOWER_MISSING", "FOLLOWER_LOCKED",
               "NOT_AVAILABLE", "STALE_SESSION", "TIMEOUT", "PLAYER_MOVED", "ALREADY_BLESSED",
               "ALREADY_INSPIRED", "ALREADY_INTIMIDATED", "ALREADY_BRIBED", "NOT_ENOUGH_GOLD",
               "ALREADY_KISSED", "ALREADY_PETTED", "ALREADY_INTERACTED", "SLEEPS_UNTIL_SEASON",
               "TOO_COLD", "ALREADY_EXTORTED", "NO_BUILD_SITES", "NO_MEALS", "NO_DRINKS", "FASTING",
               "CANT_WORK", "NO_GIFTS", "HAS_NECKLACE", "HAS_PET"]

# Fake memory layout for Unicorn
MAIN = 0x10000000
TYPEINFO = MAIN + 0x713FE88
KLASS = 0x50000000
STATICS = 0x50010000
PLAYER = 0x60000000
OTHER = 0x60001000
CODE = 0x18000000  # as in the host, close to main (b only reaches +-128 MiB)
MAILBOX = 0x19000000
STACK = 0x1A000000
HEAP = 0x70000000
HEAP_SIZE = 0x100000
# Second heap above 4 GiB: the Switch heap lives there, pointers have their upper 32 bits set
HIGH_HEAP = 0x1_2000_0000
HIGH_HEAP_SIZE = 0x10000

# Game facts (dump.cs / script.json, build 316A4D48...), recorded here independently of the helper
TI = {"PF": 0x713FE88, "FOLLOWER": 0x713C060, "BRAIN": 0x713C068, "INFO": 0x713C098,
      "DM": 0x713AC50, "FLOATING": 0x713C390, "UIMENU": 0x7142908, "GM": 0x713C8A0,
      "MMCONV": 0x713EBD8, "MMTRANS": 0x713EBF0, "MANUAL": 0x713C478, "MANUAL_HARD": 0x713C3D8,
      "MANUAL_LIGHT": 0x713C468, "MANUAL_ENFORCING": 0x713C348, "SLEEP": 0x713C5B0,
      "SLEEP_BED_REST": 0x713C5B8, "STRUCTURES": 0x7141768, "INT_ARRAY": 0x7138528,
      "NOTIFICATION": 0x713F5C8, "AUDIO": 0x7139908}
# Game functions the helper may call (name in script.json, RVA). Anything else in main
# makes run() fail.
GAME_FUNCS = {
    "cache_and_set": ("interaction_FollowerInteraction$$CacheAndSetFollower", 0x2C5A570),
    "reset_follower": ("interaction_FollowerInteraction$$ResetFollower", 0x2C5BE40),
    "go_to": ("PlayerFarming$$GoToAndStop", 0x1CFBAA0),
    "abort_go_to": ("PlayerFarming$$AbortGoTo", 0x1CFC150),
    "get_game_object": ("UnityEngine.Component$$get_gameObject", 0x531DA60),
    "get_transform": ("UnityEngine.Component$$get_transform", 0x531DA10),
    "get_position": ("UnityEngine.Transform$$get_position", 0x5333280),
    "is_active": ("UnityEngine.Behaviour$$get_isActiveAndEnabled", 0x531CE80),
    "go_active": ("UnityEngine.GameObject$$get_activeInHierarchy", 0x53221D0),
    "set_enabled": ("UnityEngine.Behaviour$$set_enabled", 0x531CE10),
    "sleep_is_awake": ("FollowerTask_Sleep$$get_isAwake", 0x2A3A6C0),
    "gameplay": ("InputManager$$get_Gameplay", 0x2B3E420),
    "axis_h": ("RewiredGameplayInputSource$$GetHorizontalAxis", 0x2B3B900),
    "axis_v": ("RewiredGameplayInputSource$$GetVerticalAxis", 0x2B3BC70),
    "command_finalized": ("interaction_FollowerInteraction$$OnFollowerCommandFinalized", 0x2C5C200),
    "close": ("interaction_FollowerInteraction$$Close", 0x2C60370),
    "conversation_new": ("GameManager$$OnConversationNew", 0x2A1B090),
    "sim_pause": ("SimulationManager$$Pause", 0x1EB0040),
    "sim_unpause": ("SimulationManager$$UnPause", 0x1EB00E0),
    "current_task_type": ("FollowerBrain$$get_CurrentTaskType", 0x299B6F0),
    "all_structures": ("StructureManager$$GetAllStructuresOfType", 0x1F2FD80),
}
# No name in the dump: Il2Cpp runtime; RealImageTests pins the instructions
RUNTIME_FUNCS = {"array_new": 0x4806F0, "init_metadata": 0x4805F0}
# Fields from dump.cs
PF_GOTO_FLAG, PF_GOTO_CB, PF_ABORT_CB, PF_TARGET, PF_LAST_POS = 0x2E8, 0x2F8, 0x300, 0x310, 0x320
PF_IDLE_ON_END = 0x2E9
STATE_IDLE, STATE_MOVING, STATE_INACTIVE = 0, 1, 13  # StateMachine.State
STATE_TIMED_ACTION = 16  # StateMachine.State.TimedAction
FOLLOWER_STATE, FOLLOWER_SPINE = 0x98, 0xA0  # Follower.State, Follower.Spine
TRAIT_DROWSY = 0x44
NOTIFICATION_INSTANCE = 0x8  # NotificationCentre.Instance (static)
AUDIO_INSTANCE = 0x0  # AudioManager._instance (static)
INTER_STATE, INTER_PLAYER, INTER_INTERACTABLE, INTER_ACTIVATED, INTER_TASK = 0x48, 0x98, 0x79, 0x129, 0x138
BRAIN_TASK = 0xC8
INTER_ANIMATOR = 0x140  # interaction_FollowerInteraction: cached by the coroutine
TASK_STATE, TASK_STATE_DOING = 0x20, 4  # FollowerTask.State
TASK_BRAIN = 0x18  # FollowerTask._brain: get_isAwake reads it and throws on null (0x2A3A708)
INTER_FOLLOWER, INTER_PREV_TASK, INTER_LISTENER, INTER_PREVENT_CLOSE = 0x130, 0x1F8, 0x1D0, 0x230
INFO_RECEIVED_BLESSING = 0x2D1
# FollowerCommands and the daily flags the handler checks (tools/cotl/README.md, Simple commands)
CMD_EXTORT, CMD_DANCE, CMD_ROMANCE, CMD_WAKE_UP, CMD_SLEEP, CMD_WORSHIP = 8, 9, 22, 23, 26, 27
CMD_INTIMIDATE, CMD_BRIBE, CMD_PET_DOG, CMD_BULLY, CMD_REASSURE = 43, 44, 75, 85, 86
INFO_KISSED, INFO_INSPIRED, INFO_PET_DOG, INFO_INTIMIDATED, INFO_BRIBED, INFO_SCARED_INTERACTED = \
    0x2D3, 0x2D4, 0x2D5, 0x2D7, 0x2D8, 0x2D9
INFO_PAID_TITHES = 0x2D0
DAILY_FLAGS = {CMD_EXTORT: (INFO_PAID_TITHES, "ALREADY_EXTORTED"),
               CMD_DANCE: (INFO_INSPIRED, "ALREADY_INSPIRED"),
               CMD_INTIMIDATE: (INFO_INTIMIDATED, "ALREADY_INTIMIDATED"),
               CMD_BRIBE: (INFO_BRIBED, "ALREADY_BRIBED"),
               CMD_ROMANCE: (INFO_KISSED, "ALREADY_KISSED"),
               CMD_PET_DOG: (INFO_PET_DOG, "ALREADY_PETTED"),
               CMD_BULLY: (INFO_SCARED_INTERACTED, "ALREADY_INTERACTED"),
               CMD_REASSURE: (INFO_SCARED_INTERACTED, "ALREADY_INTERACTED")}
INFO_IS_SNOWMAN, INFO_WORK_THROUGH_NIGHT = 0x210, 0x2E4
DM_ITEMS, DM_CURRENT_DAY, DM_SEASON, DM_WEATHER = 0xE80, 0x610, 0x16C, 0x174
ITEM_GOLD = 0x14
BRAIN_BRAIN_INFO, BRAIN_STATS = 0x10, 0x18
COMMAND_BLESS = 59
CMD_WALK_ONLY = 0x10000  # just walk there (quest giver), no command
REOPEN_ABORT = 0x2C70B30  # <OnInteract>b__5: Close(1, 1, 0), the game's own abort
STATE_LOCK = 0x44  # StateMachine.<LockStateChanges>
SOURCE = os.path.join(ROOT, "native", "helper", "cotl", "action_tick.c")


def f2b(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def b2f(v):
    return struct.unpack("<f", struct.pack("<I", v))[0]


def source_constant(name):
    with open(SOURCE, encoding="utf-8") as f:
        return int(re.search(name + r"\s*=\s*(\d+)", f.read()).group(1))

LOCATION_BASE = 1
THOUGHT_STARVING, THOUGHT_FREEZING, THOUGHT_CHILD = 0x83, 0x1E0, 0x188
TRAIT_EXISTENTIAL_DREAD, TRAIT_MISSIONARY_TERRIFIED = 0x4E, 0x43
# DataManager: Lists of IDs that FollowerLocked blocks on
DM_ID_LISTS = {"TraitManipulating": 0xDC0, "Imprisoned": 0xDC8, "OnMissionary": 0xDD8,
               "LeftInTheDungeon": 0xDE0, "Transitioning": 0xDE8, "Demons": 0xDF0}
DM_RECRUIT, DM_CULT_TRAITS = 0xCE8, 0x5C0

ST = {"IDLE": 0, "WALKING": 1, "RUNNING": 2, "DONE": 3, "REFUSED": 4, "CANCELLED": 5}
RS = {"NONE": 0, "NOT_AT_BASE": 1, "LAMB_BUSY": 2, "FOLLOWER_MISSING": 3, "FOLLOWER_LOCKED": 4,
      "NOT_AVAILABLE": 5, "STALE_SESSION": 6, "TIMEOUT": 7, "PLAYER_MOVED": 8, "ALREADY_BLESSED": 9,
      "ALREADY_INSPIRED": 10, "ALREADY_INTIMIDATED": 11, "ALREADY_BRIBED": 12, "NOT_ENOUGH_GOLD": 13,
      "ALREADY_KISSED": 14, "ALREADY_PETTED": 15, "ALREADY_INTERACTED": 16, "SLEEPS_UNTIL_SEASON": 17,
      "TOO_COLD": 18, "ALREADY_EXTORTED": 19, "NO_BUILD_SITES": 20, "NO_MEALS": 21, "NO_DRINKS": 22,
      "FASTING": 23, "CANT_WORK": 24, "NO_GIFTS": 25, "HAS_NECKLACE": 26, "HAS_PET": 27}
MB = {"EPOCH": 0x04, "REQ_SEQ": 0x08, "REQ_FOLLOWER": 0x0C, "REQ_COMMAND": 0x10, "REQ_EPOCH": 0x14,
      "ST_SEQ": 0x20, "ST_STATE": 0x24, "ST_REASON": 0x28, "ST_BUSY_SEQ": 0x2C,
      "CANCEL_SEQ": 0x18, "RUN_FOLLOWER": 0x150, "RUN_EPOCH": 0x154, "RUN_COMMAND": 0x158,
      "RUN_PHASE": 0x15C, "RUN_FRAMES": 0x160, "RUN_TARGET_X": 0x164, "RUN_TARGET_Y": 0x168,
      "RUN_TARGET_Z": 0x16C, "RUN_AXIS_X": 0x170, "RUN_AXIS_Y": 0x174, "RUN_TASK": 0x180,
      "REQ_PARENT": 0x1C, "RUN_PARENT": 0x188}


def fake_image():
    img = bytearray(HOOK + 8)
    img[HOOK:HOOK + 4] = bytes.fromhex("ff8304d1")
    return bytes(img)


def apply_relocations(code, relocs, bases):
    """The host's loader in miniature: patches adrp/add/b with the real addresses."""
    code = bytearray(code)
    for r in relocs:
        off = r["offset"]
        word = struct.unpack_from("<I", code, off)[0]
        target = bases[r["target"]] + r["addend"]
        pc = CODE + off
        if r["kind"] == "aarch64_adrp":
            pages = (target >> 12) - (pc >> 12)
            word = (word & 0x9F00001F) | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5)
        elif r["kind"] == "aarch64_add_lo12":
            word = (word & ~(0xFFF << 10)) | ((target & 0xFFF) << 10)
        elif r["kind"] == "aarch64_branch26":
            assert abs(target - pc) < (1 << 27), "Jump out of range"
            word = (word & 0xFC000000) | (((target - pc) >> 2) & 0x3FFFFFF)
        else:
            raise AssertionError(r["kind"])
        struct.pack_into("<I", code, off, word)
    return bytes(code)


class Machine:
    """Unicorn setup: fake main (only the pages needed), guest code, mailbox, stack and a
    small fake game (Lamb, follower, DataManager) in the heap. Game functions from GAME_FUNCS are
    stubs: they record the call (self.calls) and imitate the effect. Any other
    execution in main and any write outside mailbox, stack and allowed_writes (the
    fields of a follower interaction that the helper sets) makes run() fail."""

    def __init__(self, game=True):
        plan = gen.generate(fake_image())
        code = bytes.fromhex(plan["guest_code"]["bytes"])
        code = apply_relocations(code, plan["guest_code"]["relocations"], {"mailbox": MAILBOX, "main": MAIN, "code": CODE})
        self.size = len(code)
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc = self.uc
        uc.mem_map(CODE, (len(code) + 0xFFF) & ~0xFFF)
        uc.mem_write(CODE, code)
        uc.mem_map(MAILBOX, 0x1000)
        uc.mem_map(STACK, 0x10000)
        for page in sorted(set((MAIN + v) & ~0xFFF for v in TI.values())):
            uc.mem_map(page, 0x1000)
        uc.mem_map(KLASS, 0x1000)
        uc.mem_map(STATICS, 0x1000)
        uc.mem_map(PLAYER, 0x2000)
        uc.mem_map(HEAP, HEAP_SIZE)
        uc.mem_map(HIGH_HEAP, HIGH_HEAP_SIZE)
        uc.mem_map((MAIN + HOOK) & ~0xFFF, 0x1000)
        self.resume = MAIN + HOOK + 4
        uc.mem_write(self.resume, struct.pack("<I", 0xD503201F))
        self.heap_next = HEAP
        self.high_next = HIGH_HEAP
        self.bad_writes = []
        self.game_calls = []
        # Stubs of the game functions: one ret at each RVA
        self.stubs = {MAIN + rva: name for name, (_, rva) in GAME_FUNCS.items()}
        self.stubs.update({MAIN + rva: name for name, rva in RUNTIME_FUNCS.items()})
        mapped = set()
        for start, end, _ in uc.mem_regions():
            mapped.update(range(start, end + 1, 0x1000))
        for addr in self.stubs:
            if (addr & ~0xFFF) not in mapped:
                uc.mem_map(addr & ~0xFFF, 0x1000)
                mapped.add(addr & ~0xFFF)
            uc.mem_write(addr, struct.pack("<I", 0xD65F03C0))
        self.calls = []            # (name, {x0..x7, s0..s3, stack, sp})
        self.allowed_writes = set()
        self.coroutine_runs = True  # False: the coroutine never gets to set the task
        self.coroutines = []       # started, end of frame still pending
        self.coroutines_second = []  # task set, animator still on
        self.no_manual = set()     # interactions whose coroutine runs without ManualControl (sleepers)
        self.axis = {"axis_h": 0.0, "axis_v": 0.0}
        self.active = 1            # isActiveAndEnabled of the interaction, as long as it is not in disabled
        self.disabled = set()      # interactions that Follower.OnTaskChanged switched off
        self.go_off = set()        # components whose GameObject is inactive
        self.awake = 0             # FollowerTask_Sleep.get_isAwake
        self.reset_before_cache = 0
        self.handler_args = []     # (interaction, array, length, first element, MethodInfo)
        self.paused = False        # SimulationManager
        self.task_type = 0         # FollowerBrain.get_CurrentTaskType
        self.structures = []       # GetAllStructuresOfType(0x177): the daycares
        self.structures_by_type = {}  # GetAllStructuresOfType of other types (build sites)
        self.handler_arrays = []   # contents of each command array
        self.input_source = 0x7FFF0000
        self.pos = {}
        self.pos_default = (0.0, 0.0, 0.0)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        uc.hook_add(UC_HOOK_CODE, self._on_code, begin=MAIN, end=CODE - 1)
        self.set_game(klass=KLASS, statics=STATICS, instance=PLAYER)
        self.followers = []      # in Follower.Followers
        self.all_followers = []  # also those that vanished from the list: they live on
        if game:
            self.build_game()

    # --- Guard: the helper only reads
    def _on_write(self, uc, access, address, size, value, user):
        if not (MAILBOX <= address < MAILBOX + 0x1000 or STACK <= address < STACK + 0x10000 or
                address in self.allowed_writes):
            self.bad_writes.append(address)

    def _on_code(self, uc, address, size, user):
        name = self.stubs.get(address)
        if name:
            self._game_call(name)
        elif address != self.resume:
            self.game_calls.append(address)

    # --- Stubs of the game functions
    def _game_call(self, name):
        uc = self.uc
        sp = uc.reg_read(A.UC_ARM64_REG_SP)
        args = {f"x{n}": uc.reg_read(getattr(A, f"UC_ARM64_REG_X{n}")) for n in range(8)}
        args.update({f"s{n}": uc.reg_read(getattr(A, f"UC_ARM64_REG_S{n}")) for n in range(4)})
        args["sp"] = sp
        args["stack"] = struct.unpack("<6Q", bytes(uc.mem_read(sp, 48)))
        self.calls.append((name, args))
        x0 = args["x0"]
        if name == "cache_and_set":
            self.coroutines.append(x0)
            if not args["x2"] & 1:
                self.no_manual.add(x0)  # setManualControl = 0: the coroutine leaves the task alone (0x2C7B400)
        elif name == "go_to":
            self.set_u8(x0 + PF_GOTO_FLAG, 1)
            self.set_u8(x0 + PF_IDLE_ON_END, args["x2"] & 1)
            self.set_u64(x0 + PF_GOTO_CB, args["x4"])
            self.set_u64(x0 + PF_ABORT_CB, args["x6"])
            for i in range(3):
                self.set_u32(x0 + PF_TARGET + i * 4, args[f"s{i}"])
            for i, v in enumerate(self.position_of(x0)):
                self.set_u32(x0 + PF_LAST_POS + i * 4, f2b(v))
        elif name == "abort_go_to":
            # AbortGoTo (0x1CFC1F0): always Idle, IdleOnEnd does not count here. set_CURRENT_STATE
            # (0x1ED6BF4) changes nothing when <LockStateChanges> is set.
            self.set_u8(x0 + PF_GOTO_FLAG, 0)
            if not self.uc.mem_read(self.state + STATE_LOCK, 1)[0]:
                self.set_u32(self.state + 0x30, STATE_IDLE)
        elif name == "reset_follower":
            self.reset(x0)
        elif name == "set_enabled":
            f = next(f for f in self.all_followers if f["animator"] == x0)
            f["animator_on"] = bool(args["x1"] & 1)
        elif name == "sleep_is_awake":
            uc.reg_write(A.UC_ARM64_REG_X0, self.awake)
        elif name == "array_new" and self.array_new_fails:
            uc.reg_write(A.UC_ARM64_REG_X0, 0)
        elif name == "array_new":
            # il2cpp_array_new_specific: length @0x18, data @0x20; the helper writes only the data
            arr = self.alloc(0x20 + 4 * args["x1"])
            self.set_u64(arr, x0)
            self.set_u32(arr + 0x18, args["x1"])
            self.allowed_writes.update(arr + 0x20 + 4 * i for i in range(args["x1"]))
            uc.reg_write(A.UC_ARM64_REG_X0, arr)
        elif name == "init_metadata":
            # x0 is the address of the slot: afterwards the class is in it
            self.set_u64(x0, self.init_metadata_result)
        elif name == "command_finalized":
            arr = args["x1"]
            first = self.get_u32(arr + 0x20)
            self.handler_args.append((x0, arr, self.get_u32(arr + 0x18), first, args["x2"]))
            self.handler_arrays.append([self.get_u32(arr + 0x20 + 4 * i) for i in range(self.get_u32(arr + 0x18))])
            # The game sets ReceivedBlessing right away in the Bless branch (0x2C5F764..); the rest runs as a
            # coroutine (close_game)
            if first == COMMAND_BLESS:
                f = self.follower_of_inter(x0)
                self.set_u8(f["info"] + INFO_RECEIVED_BLESSING, 1)
            # Daily flags set right away (Bribe only in the coroutine's callback, 0x2C6B370)
            if first in DAILY_FLAGS and first != CMD_BRIBE:
                f = self.follower_of_inter(x0)
                self.set_u8(f["info"] + DAILY_FLAGS[first][0], 1)
            # These branches close by themselves before the handler returns (re-notes, Simple commands)
            if first == CMD_WAKE_UP:
                self.close(x0, 0, 1, 0)   # 0x2C5CF2C.., then TimedAnimation
                f = self.follower_of_inter(x0)
                self.set_u32(f["state_machine"] + 0x30, STATE_TIMED_ACTION)  # 0x2993E58
            elif first in (CMD_SLEEP, CMD_WORSHIP) or (self.get_u32(arr + 0x18) >= 2 and
                                                        self.get_u32(arr + 0x24) in (1, 25, 89)):
                # Submenus: Close(1, 1, 0) before the Drowsy block (tools/cotl/README.md, Food and work). Gift
                # and Give Pet only start their coroutine (close_game)
                self.close(x0, 1, 1, 0)   # end of 0x2C5ED0C
        elif name == "close":
            self.close(x0, args["x1"] & 1, args["x2"] & 1, args["x3"] & 1)
        elif name == "sim_pause":
            self.paused = True
        elif name == "sim_unpause":
            self.paused = False
        elif name == "conversation_new":
            self.set_u32(self.state + 0x30, STATE_INACTIVE)  # SetPlayerInactive = 1
        elif name == "current_task_type":
            uc.reg_write(A.UC_ARM64_REG_X0, self.task_type)
        elif name == "all_structures":
            # Daycares (0x177) from structures, other types from structures_by_type
            kind = x0 & 0xFFFFFFFF
            found = self.structures if kind == 0x177 else self.structures_by_type.get(kind, [])
            uc.reg_write(A.UC_ARM64_REG_X0, self.make_list(found, 8))
        elif name == "get_game_object":
            uc.reg_write(A.UC_ARM64_REG_X0, x0 + 8)
        elif name == "is_active":
            uc.reg_write(A.UC_ARM64_REG_X0, 0 if x0 in self.disabled else self.active)
        elif name == "go_active":
            uc.reg_write(A.UC_ARM64_REG_X0, 0 if x0 - 8 in self.go_off else 1)
        elif name == "gameplay":
            uc.reg_write(A.UC_ARM64_REG_X0, self.input_source)
        elif name in self.axis:
            uc.reg_write(A.UC_ARM64_REG_S0, self.axis_bits(name))
        elif name == "get_position":
            for i, v in enumerate(self.position_of(x0)):
                uc.reg_write(getattr(A, f"UC_ARM64_REG_S{i}"), f2b(v))

    def reset(self, inter):
        """ResetFollower (0x2C5BF14): animator on, then it finishes the current task. Before the
        coroutine caches it, that is wrong (reset_before_cache)."""
        f = self.follower_of_inter(inter)
        if inter in self.coroutines or inter in self.coroutines_second or not self.get_u64(inter + INTER_ANIMATOR):
            self.reset_before_cache += 1
        f["animator_on"] = True
        task = self.get_u64(f["brain"] + BRAIN_TASK)
        # Only the hold command is finished (CurrentTaskType 1, 0x2C5C08C); sleep stays
        if not task or self.get_u64(task) not in (self.kl["SLEEP"][0], self.kl["SLEEP_BED_REST"][0]):
            self.set_u64(f["brain"] + BRAIN_TASK, 0)
        self.disabled.discard(inter)  # Follower.OnTaskChanged: new task without the flag

    def close(self, inter, reset=1, unpause=1, reopen=0):
        """Close (0x2C60370) at its core: pause off, Lamb Idle, Activated 0, ResetFollower. With reopen it calls
        OnInteract again, which holds the follower again and sends the Lamb on its way to the wheel
        (0x2C5A2E0), callback b__4, abort b__5."""
        if unpause:
            self.paused = False
        self.set_u32(self.state + 0x30, STATE_IDLE)
        self.set_u8(inter + INTER_ACTIVATED, 0)
        if reset:
            self.reset(inter)
        if reopen:
            self.set_u64(inter + INTER_ANIMATOR, 0)
            self.coroutines.append(inter)
            closure = self.alloc(0x20)
            self.set_u64(closure + 0x10, inter)
            walk_cb, abort_cb = self.alloc(0x50), self.alloc(0x50)
            self.set_u64(abort_cb + 0x10, MAIN + REOPEN_ABORT)
            self.set_u64(abort_cb + 0x20, closure)
            self.set_u8(PLAYER + PF_GOTO_FLAG, 1)
            self.set_u8(PLAYER + PF_IDLE_ON_END, 0)
            self.set_u64(PLAYER + PF_GOTO_CB, walk_cb)
            self.set_u64(PLAYER + PF_ABORT_CB, abort_cb)

    def close_game(self, reopen=1):
        """The game ends the command itself: without a level-up Close() = Close(1, 1, 1), after
        LevelUpRoutine Close(1, 1, 0)."""
        self.close(self.followers[0]["inter"], 1, 1, reopen)

    def axis_bits(self, name):
        v = self.axis[name]
        return v if isinstance(v, int) else f2b(v)

    def position_of(self, obj):
        return self.pos.get(obj, self.pos_default)

    def follower_of_inter(self, inter):
        return next(f for f in self.all_followers if f["inter"] == inter)

    def end_walk(self, offset=(0.0, 0.0)):
        """EndGoToAndStop as in the game (0x1CFCF84..0x1CFCF90): flag off and state IdleOnEnd ? Idle :
        InActive; last position close to the target, off by offset. A run in the game got stuck exactly
        here: without IdleOnEnd and without a callback the Lamb stays in InActive."""
        idle = self.uc.mem_read(PLAYER + PF_IDLE_ON_END, 1)[0]
        self.set_u8(PLAYER + PF_GOTO_FLAG, 0)
        self.set_u32(self.state + 0x30, STATE_IDLE if idle else STATE_INACTIVE)
        for i in range(2):
            bits = self.get_u32(PLAYER + PF_TARGET + i * 4)
            self.set_u32(PLAYER + PF_LAST_POS + i * 4, f2b(b2f(bits) + offset[i]))

    def end_of_frame(self):
        """End of frame: while walking, PlayerFarming.Update forces Moving (except 0x10). The coroutine
        (0x2C7B5C8, 0x2C7B668) sets the task at the first end of frame and at the second
        switches the animator off, without checking the task again."""
        if self.followers and self.uc.mem_read(PLAYER + PF_GOTO_FLAG, 1)[0]:
            if self.get_u32(self.state + 0x30) != 0x10:
                self.set_u32(self.state + 0x30, STATE_MOVING)
        for inter in self.coroutines_second:
            f = self.follower_of_inter(inter)
            self.set_u64(inter + INTER_ANIMATOR, f["animator"])
            f["animator_on"] = False
        self.coroutines_second = []
        for inter in self.coroutines:
            if inter in self.no_manual:
                self.no_manual.discard(inter)
                self.coroutines_second.append(inter)
            elif self.coroutine_runs:
                f = self.follower_of_inter(inter)
                task = self.alloc(0x100)
                self.set_u64(task, self.kl["MANUAL"][0])
                self.set_u64(inter + INTER_TASK, task)
                self.set_u64(f["brain"] + BRAIN_TASK, task)
                # Follower.OnTaskChanged 0x298C758: enabled = !DisablePickUpInteraction, and that is
                # always 1 for FollowerTask_ManualControl (0x2A15AE0). The hold switches it off.
                self.disabled.add(inter)
                self.coroutines_second.append(inter)
        self.coroutines = []

    def unlist(self, f):
        """Follower.OnDisable (0x2989808): out of Follower.Followers, the object lives on."""
        lst = self.make_list([g["obj"] for g in self.followers if g is not f], 8)
        self.set_u64(self.kl["FOLLOWER"][1] + 0x0, lst)

    def names(self):
        return [c[0] for c in self.calls]

    def count(self, name):
        return self.names().count(name)

    def effects(self):
        """Calls with an effect, without the pure queries (position, input, ...)."""
        return [n for n in self.names() if n in ("cache_and_set", "go_to", "abort_go_to", "reset_follower",
                                                 "set_enabled", "sim_pause", "sim_unpause",
                                                 "conversation_new", "command_finalized", "close")]

    # --- Memory
    def set_u64(self, addr, value):
        self.uc.mem_write(addr, struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF))

    def set_u32(self, addr, value):
        self.uc.mem_write(addr, struct.pack("<I", value & 0xFFFFFFFF))

    def set_u8(self, addr, value):
        self.uc.mem_write(addr, bytes([value]))

    def get_u64(self, addr):
        return struct.unpack("<Q", self.uc.mem_read(addr, 8))[0]

    def get_u32(self, addr):
        return struct.unpack("<I", self.uc.mem_read(addr, 4))[0]

    def alloc_high(self, n):
        addr = self.high_next
        self.high_next += (n + 0xF) & ~0xF
        assert self.high_next <= HIGH_HEAP + HIGH_HEAP_SIZE
        return addr

    def alloc(self, n):
        addr = self.heap_next
        self.heap_next += (n + 0xF) & ~0xF
        assert self.heap_next <= HEAP + HEAP_SIZE
        return addr

    def klass(self, ti):
        """Il2CppClass in the heap: the TypeInfo slot of main points to it, static_fields @0xB8."""
        kl, statics = self.alloc(0x100), self.alloc(0x100)
        self.set_u64(kl + 0xB8, statics)
        self.set_u64(MAIN + ti, kl)
        return kl, statics

    def make_list(self, items, width):
        """List<T> (items @0x10, size @0x18) with array (length @0x18, data @0x20)."""
        arr = self.alloc(0x20 + width * (len(items) + 2))
        self.set_u32(arr + 0x18, len(items) + 2)
        for i, v in enumerate(items):
            (self.set_u64 if width == 8 else self.set_u32)(arr + 0x20 + i * width, v)
        lst = self.alloc(0x20)
        self.set_u64(lst + 0x10, arr)
        self.set_u32(lst + 0x18, len(items))
        return lst

    # --- Fake game
    def build_game(self):
        self.kl = {k: self.klass(TI[k]) for k in ("FOLLOWER", "BRAIN", "INFO", "DM", "FLOATING",
                                                           "UIMENU", "GM", "MMCONV", "MMTRANS",
                                                           "MANUAL", "MANUAL_HARD", "MANUAL_LIGHT",
                                                           "MANUAL_ENFORCING", "SLEEP",
                                                           "SLEEP_BED_REST", "STRUCTURES", "INT_ARRAY",
                                                           "NOTIFICATION", "AUDIO")}
        # The game has long since played sound by then: the AudioManager is alive
        audio = self.alloc(0x100)
        self.set_u64(audio + 0x10, 1)
        self.set_u64(self.kl["AUDIO"][1] + AUDIO_INSTANCE, audio)
        self.set_u32(self.kl["STRUCTURES"][0] + 0xE0, 1)  # class constructor has run
        self.init_metadata_result = self.kl["INT_ARRAY"][0]
        self.array_new_fails = False  # il2cpp_array_new_specific returns null (out of memory)
        # GameManager.instance (static @0x0)
        self.gm = self.alloc(0x40)
        self.set_u64(self.gm + 0x10, 1)
        self.set_u64(self.kl["GM"][1] + 0x0, self.gm)
        # Lamb: location base, state Idle
        self.set_u32(STATICS + 0x20, LOCATION_BASE)
        self.state = self.alloc(0x100)
        self.set_u64(self.state + 0x10, 1)  # m_CachedPtr: alive
        self.set_u32(self.state + 0x30, 0)
        self.allowed_writes.update({self.state + STATE_LOCK, PLAYER + PF_GOTO_CB})
        self.set_u64(PLAYER + 0x150, self.state)
        # Player list (PlayerFarming.players, static @0x0) with the Lamb; its header carries the class
        self.set_u64(PLAYER, self.get_u64(TYPEINFO))
        self.set_u64(STATICS + 0x0, self.make_list([PLAYER], 8))
        # Menus: UIMenuBase.ActiveMenus (static @0x0), empty
        self.active_menus = self.make_list([], 8)
        self.set_u64(self.kl["UIMENU"][1] + 0x0, self.active_menus)
        # DataManager
        self.dm = self.alloc(0x1000)
        self.set_u64(self.dm, self.kl["DM"][0])
        self.set_u64(self.kl["DM"][1] + 0xE0, self.dm)
        for off in DM_ID_LISTS.values():
            self.set_u64(self.dm + off, self.make_list([], 4))
        self.set_u64(self.dm + DM_RECRUIT, self.make_list([], 8))
        self.set_u64(self.dm + DM_CULT_TRAITS, self.make_list([], 4))
        self.set_items([(ITEM_GOLD, 10)])
        self.set_u32(self.dm + DM_CURRENT_DAY, 3)  # FollowerInfo.WorkThroughNight is set to a different day
        # One follower with ID 7, nothing locks it
        self.add_follower(7)
        self.publish_followers()
        self.set_u32(MAILBOX + MB["EPOCH"], 1)

    def add_follower(self, fid, thought=0):
        f = {"id": fid}
        f["info"] = self.alloc(0x300)
        self.set_u64(f["info"], self.kl["INFO"][0])
        self.set_u32(f["info"] + 0x10, fid)
        self.set_u32(f["info"] + 0x20, thought)
        f["traits"] = self.make_list([], 4)
        self.set_u64(f["info"] + 0x1E8, f["traits"])
        f["brain"] = self.alloc(0x100)
        self.set_u64(f["brain"], self.kl["BRAIN"][0])
        self.set_u64(f["brain"] + 0x20, f["info"])
        # BrainInfo and BrainStats: the handler's getters read the flags through them
        f["brain_info"], f["brain_stats"] = self.alloc(0x40), self.alloc(0x40)
        self.set_u64(f["brain_info"] + 0x10, f["info"])
        self.set_u64(f["brain_stats"] + 0x18, f["info"])
        self.set_u64(f["brain"] + BRAIN_BRAIN_INFO, f["brain_info"])
        self.set_u64(f["brain"] + BRAIN_STATS, f["brain_stats"])
        f["inter"] = self.alloc(0x300)
        self.set_u64(f["inter"], 0x1234000)  # class does not matter, only m_CachedPtr counts
        self.set_u64(f["inter"] + 0x10, 1)
        self.set_u8(f["inter"] + INTER_INTERACTABLE, 1)
        self.allowed_writes.update(f["inter"] + o for o in (INTER_STATE, INTER_PLAYER, INTER_ACTIVATED,
                                                            INTER_PREV_TASK, INTER_PREVENT_CLOSE))
        f["animator"] = self.alloc(0x40)
        self.set_u64(f["animator"] + 0x10, 1)
        f["animator_on"] = True
        f["obj"] = self.alloc(0x300)
        self.set_u64(f["obj"], self.kl["FOLLOWER"][0])
        self.set_u64(f["obj"] + 0x10, 1)
        self.set_u64(f["obj"] + 0xC8, f["inter"])
        f["state_machine"], f["spine"] = self.alloc(0x100), self.alloc(0x40)
        self.set_u64(f["state_machine"] + 0x10, 1)
        self.set_u64(f["spine"] + 0x10, 1)
        self.set_u64(f["obj"] + FOLLOWER_STATE, f["state_machine"])
        self.set_u64(f["obj"] + FOLLOWER_SPINE, f["spine"])
        self.set_u64(f["obj"] + 0x130, f["brain"])
        self.set_u64(f["inter"] + INTER_FOLLOWER, f["obj"])
        f["listener"] = self.alloc(0x40)
        self.set_u64(f["listener"] + 0x10, 1)
        self.set_u64(f["inter"] + INTER_LISTENER, f["listener"])
        self.followers.append(f)
        self.all_followers.append(f)
        return f

    def set_items(self, items):
        """DataManager.items: List<InventoryItem> with type @0x10, amount @0x14; None stands for null."""
        objs = []
        for it in items:
            if it is None:
                objs.append(0)
                continue
            o = self.alloc(0x20)
            self.set_u32(o + 0x10, it[0])
            self.set_u32(o + 0x14, it[1])
            objs.append(o)
        self.set_u64(self.dm + DM_ITEMS, self.make_list(objs, 8))

    def publish_followers(self):
        lst = self.make_list([f["obj"] for f in self.followers], 8)
        self.set_u64(self.kl["FOLLOWER"][1] + 0x0, lst)

    def make_task(self, floating=True, change_location=False):
        t = self.alloc(0x100)
        self.set_u64(t, self.kl["FLOATING"][0] if floating else 0x5550000)
        if change_location:
            self.set_u64(t + 0x28, self.alloc(0x100))
        return t

    def set_game(self, klass, statics, instance):
        self.set_u64(TYPEINFO, klass)
        self.set_u64(KLASS + 0xB8, statics)
        self.set_u64(STATICS + 0x10, instance)

    # --- Mailbox
    def heartbeat(self):
        return struct.unpack("<I", self.uc.mem_read(MAILBOX, 4))[0]

    def mailbox(self, n=0x200):
        return bytes(self.uc.mem_read(MAILBOX, n))

    def mb(self, name):
        return self.get_u32(MAILBOX + MB[name])

    def set_mb(self, name, value):
        self.set_u32(MAILBOX + MB[name], value)

    def request(self, seq, follower=7, epoch=1, command=4, parent=0):
        """Like the module: fields first, REQ_SEQ last. Default Talk (4): no refusal of its own in the helper;
        since the submenus it refuses GiveWorkerCommand_2 (1) on its own."""
        self.set_mb("REQ_FOLLOWER", follower)
        self.set_mb("REQ_COMMAND", command)
        self.set_mb("REQ_PARENT", parent)
        self.set_mb("REQ_EPOCH", epoch)
        self.set_mb("REQ_SEQ", seq)

    def status(self):
        return (self.mb("ST_SEQ"), self.mb("ST_STATE"), self.mb("ST_REASON"))

    def run(self, x0=PLAYER, regs=None):
        uc = self.uc
        sp = STACK + 0x8000
        values = {A.UC_ARM64_REG_SP: sp, A.UC_ARM64_REG_X0: x0}
        values.update(regs or {})
        for reg, v in values.items():
            uc.reg_write(reg, v)
        uc.emu_start(CODE, self.resume, count=3000000)
        self.end_of_frame()
        if self.bad_writes or self.game_calls:
            raise AssertionError("Helper wrote (%s) or executed game code (%s)" %
                                 ([hex(a) for a in self.bad_writes[:3]], [hex(a) for a in self.game_calls[:3]]))
        return sp


def reg(n):
    return getattr(A, f"UC_ARM64_REG_X{n}") if n != 29 else A.UC_ARM64_REG_X29


def walk_up(m, seq=5, **kw):
    """Request up to the running walk: frame 0 acceptance, frame 1 task seen, frame 2 walk."""
    m.request(seq, **kw)
    for _ in range(3):
        m.run()
    assert m.status() == (seq, ST["WALKING"], RS["NONE"]), m.status()
    assert "go_to" in m.names(), m.names()


class PlanTests(unittest.TestCase):
    def test_plan_shape(self):
        plan = gen.generate(fake_image())
        self.assertEqual(plan["format"], 2)
        self.assertEqual(plan["module"], "main")
        self.assertEqual(plan["title_id"], "01002E7016C46000")
        self.assertEqual(plan["build_id"], BUILD)
        self.assertEqual(len(plan["build_id"]), 64)
        self.assertEqual(plan["layout"]["segments"], [
            {"location": 0, "size": 90160584},
            {"location": 90161152, "size": 23461888},
            {"location": 113623040, "size": 7687040},
        ])
        self.assertEqual(plan["layout"]["bss_size"], 3789952)
        self.assertEqual(plan["mailbox_size"], 4096)
        self.assertEqual(plan["mailbox_epoch_offset"], 4)
        self.assertEqual(plan["payloads"], [])

    def test_hook_patch(self):
        plan = gen.generate(fake_image())
        self.assertEqual(len(plan["patches"]), 1)
        patch = plan["patches"][0]
        self.assertEqual(patch["offset"], HOOK)
        self.assertEqual(patch["expected"], "ff8304d1")
        # b (not bl): the hook replaces the first instruction of the function, x30 stays untouched
        self.assertEqual(patch["replacement"], "00000014")
        self.assertEqual(patch["relocations"],
                         [{"offset": 0, "kind": "aarch64_branch26", "target": "code", "addend": 0}])

    def test_refuses_other_hook_bytes(self):
        img = bytearray(fake_image())
        img[HOOK] ^= 0xFF
        with self.assertRaises(ValueError):
            gen.generate(bytes(img))
        with self.assertRaises(ValueError):
            gen.generate(b"\0" * 16)  # too short

    def test_guest_code_relocations(self):
        plan = gen.generate(fake_image())
        code = bytes.fromhex(plan["guest_code"]["bytes"])
        self.assertEqual(len(code) % 4, 0)
        self.assertLessEqual(len(code), 32768)
        kinds = sorted((r["kind"], r["target"], r["addend"]) for r in plan["guest_code"]["relocations"])
        self.assertEqual(kinds, sorted([
            ("aarch64_adrp", "mailbox", 0), ("aarch64_add_lo12", "mailbox", 0),
            ("aarch64_adrp", "main", 0), ("aarch64_add_lo12", "main", 0),
            ("aarch64_branch26", "main", HOOK + 4),
        ]))
        for r in plan["guest_code"]["relocations"]:
            self.assertEqual(r["offset"] % 4, 0)
            self.assertLess(r["offset"], len(code))

    def test_no_forbidden_instructions(self):
        # Mask as in the host (mod_load_plan.cpp): 0xD4xxxxxx and 0xD5xxxxxx except NOP
        code = bytes.fromhex(gen.generate(fake_image())["guest_code"]["bytes"])
        for i in range(0, len(code), 4):
            word = struct.unpack_from("<I", code, i)[0]
            self.assertNotEqual(word & 0xFF000000, 0xD4000000, f"SVC/HVC/SMC/BRK at {i:#x}")
            self.assertFalse((word & 0xFF000000) == 0xD5000000 and word != 0xD503201F,
                             f"System/barrier instruction at {i:#x}")

    def test_generation_is_reproducible(self):
        self.assertEqual(json.dumps(gen.generate(fake_image())), json.dumps(gen.generate(fake_image())))

    def test_committed_plan_is_current(self):
        if not os.path.exists(PLAN_PATH):
            self.fail("generated plan is missing: " + PLAN_PATH)
        with open(PLAN_PATH, encoding="utf-8") as f:
            self.assertEqual(f.read(), gen.render(gen.generate(fake_image())))


class AbiTests(unittest.TestCase):
    def test_header_matches_brief(self):
        with open(ABI_H, encoding="utf-8") as f:
            text = f.read()
        defs = {m.group(1): int(m.group(2), 0)
                for m in re.finditer(r"#define\s+COTL_ACT_([A-Z0-9_]+)\s+\(?(0x[0-9A-Fa-f]+|\d+)", text)}
        for name, off in {**ABI_OFFSETS, **ABI_OFFSETS_STAGE2}.items():
            self.assertEqual(defs.get(name), off, name)
        # no word twice, and the additions do not lie in Q_COMMANDS/Q_AVAILABLE (0x40..0x147)
        offs = [v for k, v in defs.items() if k in {**ABI_OFFSETS, **ABI_OFFSETS_STAGE2}]
        self.assertEqual(len(offs), len(set(offs)))
        for name, off in ABI_OFFSETS_STAGE2.items():
            self.assertFalse(0x40 <= off < 0x148, name)
        # Widths: pointers and mask 8 bytes, Q_COMMANDS 64 words, otherwise 4; no two words overlap
        wide = {"Q_COMMANDS": 0x100, "Q_AVAILABLE": 8, "RUN_OBJECT": 8, "RUN_TASK": 8}
        spans = sorted((off, off + wide.get(name, 4), name) for name, off in {**ABI_OFFSETS, **ABI_OFFSETS_STAGE2}.items())
        for (a0, a1, an), (b0, _, bn) in zip(spans, spans[1:]):
            self.assertLessEqual(a1, b0, f"{an} overlaps {bn}")
            self.assertEqual(a0 % 4, 0, an)
        enums = {m.group(1): int(m.group(2)) for m in re.finditer(r"COTL_ACT_([A-Z_]+)\s*=\s*(\d+)", text)}
        for i, n in enumerate(ABI_STATES):
            self.assertEqual(enums.get("STATE_" + n), i, n)
        for i, n in enumerate(ABI_REASONS):
            self.assertEqual(enums.get("REASON_" + n), i, n)


class GuestTests(unittest.TestCase):
    def test_passthrough(self):
        m = Machine()
        values = {reg(n): 0x1111000000000000 + n * 0x10101 for n in list(range(1, 8)) + list(range(19, 31))}
        values[reg(29)] = 0x2222222222222222
        sp = m.run(regs=values)
        uc = m.uc
        self.assertEqual(uc.reg_read(reg(0)), PLAYER)
        for n in list(range(1, 8)) + list(range(19, 31)):
            self.assertEqual(uc.reg_read(reg(n)), values[reg(n)], f"x{n}")
        # relocated instruction: sub sp, sp, #0x120 ran exactly once
        self.assertEqual(uc.reg_read(A.UC_ARM64_REG_SP), sp - 0x120)
        self.assertEqual(uc.reg_read(A.UC_ARM64_REG_PC), m.resume)

    def test_heartbeat(self):
        m = Machine()
        self.assertEqual(m.heartbeat(), 0)
        m.run()
        self.assertEqual(m.heartbeat(), 1)
        m.run()
        self.assertEqual(m.heartbeat(), 2)

    def test_only_heartbeat_word_changes(self):
        m = Machine()
        before = m.mailbox()
        m.run()
        after = m.mailbox()
        self.assertEqual(after[4:], before[4:])

    def test_query_changes_nothing(self):
        # Ruling on the command list: the module derives the list itself (tools/cotl/README.md). The helper
        # calls nothing in the game for it, even if the Q_* words are set, and leaves them alone.
        m = Machine()
        m.set_u32(MAILBOX + ABI_OFFSETS["Q_FOLLOWER"], 1)
        m.set_u32(MAILBOX + ABI_OFFSETS["Q_SEQ"], 5)
        before = m.mailbox()
        m.run()
        m.run()
        self.assertEqual(m.mailbox()[4:], before[4:])
        self.assertEqual(m.get_u32(MAILBOX + ABI_OFFSETS["Q_ACK"]), 0)
        self.assertEqual(m.get_u32(MAILBOX + ABI_OFFSETS["Q_COUNT"]), 0)
        self.assertEqual(m.calls, [])
        self.assertEqual(m.game_calls, [])
        self.assertEqual(m.bad_writes, [])

    def test_other_player_ignored(self):
        # PlayerFarming.Update runs once per player; only the main player counts
        m = Machine()
        before = m.mailbox()
        m.run(x0=OTHER)
        self.assertEqual(m.mailbox(), before)

    def test_missing_statics_do_nothing(self):
        for klass, statics, instance in [(0, STATICS, PLAYER), (KLASS, 0, PLAYER),
                                         (KLASS, STATICS, 0), (3, STATICS, PLAYER),
                                         (KLASS, 0xFFFFFFFFFFFFFFF8, PLAYER),
                                         # the real, unresolved token from a run, and
                                         # pointers in range but not 8-aligned
                                         (0x2000F921, STATICS, PLAYER), (KLASS, 0x2000F921, PLAYER),
                                         (KLASS, STATICS + 4, PLAYER), (KLASS, STATICS + 1, PLAYER),
                                         (KLASS + 4, STATICS, PLAYER), (KLASS, STATICS, PLAYER + 4)]:
            m = Machine()
            m.set_game(klass, statics, instance)
            before = m.mailbox()
            m.run(x0=PLAYER)  # must neither crash nor count
            self.assertEqual(m.mailbox(), before, (klass, statics, instance))
            m.request(5)  # and must not handle a request either
            m.run(x0=PLAYER)
            self.assertEqual(m.status(), (0, 0, 0))
        m = Machine()
        m.set_game(KLASS, STATICS, 0)
        m.run(x0=0)  # self null and instance null: no match
        self.assertEqual(m.heartbeat(), 0)

    def test_heartbeat_wraps(self):
        m = Machine()
        m.uc.mem_write(MAILBOX, struct.pack("<I", 0xFFFFFFFF))
        m.run()
        self.assertEqual(m.heartbeat(), 0)


class RequestTests(unittest.TestCase):
    """Stage 2: checking requests. A request that passes goes to WALKING. Refusals happen without
    game effect."""

    def refused(self, m, reason, seq=5):
        m.run()
        self.assertEqual(m.status(), (seq, ST["REFUSED"], RS[reason]))

    # --- Acceptance
    def test_valid_request_starts_walking(self):
        m = Machine()
        before = m.mailbox(0x1000)
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.mb("RUN_FOLLOWER"), 7)
        self.assertEqual(m.mb("RUN_EPOCH"), 1)
        # apart from the request (set by the test), heartbeat, status and run, the mailbox stays the same
        after = m.mailbox(0x1000)
        changed = {i for i in range(0, 0x1000, 4) if before[i:i + 4] != after[i:i + 4]}
        expected = {0x00, 0x08, 0x0C, 0x10, 0x14, 0x20, 0x24, 0x150, 0x154, 0x158, 0x15C, 0x160,
                    0x178, 0x17C}
        self.assertLessEqual(changed, expected)
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 0)

    def test_no_request_does_nothing(self):
        m = Machine()
        m.run()
        self.assertEqual(m.status(), (0, 0, 0))
        m.request(0)  # sequence 0 is not a request
        m.run()
        self.assertEqual(m.status(), (0, 0, 0))

    def test_request_only_handled_for_main_player(self):
        m = Machine()
        m.request(5)
        m.run(x0=OTHER)
        self.assertEqual(m.status(), (0, 0, 0))

    # --- Refusals
    def test_refuses_not_at_base(self):
        for location in (0, 2, 3, 0xFFFFFFFF):  # church, lumberjack, ..., None
            m = Machine()
            m.set_u32(STATICS + 0x20, location)
            m.request(5)
            self.refused(m, "NOT_AT_BASE")

    def test_refuses_lamb_busy(self):
        for state in (2, 3, 7, 9, 13, 49, 57, 58, 0xFFFFFFFF):  # everything except Idle (0) and Moving (1)
            m = Machine()
            m.set_u32(m.state + 0x30, state)
            m.request(5)
            self.refused(m, "LAMB_BUSY")

    def test_accepts_idle_and_moving(self):
        for state in (0, 1):
            m = Machine()
            m.set_u32(m.state + 0x30, state)
            m.request(5)
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), state)

    def test_refuses_follower_missing(self):
        m = Machine()
        m.request(5, follower=99)
        self.refused(m, "FOLLOWER_MISSING")
        m = Machine()
        m.followers.clear()
        m.publish_followers()
        m.request(5)
        self.refused(m, "FOLLOWER_MISSING")

    def test_refuses_stale_epoch(self):
        m = Machine()
        m.request(5, epoch=2)  # host is at 1
        self.refused(m, "STALE_SESSION")

    def test_refuses_null_brain(self):
        m = Machine()
        m.set_u64(m.followers[0]["obj"] + 0x130, 0)
        m.request(5)
        self.refused(m, "FOLLOWER_MISSING")

    def test_refuses_destroyed_follower(self):
        # Unity object destroyed: m_CachedPtr 0, the C# object is still alive. Nothing is called
        # (run() checks that no game code ran).
        m = Machine()
        m.set_u64(m.followers[0]["obj"] + 0x10, 0)
        m.request(5)
        self.refused(m, "FOLLOWER_MISSING")

    def test_destroyed_interaction_refuses(self):
        # Lightning (FollowerLocked) does not count on a destroyed interaction, but without an interaction
        # there is nothing to hold the follower with
        m = Machine()
        f = m.followers[0]
        m.set_u8(f["inter"] + 0x1DC, 1)  # lightning scheduled
        m.set_u64(f["inter"] + 0x10, 0)  # but destroyed
        m.request(5)
        self.refused(m, "FOLLOWER_LOCKED")
        self.assertEqual(m.calls, [])

    def lock_cases(self):
        """(name, setup) for each reason FollowerManager.FollowerLocked blocks (default values)."""
        def thought(v):
            return lambda m, f: m.set_u32(f["info"] + 0x20, v)

        def id_list(off):
            return lambda m, f: m.set_u64(m.dm + off, m.make_list([3, f["id"], 9], 4))

        cases = [
            ("leaving", lambda m, f: m.set_u8(f["info"] + 0xC0, 1)),
            ("starving", thought(THOUGHT_STARVING)),
            ("freezing", thought(THOUGHT_FREEZING)),
            ("child", thought(THOUGHT_CHILD)),
            ("lightning", lambda m, f: m.set_u8(f["inter"] + 0x1DC, 1)),
            ("floating", lambda m, f: m.set_u64(f["brain"] + 0xC8, m.make_task())),
            ("trait_dread_own", lambda m, f: m.set_u64(f["info"] + 0x1E8, m.make_list([5, TRAIT_EXISTENTIAL_DREAD], 4))),
            ("trait_terrified_own", lambda m, f: m.set_u64(f["info"] + 0x1E8, m.make_list([TRAIT_MISSIONARY_TERRIFIED], 4))),
            ("trait_dread_cult", lambda m, f: m.set_u64(m.dm + DM_CULT_TRAITS, m.make_list([TRAIT_EXISTENTIAL_DREAD], 4))),
            ("trait_terrified_cult", lambda m, f: m.set_u64(m.dm + DM_CULT_TRAITS, m.make_list([1, TRAIT_MISSIONARY_TERRIFIED], 4))),
            ("recruit", lambda m, f: m.set_u64(m.dm + DM_RECRUIT, m.make_list([0x1111110, f["info"]], 8))),
        ]
        cases += [("ids_" + n, id_list(o)) for n, o in DM_ID_LISTS.items()]
        return cases

    def test_refuses_locked(self):
        for name, setup in self.lock_cases():
            m = Machine()
            setup(m, m.followers[0])
            m.request(5)
            m.run()
            self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]), name)

    def test_unrelated_lock_data_does_not_lock(self):
        m = Machine()
        f = m.followers[0]
        m.set_u32(f["info"] + 0x20, 0x84)  # a different thought
        m.set_u64(f["info"] + 0x1E8, m.make_list([1, 2, 3], 4))
        m.set_u64(m.dm + DM_CULT_TRAITS, m.make_list([4, 5], 4))
        for off in DM_ID_LISTS.values():
            m.set_u64(m.dm + off, m.make_list([6, 8, 70000], 4))  # ID 7 is missing
        m.set_u64(m.dm + DM_RECRUIT, m.make_list([0x1111110], 8))
        m.set_u64(f["brain"] + 0xC8, m.make_task(floating=False))  # different task
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_floating_with_change_location_subtask_is_not_floating(self):
        # get_CurrentTaskType takes the subtask (@0x28) as soon as there is one
        m = Machine()
        m.set_u64(m.followers[0]["brain"] + 0xC8, m.make_task(change_location=True))
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_garbage_pointers_refuse_without_crash(self):
        bad = [0x2000F921, 0xFFFFFFFFFFFFFFF8, 0x7FFFFFFFFFFFFFF8, 0x60000003, 0x1000]

        def followers_list(m, v):
            m.set_u64(m.kl["FOLLOWER"][1] + 0x0, v)

        def list_items(m, v):
            lst = m.make_list([m.followers[0]["obj"]], 8)
            m.set_u64(lst + 0x10, v)
            followers_list(m, lst)

        def list_element(m, v):
            followers_list(m, m.make_list([v], 8))

        paths = {
            "list": followers_list, "list_items": list_items, "list_element": list_element,
            "brain": lambda m, v: m.set_u64(m.followers[0]["obj"] + 0x130, v),
            "info": lambda m, v: m.set_u64(m.followers[0]["brain"] + 0x20, v),
            "state": lambda m, v: m.set_u64(PLAYER + 0x150, v),
            "dm": lambda m, v: m.set_u64(m.kl["DM"][1] + 0xE0, v),
            "dm_list": lambda m, v: m.set_u64(m.dm + 0xDC8, v),
            "dm_traits": lambda m, v: m.set_u64(m.dm + DM_CULT_TRAITS, v),
            "dm_recruit": lambda m, v: m.set_u64(m.dm + DM_RECRUIT, v),
            "traits": lambda m, v: m.set_u64(m.followers[0]["info"] + 0x1E8, v),
            "task": lambda m, v: m.set_u64(m.followers[0]["brain"] + 0xC8, v),
            "klass_follower": lambda m, v: m.set_u64(MAIN + TI["FOLLOWER"], v),
            "statics_follower": lambda m, v: m.set_u64(m.kl["FOLLOWER"][0] + 0xB8, v),
        }
        for name, poke in paths.items():
            for v in bad:
                m = Machine()
                poke(m, v)
                m.request(5)
                m.run()
                # never WALKING: unreadable data counts as missing, locked or Lamb busy
                self.assertEqual(m.status()[:2], (5, ST["REFUSED"]), (name, hex(v)))
                self.assertIn(m.status()[2], (RS["FOLLOWER_MISSING"], RS["FOLLOWER_LOCKED"],
                                              RS["LAMB_BUSY"]), (name, hex(v)))

    def test_oversized_lists_refuse(self):
        m = Machine()
        lst = m.make_list([m.followers[0]["obj"]], 8)
        m.set_u32(lst + 0x18, 0x7FFFFFFF)  # size far above the array length
        m.set_u64(m.kl["FOLLOWER"][1] + 0x0, lst)
        m.request(5)
        self.refused(m, "FOLLOWER_MISSING")

    # --- Order and repetition
    def test_seq_runs_once(self):
        # Refused: the same sequence is not re-evaluated, even if the situation changes
        m = Machine()
        m.set_u32(STATICS + 0x20, 0)
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["NOT_AT_BASE"]))
        m.set_u32(STATICS + 0x20, LOCATION_BASE)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["NOT_AT_BASE"]))
        # Accepted: after DONE (set later by the module or helper) the same sequence does not start again
        m = Machine()
        m.request(6)
        m.run()
        self.assertEqual(m.status(), (6, ST["WALKING"], RS["NONE"]))
        m.set_mb("ST_STATE", ST["DONE"])
        m.run()
        m.run()
        self.assertEqual(m.status(), (6, ST["DONE"], RS["NONE"]))

    def test_new_request_after_finish_is_handled(self):
        m = Machine()
        m.request(5)
        m.run()
        m.set_mb("ST_STATE", ST["DONE"])
        m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, 0)  # as after releasing
        m.disabled.clear()
        m.request(6, follower=99)
        m.run()
        self.assertEqual(m.status(), (6, ST["REFUSED"], RS["FOLLOWER_MISSING"]))
        m.request(7)
        m.run()
        self.assertEqual(m.status(), (7, ST["WALKING"], RS["NONE"]))

    def test_busy_refuses_second_request(self):
        m = Machine()
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.request(6)
        for _ in range(3):
            m.run()
            # the first keeps running, the second is noted as refused (LAMB_BUSY)
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
            self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        self.assertEqual(m.mb("RUN_FOLLOWER"), 7)
        # a second request for the same follower is busy too
        m.request(7)
        m.run()
        self.assertEqual((m.status(), m.mb("ST_BUSY_SEQ")), ((5, ST["WALKING"], RS["NONE"]), 7))

    def test_busy_request_is_not_started_after_first_ends(self):
        m = Machine()
        m.request(5)
        m.run()
        m.request(6)
        m.run()
        m.followers.clear()
        m.publish_followers()  # first action ends
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))
        m.run()
        self.assertEqual(m.status()[0], 5)  # 6 does not start by itself; the module asks again
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)

    def test_busy_uses_own_copy_of_the_run(self):
        # A second request overwrites REQ_*; the run keeps checking the first follower
        m = Machine()
        m.add_follower(8)
        m.publish_followers()
        m.request(5, follower=7)
        m.run()
        m.request(6, follower=8, epoch=1)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.followers.pop(0)  # follower 7 disappears, 8 stays
        m.publish_followers()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))

    # --- Lamb busy through menu, conversation, scene change (read only)
    def test_refuses_when_menu_open(self):
        m = Machine()
        m.set_u64(m.kl["UIMENU"][1] + 0x0, m.make_list([0x7000F000], 8))  # UIMenuBase.ActiveMenus
        m.request(5)
        self.refused(m, "LAMB_BUSY")
        m = Machine()
        m.set_u8(m.kl["GM"][1] + 0x24, 1)  # GameManager.InMenu
        m.request(5)
        self.refused(m, "LAMB_BUSY")

    def test_refuses_during_transition_and_conversation(self):
        m = Machine()
        m.set_u8(m.kl["MMTRANS"][1] + 0x9, 1)  # MMTransition.IsPlaying
        m.request(5)
        self.refused(m, "LAMB_BUSY")
        m = Machine()
        m.set_u8(m.kl["MMCONV"][1] + 0x38, 1)  # MMConversation.isPlaying
        m.request(5)
        self.refused(m, "LAMB_BUSY")
        m = Machine()
        m.set_u8(PLAYER + 0x378, 1)  # PlayerFarming.wasInConversation (AnyPlayerInConversation)
        m.request(5)
        self.refused(m, "LAMB_BUSY")

    def test_other_player_in_conversation_blocks(self):
        m = Machine()
        m.set_u64(OTHER, m.get_u64(TYPEINFO))
        m.set_u8(OTHER + 0x378, 1)
        m.set_u64(STATICS + 0x0, m.make_list([PLAYER, OTHER], 8))
        m.request(5)
        self.refused(m, "LAMB_BUSY")

    def test_gates_open_when_classes_unused(self):
        # A class whose code has never run (TypeInfo slot unresolved or without statics) cannot
        # show anything: no obstacle
        for ti in ("UIMENU", "GM", "MMCONV", "MMTRANS"):
            for slot in (0, 0x2000F921):
                m = Machine()
                m.set_u64(MAIN + TI[ti], slot)
                m.request(5)
                m.run()
                self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), (ti, slot))
            m = Machine()
            m.set_u64(m.kl[ti][0] + 0xB8, 0)
            m.request(5)
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), ti)

    def test_gate_garbage_refuses(self):
        for v in (0x2000F921, 0xFFFFFFFFFFFFFFF8, 0x60000003):
            for poke in (lambda m: m.set_u64(m.kl["UIMENU"][1] + 0x0, v),
                         lambda m: m.set_u64(STATICS + 0x0, v),
                         lambda m: m.set_u64(STATICS + 0x0, m.make_list([v], 8))):
                m = Machine()
                poke(m)
                m.request(5)
                self.refused(m, "LAMB_BUSY")

    # --- Follower no longer available while walking
    def walking(self):
        m = Machine()
        walk_up(m)
        return m

    def test_cancel_when_follower_starts_leaving_mid_walk(self):
        m = self.walking()
        m.set_u8(m.followers[0]["info"] + 0xC0, 1)  # LeavingCult
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))

    def test_cancel_for_other_lock_reasons_mid_walk(self):
        for name, setup in self.lock_cases():
            m = self.walking()
            setup(m, m.followers[0])
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]), name)

    def test_cancel_when_follower_picked_up(self):
        # Follower.PickUp sets PlayerFarming.PickedUpFollower (@0x2C0) and the ManualControl task
        m = self.walking()
        m.set_u64(PLAYER + 0x2C0, m.followers[0]["obj"])
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))

    def test_other_followers_picked_up_does_not_matter(self):
        m = Machine()
        m.add_follower(8)
        m.publish_followers()
        m.set_u64(PLAYER + 0x2C0, m.followers[1]["obj"])
        m.request(5, follower=7)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_refuses_picked_up_follower(self):
        m = Machine()
        m.set_u64(PLAYER + 0x2C0, m.followers[0]["obj"])
        m.request(5)
        self.refused(m, "FOLLOWER_LOCKED")

    def test_dying_follower(self):
        # Follower._dying (@0x150): is refused and aborts a run
        m = Machine()
        m.set_u8(m.followers[0]["obj"] + 0x150, 1)
        m.request(5)
        self.refused(m, "FOLLOWER_MISSING")
        m = self.walking()
        m.set_u8(m.followers[0]["obj"] + 0x150, 1)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))

    def test_walk_with_unreadable_lock_data_is_cancelled(self):
        m = self.walking()
        m.set_u64(m.dm + 0xDC8, 0x2000F921)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))

    # --- while walking
    def test_cancel_when_follower_gone(self):
        m = Machine()
        m.request(5)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.followers.clear()
        m.publish_followers()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))
        m.run()  # stays that way
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))

    def test_cancel_when_follower_destroyed_or_brain_null(self):
        for poke in (lambda m: m.set_u64(m.followers[0]["obj"] + 0x10, 0),
                     lambda m: m.set_u64(m.followers[0]["obj"] + 0x130, 0),
                     lambda m: m.set_u64(m.kl["FOLLOWER"][1] + 0x0, 0)):
            m = Machine()
            m.request(5)
            m.run()
            poke(m)
            # As long as the object lives, the helper waits for the coroutine; only then the end
            for _ in range(source_constant("kHoldTimeoutFrames")):
                m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))

    def test_destroyed_follower_ends_the_hold_wait_at_once(self):
        m = Machine()
        m.request(5)
        m.run()
        m.set_u64(m.followers[0]["obj"] + 0x10, 0)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))

    def test_epoch_change_mid_walk(self):
        m = Machine()
        m.request(5)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.set_mb("EPOCH", 2)  # the host paused or resumed
        m.run()
        m.run()  # only in the frame after the task becomes visible is the coroutine done
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))

    def test_epoch_change_beats_follower_gone(self):
        m = Machine()
        m.request(5)
        m.run()
        m.set_mb("EPOCH", 2)
        m.unlist(m.followers[0])
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))

    def test_new_request_after_cancel_is_handled(self):
        m = Machine()
        m.request(5)
        m.run()
        m.set_mb("EPOCH", 2)
        m.run()
        m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, 0)  # the session change clears the brains
        m.disabled.clear()
        m.request(6, epoch=2)
        m.run()
        self.assertEqual(m.status(), (6, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.mb("RUN_EPOCH"), 2)

    def test_walking_continues_while_everything_holds(self):
        m = Machine()
        m.request(5)
        m.run()
        for _ in range(5):
            m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.heartbeat(), 6)


class WalkTests(unittest.TestCase):
    """The walk to the follower: holding, GoToAndStop, queries, abort and release."""

    FOLLOWER_POS = (10.0, 5.0, 0.25)

    def machine(self, player=(0.0, 0.0, 0.0)):
        m = Machine()
        m.pos[PLAYER] = player
        m.pos[m.followers[0]["obj"]] = self.FOLLOWER_POS
        return m

    def walking(self):
        m = self.machine()
        walk_up(m)
        return m

    def arrive(self, m, offset=(0.0, 0.0)):
        """The game ends the walk by itself (EndGoToAndStop)."""
        m.end_walk(offset)

    def cancelled(self, m, reason, effects):
        before = len(m.effects())
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS[reason]))
        self.assertEqual(m.effects()[before:], effects)
        # and it stays that way: no further call
        n = len(m.calls)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS[reason]))
        self.assertEqual(len(m.calls), n)

    # --- Order and arguments
    def test_walk_called_once(self):
        m = self.walking()
        for _ in range(20):
            m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.effects(), ["cache_and_set", "go_to"])

    def test_hold_arguments(self):
        m = self.machine()
        f = m.followers[0]
        m.request(5)
        m.run()
        name, a = m.calls[-1]
        self.assertEqual(name, "cache_and_set")
        self.assertEqual((a["x0"], a["x1"], a["x2"] & 0xFF), (f["inter"], 0, 1))  # null, setManualControl
        # the two fields that Interaction.OnInteract otherwise sets and the coroutine reads
        self.assertEqual(m.get_u64(f["inter"] + INTER_STATE), m.state)
        self.assertEqual(m.get_u64(f["inter"] + INTER_PLAYER), PLAYER)
        self.assertEqual(m.mb("ST_STATE"), ST["WALKING"])

    def test_status_is_published_before_the_game_call(self):
        # Should the game function re-enter the update, the request must not start again
        m = self.machine()
        seen = []
        original = m._game_call

        def spy(name):
            if name == "cache_and_set":
                seen.append(m.status())
            original(name)
        m._game_call = spy
        m.request(5)
        m.run()
        self.assertEqual(seen, [(5, ST["WALKING"], RS["NONE"])])

    def test_walk_waits_until_follower_is_held(self):
        m = self.machine()
        m.request(5)
        m.run()  # Frame 0: acceptance, task only at the end of the frame
        self.assertEqual(m.effects(), ["cache_and_set"])
        m.run()  # Frame 1: task seen; the caching only runs at the end of this frame
        self.assertEqual(m.effects(), ["cache_and_set"])
        m.run()  # Frame 2
        self.assertEqual(m.effects(), ["cache_and_set", "go_to"])

    def test_walk_arguments(self):
        m = self.walking()
        f = m.followers[0]
        a = dict(m.calls)["go_to"]
        self.assertEqual(a["x0"], PLAYER)
        # next to the follower, on the side facing the Lamb: x - 2, as in interaction_FollowerInteraction.OnInteract
        self.assertEqual((a["s0"], a["s1"], a["s2"]), (f2b(8.0), f2b(5.0), f2b(0.25)))
        self.assertEqual(a["s3"], f2b(30.0))  # maxDuration
        self.assertEqual(a["x1"], f["obj"] + 8)  # LookTo = follower.gameObject
        self.assertEqual(a["x2"] & 0xFFFFFFFF, 1)  # IdleOnEnd: the Lamb ends up free, see re-notes
        self.assertEqual(a["x3"] & 0xFFFFFFFF, 1)  # DisableCollider
        self.assertEqual(a["x4"], 0)  # GoToCallback: none, the helper polls
        self.assertEqual(a["x5"] & 0xFFFFFFFF, 0)  # forcePositionOnTimeout
        self.assertEqual(a["x6"], 0)  # AbortCallback
        self.assertEqual(a["x7"] & 0xFFFFFFFF, 0)  # groupAction
        self.assertEqual(a["stack"][0] & 0xFF, 1)
        self.assertEqual(a["stack"][1] & 0xFF, 1)
        self.assertEqual(a["stack"][2] & 0xFF, 0)
        self.assertEqual(a["stack"][3:], (0, 0, 0))  # Nullable<Vector3> empty, MethodInfo null
        for i, v in zip(("X", "Y", "Z"), (8.0, 5.0, 0.25)):
            self.assertEqual(m.mb("RUN_TARGET_" + i), f2b(v))

    def test_walk_target_is_on_the_lamb_side(self):
        m = self.machine(player=(20.0, 5.0, 0.0))  # Lamb to the right of the follower
        walk_up(m)
        self.assertEqual(dict(m.calls)["go_to"]["s0"], f2b(12.0))

    def test_stack_is_aligned_at_every_game_call(self):
        m = self.walking()
        m.axis["axis_h"] = 0.9
        m.run()
        self.assertGreater(len(m.calls), 8)
        for name, a in m.calls:
            self.assertEqual(a["sp"] % 16, 0, name)

    def test_method_info_is_null(self):
        # The game functions do not read the hidden MethodInfo; null is passed, as in the game
        m = self.walking()
        calls = dict(m.calls)
        self.assertEqual(calls["get_transform"]["x1"], 0)
        self.assertEqual(calls["get_position"]["x1"], 0)
        self.assertEqual(calls["get_game_object"]["x1"], 0)
        self.assertEqual(calls["is_active"]["x1"], 0)
        self.assertEqual(calls["cache_and_set"]["x3"], 0)
        m.axis["axis_v"] = 0.9
        m.run()
        calls = dict(m.calls)
        self.assertEqual(calls["gameplay"]["x0"], 0)
        self.assertEqual((calls["axis_v"]["x0"], calls["axis_v"]["x1"], calls["axis_v"]["x2"]),
                         (m.input_source, PLAYER, 0))
        self.assertEqual((calls["abort_go_to"]["x1"] & 0xFFFFFFFF, calls["abort_go_to"]["x2"]), (0, 0))
        self.assertEqual(calls["reset_follower"]["x1"], 0)

    # --- Arrival
    def test_arrival_runs_the_command_once(self):
        m = self.walking()
        self.arrive(m)
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        self.assertEqual(m.effects(), ["cache_and_set", "go_to", "sim_pause", "conversation_new",
                                       "command_finalized"])
        n = len(m.calls)
        m.run()
        self.assertEqual((m.status()[1], len(m.calls)), (ST["RUNNING"], n))

    def test_arrival_tolerates_a_stop_short_of_the_target(self):
        m = self.walking()
        self.arrive(m, offset=(-1.5, 1.5))
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))

    def test_walk_ended_far_away_is_timeout(self):
        # The game ends the walk through the same function on timeout or when the Lamb is stuck
        for offset in ((-5.0, 0.0), (0.0, 5.0), (float("nan"), 0.0)):
            m = self.walking()
            self.arrive(m, offset=offset)
            self.cancelled(m, "TIMEOUT", ["reset_follower"])

    def test_new_request_after_arrival_starts_again(self):
        m = self.walking()
        self.arrive(m)
        m.run()
        m.close_game(reopen=0)
        m.run()
        walk_up(m, seq=6)
        self.assertEqual(m.effects(), ["cache_and_set", "go_to", "sim_pause", "conversation_new",
                                       "command_finalized", "cache_and_set", "go_to"])

    # --- The Lamb is free after every ending
    def assert_lamb_free(self, m, name):
        self.assertEqual(m.uc.mem_read(PLAYER + PF_GOTO_FLAG, 1)[0], 0, name)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE, name)

    def test_lamb_is_free_after_every_end_path(self):
        limit = source_constant("kWalkTimeoutFrames")

        def menu(m, on):
            m.set_u64(m.kl["UIMENU"][1], m.make_list([0x7000F000] if on else [], 8))

        # Name: (setup, undo for the second request, state, reason)
        paths = {
            "arrival": (lambda m: m.end_walk(), None, "DONE", "NONE"),
            "stopped_short": (lambda m: m.end_walk((-1.5, 1.5)), None, "DONE", "NONE"),
            "ended_far_away": (lambda m: m.end_walk((-9.0, 0.0)), None, "CANCELLED", "TIMEOUT"),
            "stick": (lambda m: m.axis.update(axis_h=1.0), lambda m: m.axis.update(axis_h=0.0),
                      "CANCELLED", "PLAYER_MOVED"),
            "module_cancel": (lambda m: m.set_mb("CANCEL_SEQ", 5), None, "CANCELLED", "PLAYER_MOVED"),
            "timeout": (lambda m: m.set_mb("RUN_FRAMES", limit), None, "CANCELLED", "TIMEOUT"),
            "menu": (lambda m: menu(m, True), lambda m: menu(m, False), "CANCELLED", "LAMB_BUSY"),
            "in_menu": (lambda m: m.set_u8(m.kl["GM"][1] + 0x24, 1),
                        lambda m: m.set_u8(m.kl["GM"][1] + 0x24, 0), "CANCELLED", "LAMB_BUSY"),
            "conversation": (lambda m: m.set_u8(m.kl["MMCONV"][1] + 0x38, 1),
                             lambda m: m.set_u8(m.kl["MMCONV"][1] + 0x38, 0), "CANCELLED", "LAMB_BUSY"),
            "transition": (lambda m: m.set_u8(m.kl["MMTRANS"][1] + 0x9, 1),
                           lambda m: m.set_u8(m.kl["MMTRANS"][1] + 0x9, 0), "CANCELLED", "LAMB_BUSY"),
            "locked": (lambda m: m.set_u8(m.followers[0]["info"] + 0xC0, 1), None, "CANCELLED", "FOLLOWER_LOCKED"),
            "hold_lost": (lambda m: m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, m.make_task(floating=False)),
                          None, "CANCELLED", "FOLLOWER_LOCKED"),
            "follower_gone": (lambda m: m.set_u64(m.followers[0]["obj"] + 0x10, 0), None, "CANCELLED", "FOLLOWER_MISSING"),
            "follower_dying": (lambda m: m.set_u8(m.followers[0]["obj"] + 0x150, 1), None, "CANCELLED", "FOLLOWER_MISSING"),
            "epoch": (lambda m: m.set_mb("EPOCH", 2), None, "CANCELLED", "STALE_SESSION"),
            # The walk ends in the same frame in which something else happens: no AbortGoTo any more
            "ended_and_locked": (lambda m: (m.end_walk(), m.set_u8(m.followers[0]["info"] + 0xC0, 1)),
                                 None, "CANCELLED", "FOLLOWER_LOCKED"),
            "ended_and_menu": (lambda m: (m.end_walk(), menu(m, True)), lambda m: menu(m, False),
                               "CANCELLED", "LAMB_BUSY"),
            "ended_and_gone": (lambda m: (m.end_walk(), m.set_u64(m.followers[0]["obj"] + 0x10, 0)),
                               None, "CANCELLED", "FOLLOWER_MISSING"),
        }
        for name, (poke, undo, state, reason) in paths.items():
            m = self.machine()
            m.add_follower(8)
            m.publish_followers()
            walk_up(m)
            self.assertEqual(m.get_u32(m.state + 0x30), STATE_MOVING, name)
            poke(m)
            m.run()
            if state == "DONE":  # arrived: the command runs until the game closes it
                self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), name)
                m.close_game(reopen=0)
                m.run()
            self.assertEqual(m.status(), (5, ST[state], RS[reason]), name)
            self.assert_lamb_free(m, name)
            m.run()
            self.assert_lamb_free(m, name)  # and stays so without anyone helping
            self.assertEqual(m.reset_before_cache, 0, name)
            if m.get_u64(m.followers[0]["obj"] + 0x10):  # destroyed: no animator needed any more
                self.assertTrue(m.followers[0]["animator_on"], name)
            # The ending only counts once the Lamb can be sent off again
            if undo:
                undo(m)
            walk_up(m, seq=6, follower=8, epoch=m.mb("EPOCH"))
            self.assertEqual(m.count("go_to"), 2, name)

    def test_second_bless_after_arrival_is_not_lamb_busy(self):
        # This is how it looked in the game: Done, Lamb in InActive, every further request "Lamb is busy"
        m = self.machine()
        m.add_follower(8)
        m.publish_followers()
        walk_up(m)
        m.end_walk()
        m.run()
        m.close_game(reopen=0)
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        m.request(6, follower=8)
        m.run()
        self.assertEqual(m.status(), (6, ST["WALKING"], RS["NONE"]))

    def test_fake_strands_the_lamb_without_idle_on_end(self):
        # Checks the stub itself: it must not show the end of the walk more kindly than the game does
        m = self.walking()
        m.set_u8(PLAYER + PF_IDLE_ON_END, 0)
        m.end_walk()
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_INACTIVE)

    # --- Abort
    def test_timeout_cancels(self):
        limit = source_constant("kWalkTimeoutFrames")
        m = self.walking()
        m.set_mb("RUN_FRAMES", limit - 2)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.cancelled(m, "TIMEOUT", ["abort_go_to", "reset_follower"])

    def test_timeout_outlasts_a_far_corner_walk_at_30_and_60_fps(self):
        # Longer than the 30 s the game itself gives the walk, even at 60 frames per second
        limit = source_constant("kWalkTimeoutFrames")
        self.assertGreater(limit / 60.0, 30.0)
        self.assertLess(limit / 30.0, 120.0)

    def test_stick_cancels(self):
        for axis, value in (("axis_h", 0.5), ("axis_h", -0.5), ("axis_v", 0.5), ("axis_v", -1.0)):
            m = self.walking()
            m.axis[axis] = value
            self.cancelled(m, "PLAYER_MOVED", ["abort_go_to", "reset_follower"])

    def test_stick_nan_cancels(self):
        m = self.walking()
        m.axis["axis_h"] = 0x7FC00000
        self.cancelled(m, "PLAYER_MOVED", ["abort_go_to", "reset_follower"])

    # --- Diagnostics until Task 8: what the helper read from the stick (act.dbg_axis)
    def axis_words(self, m):
        return m.mb("RUN_AXIS_X"), m.mb("RUN_AXIS_Y")

    def test_axis_words_say_not_read_until_the_first_read(self):
        m = self.walking()
        self.assertEqual(self.axis_words(m), (0xFFFFFFFF, 0xFFFFFFFF))

    def test_axis_words_hold_the_raw_read(self):
        m = self.walking()
        m.axis.update(axis_h=0.2, axis_v=-0.2)
        m.run()
        self.assertEqual(self.axis_words(m), (f2b(0.2), f2b(-0.2)))
        m.axis.update(axis_h=0.0, axis_v=0.0)
        m.run()
        self.assertEqual(self.axis_words(m), (0, 0))

    def test_axis_words_keep_the_read_that_cancelled(self):
        m = self.walking()
        m.axis.update(axis_h=-1.0, axis_v=0.5)
        self.cancelled(m, "PLAYER_MOVED", ["abort_go_to", "reset_follower"])
        self.assertEqual(self.axis_words(m), (f2b(-1.0), f2b(0.5)))

    def test_axis_words_hold_nan_bits(self):
        m = self.walking()
        m.axis["axis_h"] = 0x7FC00000
        m.run()
        self.assertEqual(m.mb("RUN_AXIS_X"), 0x7FC00000)

    def test_second_walk_resets_the_axis_words(self):
        m = self.walking()
        m.axis["axis_h"] = 1.0
        m.run()
        m.axis["axis_h"] = 0.0
        walk_up(m, seq=6)
        self.assertEqual(self.axis_words(m), (0xFFFFFFFF, 0xFFFFFFFF))

    def test_stick_drift_does_not_cancel(self):
        m = self.walking()
        m.axis.update(axis_h=0.2, axis_v=-0.2)
        for _ in range(5):
            m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_stick_is_ignored_while_only_holding(self):
        m = self.machine()
        m.axis["axis_h"] = 1.0
        m.request(5)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.count("axis_h"), 0)

    def test_module_cancel_stops_the_walk(self):
        m = self.walking()
        m.set_mb("CANCEL_SEQ", 4)  # an old sequence does not count
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.set_mb("CANCEL_SEQ", 5)
        self.cancelled(m, "PLAYER_MOVED", ["abort_go_to", "reset_follower"])

    def test_follower_released_on_cancel(self):
        limit = source_constant("kWalkTimeoutFrames")

        def lock(m):
            m.set_u8(m.followers[0]["info"] + 0xC0, 1)

        paths = {
            "stick": (lambda m: m.axis.update(axis_h=1.0), "PLAYER_MOVED"),
            "timeout": (lambda m: m.set_mb("RUN_FRAMES", limit), "TIMEOUT"),
            "module": (lambda m: m.set_mb("CANCEL_SEQ", 5), "PLAYER_MOVED"),
            "locked": (lock, "FOLLOWER_LOCKED"),
            "menu": (lambda m: m.set_u64(m.kl["UIMENU"][1], m.make_list([0x7000F000], 8)), "LAMB_BUSY"),
        }
        for name, (poke, reason) in paths.items():
            m = self.walking()
            poke(m)
            self.cancelled(m, reason, ["abort_go_to", "reset_follower"])
            self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), 0, name)
            self.assertEqual(m.get_u32(PLAYER + PF_GOTO_FLAG) & 0xFF, 0, name)

    def test_menu_or_conversation_mid_walk_cancels(self):
        gates = {
            "menus": lambda m: m.set_u64(m.kl["UIMENU"][1], m.make_list([0x7000F000], 8)),
            "in_menu": lambda m: m.set_u8(m.kl["GM"][1] + 0x24, 1),
            "transition": lambda m: m.set_u8(m.kl["MMTRANS"][1] + 0x9, 1),
            "conversation": lambda m: m.set_u8(m.kl["MMCONV"][1] + 0x38, 1),
            "was_in_conversation": lambda m: m.set_u8(PLAYER + 0x378, 1),
        }
        for name, poke in gates.items():
            m = self.walking()
            poke(m)
            self.cancelled(m, "LAMB_BUSY", ["abort_go_to", "reset_follower"])

    def test_walk_does_not_cancel_itself(self):
        # Held: task ManualControl, flag GoToAndStopping, the game forces Moving. Neither
        # follower_unavailable nor the request's state check may react to it.
        m = self.walking()
        task = m.get_u64(m.followers[0]["brain"] + BRAIN_TASK)
        self.assertEqual(m.get_u64(task), m.kl["MANUAL"][0])
        for state in (1, 0x10, 0, 2, 0x39):
            m.set_u32(m.state + 0x30, state)
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), state)
        self.assertEqual(m.effects(), ["cache_and_set", "go_to"])

    def test_epoch_change_mid_walk_releases_like_any_cancel(self):
        # The host changes the epoch on pause and resume (core.cpp), the scene lives on.
        # Without calls the follower would stay in ManualControl with the animator switched off.
        m = self.walking()
        m.set_mb("EPOCH", 2)
        self.cancelled(m, "STALE_SESSION", ["abort_go_to", "reset_follower"])
        f = m.followers[0]
        self.assertTrue(f["animator_on"])
        self.assertEqual(m.get_u64(f["brain"] + BRAIN_TASK), 0)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE)

    def test_epoch_change_during_hold_waits_for_the_coroutine(self):
        m = self.machine()
        m.request(5)
        m.run()
        m.set_mb("EPOCH", 2)
        m.run()  # task only just visible: the animator only goes off at the end of this frame
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.effects(), ["cache_and_set"])
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))
        self.assertEqual(m.effects(), ["cache_and_set", "reset_follower"])
        self.assertEqual(m.reset_before_cache, 0)
        self.assertTrue(m.followers[0]["animator_on"])

    def test_epoch_change_beats_everything(self):
        m = self.walking()
        m.set_mb("EPOCH", 2)
        m.axis["axis_h"] = 1.0
        m.set_u8(m.kl["GM"][1] + 0x24, 1)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))

    def test_follower_destroyed_mid_walk_stops_the_lamb_only(self):
        m = self.walking()
        m.set_u64(m.followers[0]["obj"] + 0x10, 0)
        self.cancelled(m, "FOLLOWER_MISSING", ["abort_go_to"])

    def test_follower_dying_mid_walk_gets_only_the_animator_back(self):
        # find_follower no longer reports it. ResetFollower would overwrite the death animation.
        m = self.walking()
        f = m.followers[0]
        task = m.get_u64(f["brain"] + BRAIN_TASK)
        m.set_u8(f["obj"] + 0x150, 1)
        self.cancelled(m, "FOLLOWER_MISSING", ["abort_go_to", "set_enabled"])
        self.assertTrue(f["animator_on"])
        self.assertEqual(m.get_u64(f["brain"] + BRAIN_TASK), task)
        a = dict(m.calls)["set_enabled"]
        self.assertEqual((a["x0"], a["x1"] & 0xFFFFFFFF, a["x2"]), (f["animator"], 1, 0))

    def test_follower_dying_during_hold_waits_for_the_coroutine(self):
        m = self.machine()
        f = m.followers[0]
        m.request(5)
        m.run()
        m.set_u8(f["obj"] + 0x150, 1)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))
        self.assertTrue(f["animator_on"])
        self.assertEqual(m.effects(), ["cache_and_set", "set_enabled"])

    def test_follower_disabled_mid_walk_is_released_and_can_be_asked_again(self):
        # Follower.OnDisable takes it out of Follower.Followers. If it stayed in ManualControl, it would be
        # locked after OnEnable until reloading: BlockTaskChanges, and check_hold refuses it.
        m = self.walking()
        f = m.followers[0]
        m.unlist(f)
        self.cancelled(m, "FOLLOWER_MISSING", ["abort_go_to", "reset_follower"])
        self.assertEqual(m.get_u64(f["brain"] + BRAIN_TASK), 0)
        self.assertTrue(f["animator_on"])
        m.publish_followers()  # Follower.OnEnable
        walk_up(m, seq=6)
        self.assertEqual(m.count("go_to"), 2)

    def test_hold_lost_mid_walk_restores_only_the_animator(self):
        # The game gave the follower another task (HardSwapToTask checks nothing).
        # ResetFollower would finish that task; without the animator it would stay frozen in its pose.
        m = self.walking()
        f = m.followers[0]
        theirs = m.make_task(floating=False)
        m.set_u64(f["brain"] + BRAIN_TASK, theirs)
        self.cancelled(m, "FOLLOWER_LOCKED", ["abort_go_to", "set_enabled"])
        self.assertTrue(f["animator_on"])
        self.assertEqual(m.get_u64(f["brain"] + BRAIN_TASK), theirs)

    def test_hold_lost_unseen_during_hold_wait_restores_the_animator(self):
        # Task set at the first end of frame and swapped before the next hook: the helper never
        # sees it, but the coroutine switches the animator off anyway.
        limit = source_constant("kHoldTimeoutFrames")
        m = self.machine()
        f = m.followers[0]
        m.request(5)
        m.run()
        m.set_u64(f["brain"] + BRAIN_TASK, m.make_task(floating=False))
        for _ in range(limit - 1):
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertFalse(f["animator_on"])
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["TIMEOUT"]))
        self.assertTrue(f["animator_on"])
        self.assertEqual(m.effects(), ["cache_and_set", "set_enabled"])

    def test_destroyed_animator_is_left_alone(self):
        m = self.walking()
        f = m.followers[0]
        m.set_u64(f["animator"] + 0x10, 0)
        m.set_u64(f["brain"] + BRAIN_TASK, m.make_task(floating=False))
        self.cancelled(m, "FOLLOWER_LOCKED", ["abort_go_to"])

    def test_activated_interaction_keeps_its_disabled_animator(self):
        # The game runs the interaction and releases by itself
        m = self.walking()
        f = m.followers[0]
        m.set_u8(f["inter"] + INTER_ACTIVATED, 1)
        m.set_u64(f["brain"] + BRAIN_TASK, m.make_task(floating=False))
        self.cancelled(m, "FOLLOWER_LOCKED", ["abort_go_to"])
        self.assertFalse(f["animator_on"])

    def test_normal_ends_bring_the_animator_back(self):
        m = self.walking()
        self.assertFalse(m.followers[0]["animator_on"])  # the stub shows what the game does
        self.arrive(m)
        m.run()
        m.close_game(reopen=0)  # the game's Close, no longer the helper's
        self.assertTrue(m.followers[0]["animator_on"])
        self.assertEqual(m.reset_before_cache, 0)

    # --- The game has taken over the Lamb
    def test_abort_keeps_the_state_the_game_set(self):
        # A on something else while walking: the opener sets InActive or a state of its own, and
        # AbortGoTo would write Idle over it. Then the stick would work again during the conversation.
        for state in (STATE_INACTIVE, 49):
            m = self.walking()
            m.set_u32(m.state + 0x30, state)
            m.set_u8(m.kl["MMCONV"][1] + 0x38, 1)
            self.cancelled(m, "LAMB_BUSY", ["abort_go_to", "reset_follower"])
            self.assertEqual(m.get_u32(m.state + 0x30), state)
            self.assertEqual(m.uc.mem_read(PLAYER + PF_GOTO_FLAG, 1)[0], 0)
            self.assertEqual(m.uc.mem_read(m.state + STATE_LOCK, 1)[0], 0)

    def test_inactive_alone_ends_the_walk(self):
        # Otherwise PlayerFarming.Update would set Moving over it in the same frame
        m = self.walking()
        m.set_u32(m.state + 0x30, STATE_INACTIVE)
        self.cancelled(m, "LAMB_BUSY", ["abort_go_to", "reset_follower"])
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_INACTIVE)

    def test_abort_leaves_an_existing_lock_set(self):
        m = self.walking()
        m.set_u32(m.state + 0x30, STATE_INACTIVE)
        m.set_u8(m.state + STATE_LOCK, 1)
        m.run()
        self.assertEqual(m.uc.mem_read(m.state + STATE_LOCK, 1)[0], 1)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_INACTIVE)

    def test_stick_cancel_while_moving_still_ends_idle(self):
        m = self.walking()
        m.axis["axis_h"] = 1.0
        m.run()
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE)
        self.assertEqual(m.uc.mem_read(m.state + STATE_LOCK, 1)[0], 0)

    # --- Reach
    def test_arrival_tolerance(self):
        for offset, state, reason in (((1.5, 0.0), "RUNNING", "NONE"), ((-1.5, 1.5), "RUNNING", "NONE"),
                                      ((1.6, 0.0), "CANCELLED", "TIMEOUT"),
                                      ((0.0, -1.6), "CANCELLED", "TIMEOUT")):
            m = self.walking()
            self.arrive(m, offset=offset)
            m.run()
            self.assertEqual(m.status(), (5, ST[state], RS[reason]), offset)

    def test_picked_up_mid_walk_does_not_release(self):
        # Follower.PickUp: new task ManualControl (not ours) and PickedUpFollower
        m = self.walking()
        f = m.followers[0]
        task = m.alloc(0x100)
        m.set_u64(task, m.kl["MANUAL"][0])
        m.set_u64(f["brain"] + BRAIN_TASK, task)
        m.set_u64(PLAYER + 0x2C0, f["obj"])
        self.cancelled(m, "FOLLOWER_LOCKED", ["abort_go_to", "set_enabled"])

    def test_walk_overwritten_by_game_is_not_aborted(self):
        # Another GoToAndStop is running: the flag is set, but target or callback are not ours
        pokes = {
            "target": lambda m: m.set_u32(PLAYER + PF_TARGET, f2b(99.0)),
            "callback": lambda m: m.set_u64(PLAYER + PF_GOTO_CB, 0x70001000),
            "abort_callback": lambda m: m.set_u64(PLAYER + PF_ABORT_CB, 0x70001000),
        }
        for name, poke in pokes.items():
            m = self.walking()
            poke(m)
            self.cancelled(m, "LAMB_BUSY", ["reset_follower"])

    def test_player_interacts_with_the_follower_mid_walk(self):
        # The interactor keeps running while walking too: A on the same follower takes it over. Neither
        # abort nor release; the game runs the interaction itself.
        m = self.walking()
        f = m.followers[0]
        m.set_u8(f["inter"] + INTER_ACTIVATED, 1)
        m.set_u64(PLAYER + PF_GOTO_CB, 0x70001000)
        self.cancelled(m, "LAMB_BUSY", [])

    # --- Holding
    def test_coroutine_that_never_runs_times_out_without_release(self):
        limit = source_constant("kHoldTimeoutFrames")
        m = self.machine()
        m.coroutine_runs = False
        m.request(5)
        m.run()
        for _ in range(limit - 1):
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["TIMEOUT"]))
        self.assertEqual(m.effects(), ["cache_and_set"])
        self.assertLessEqual(limit, 120)

    def test_cancels_during_hold_wait_for_the_cache(self):
        # In the frame in which the task first becomes visible, the coroutine has not yet
        # cached anything; ResetFollower would write nonsense back. Release only after that.
        locks = {
            "locked": (lambda m: m.set_u8(m.followers[0]["info"] + 0xC0, 1), "FOLLOWER_LOCKED"),
            "menu": (lambda m: m.set_u8(m.kl["GM"][1] + 0x24, 1), "LAMB_BUSY"),
        }
        for name, (poke, reason) in locks.items():
            m = self.machine()
            m.request(5)
            m.run()
            poke(m)
            m.run()
            self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), name)
            self.assertEqual(m.effects(), ["cache_and_set"], name)
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS[reason]), name)
            self.assertEqual(m.effects(), ["cache_and_set", "reset_follower"], name)

    def test_lamb_busy_when_hold_settles(self):
        m = self.machine()
        m.request(5)
        m.run()
        m.run()
        m.set_u32(m.state + 0x30, 7)  # the state check still applies before setting off
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["LAMB_BUSY"]))
        self.assertEqual(m.effects(), ["cache_and_set", "reset_follower"])

    def test_hold_lost_before_walk(self):
        m = self.machine()
        m.request(5)
        m.run()
        m.run()
        m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, m.make_task(floating=False))
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.effects(), ["cache_and_set", "set_enabled"])
        self.assertTrue(m.followers[0]["animator_on"])

    def test_sleeping_followers_are_refused(self):
        # OnInteract passes setManualControl = !isSleeping (0x2C59D98). The helper always passes 1,
        # so HardSwapToTask would break off sleep or bed rest.
        def task(m, f, key, state, sub=False):
            t = m.alloc(0x100)
            m.set_u64(t, m.kl[key][0])
            m.set_u32(t + TASK_STATE, state)
            m.set_u64(t + TASK_BRAIN, f["brain"])
            if sub:
                m.set_u64(t + 0x28, m.alloc(0x100))
            m.set_u64(f["brain"] + BRAIN_TASK, t)
        cases = {
            "asleep": ("SLEEP", TASK_STATE_DOING, 0, False, True),
            "sleep_awake": ("SLEEP", TASK_STATE_DOING, 1, False, False),
            "going_to_bed": ("SLEEP", 1, 0, False, False),
            "sleep_sub_task": ("SLEEP", TASK_STATE_DOING, 0, True, False),
            "bed_rest": ("SLEEP_BED_REST", TASK_STATE_DOING, 0, False, True),
            "bed_rest_arriving": ("SLEEP_BED_REST", 1, 0, False, False),
            "bed_rest_sub_task": ("SLEEP_BED_REST", TASK_STATE_DOING, 0, True, False),
        }
        for name, (key, state, awake, sub, refused) in cases.items():
            m = self.machine()
            m.awake = awake
            task(m, m.followers[0], key, state, sub)
            m.request(5)
            m.run()
            if refused:
                self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]), name)
                self.assertEqual(m.effects(), [], name)
            else:
                self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), name)

    def test_refuses_what_cannot_be_held(self):
        def manual(key):
            def poke(m, f):
                t = m.alloc(0x100)
                m.set_u64(t, m.kl[key][0])
                m.set_u64(f["brain"] + BRAIN_TASK, t)
            return poke
        cases = {
            "not_interactable": (lambda m, f: m.set_u8(f["inter"] + INTER_INTERACTABLE, 0), "FOLLOWER_LOCKED"),
            "activated": (lambda m, f: m.set_u8(f["inter"] + INTER_ACTIVATED, 1), "FOLLOWER_LOCKED"),
            "no_interaction": (lambda m, f: m.set_u64(f["obj"] + 0xC8, 0), "FOLLOWER_LOCKED"),
            "inactive": (lambda m, f: setattr(m, "active", 0), "FOLLOWER_LOCKED"),
            "manual": (manual("MANUAL"), "FOLLOWER_LOCKED"),
            "manual_hard": (manual("MANUAL_HARD"), "FOLLOWER_LOCKED"),
            "manual_light": (manual("MANUAL_LIGHT"), "FOLLOWER_LOCKED"),
            "manual_enforcing": (manual("MANUAL_ENFORCING"), "FOLLOWER_LOCKED"),
            "walking_already": (lambda m, f: m.set_u8(PLAYER + PF_GOTO_FLAG, 1), "LAMB_BUSY"),
        }
        for name, (poke, reason) in cases.items():
            m = self.machine()
            poke(m, m.followers[0])
            m.request(5)
            m.run()
            self.assertEqual(m.status(), (5, ST["REFUSED"], RS[reason]), name)
            self.assertEqual(m.effects(), [], name)
            # the interaction's fields stay untouched
            self.assertEqual(m.get_u64(m.followers[0]["inter"] + INTER_STATE), 0, name)

    def test_refuses_destroyed_player_parts(self):
        for off in (0xF0, 0xF8):
            m = self.machine()
            part = m.alloc(0x40)  # m_CachedPtr 0: destroyed
            m.set_u64(PLAYER + off, part)
            m.request(5)
            m.run()
            self.assertEqual(m.status(), (5, ST["REFUSED"], RS["LAMB_BUSY"]), off)
            self.assertEqual(m.effects(), [])

    def test_no_game_call_on_refusal(self):
        m = self.machine()
        m.request(5, follower=99)
        m.run()
        self.assertEqual(m.calls, [])

    def test_only_two_interaction_fields_are_written(self):
        # run() checks all writes; here: the guard really triggers
        m = self.machine()
        m.allowed_writes.clear()
        m.request(5)
        with self.assertRaises(AssertionError):
            m.run()

    # --- Run copies
    def test_run_command_is_copied(self):
        m = self.machine()
        m.request(5, command=4)
        m.run()
        self.assertEqual(m.mb("RUN_COMMAND"), 4)
        m.request(6, command=44)  # second request overwrites REQ_*, is refused as busy
        m.run()
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        self.assertEqual(m.mb("RUN_COMMAND"), 4)
        self.assertEqual(m.mb("REQ_COMMAND"), 44)

    def test_busy_request_during_walk_causes_no_game_call(self):
        m = self.walking()
        n = len(m.effects())
        m.request(6)
        m.run()
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        self.assertEqual(len(m.effects()), n)


class WalkOnlyTests(unittest.TestCase):
    """Walk only (Walk over in the quest window): no holding, no command. Holding would replace the
    follower's task, for the quest giver FollowerTask_GetAttention, and with it the offer."""

    def machine(self):
        m = Machine()
        m.pos[PLAYER] = (0.0, 0.0, 0.0)
        m.pos[m.followers[0]["obj"]] = WalkTests.FOLLOWER_POS
        return m

    def walking(self):
        m = self.machine()
        m.request(5, command=CMD_WALK_ONLY)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        return m

    def test_walks_at_once_without_holding(self):
        m = self.machine()
        task = m.get_u64(m.followers[0]["brain"] + BRAIN_TASK)
        m.request(5, command=CMD_WALK_ONLY)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.effects(), ["go_to"])
        self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), task)

    def test_arrival_is_done_and_leaves_the_follower_alone(self):
        m = self.walking()
        m.end_walk((0.0, 0.0))
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        self.assertEqual(m.effects(), ["go_to"])
        n = len(m.calls)
        m.run()
        self.assertEqual((m.status(), len(m.calls)), ((5, ST["DONE"], RS["NONE"]), n))

    def test_stick_stops_the_lamb_only(self):
        m = self.walking()
        m.axis.update(axis_h=1.0)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["PLAYER_MOVED"]))
        self.assertEqual(m.effects(), ["go_to", "abort_go_to"])

    def test_module_cancel_and_timeout_stop_the_lamb_only(self):
        limit = source_constant("kWalkTimeoutFrames")
        for poke, reason in ((lambda m: m.set_mb("CANCEL_SEQ", 5), "PLAYER_MOVED"),
                             (lambda m: m.set_mb("RUN_FRAMES", limit), "TIMEOUT")):
            m = self.walking()
            poke(m)
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS[reason]))
            self.assertEqual(m.effects(), ["go_to", "abort_go_to"])

    def test_locked_follower_can_still_be_walked_to(self):
        # Locked (here: leaving) forbids commands, not walking over
        m = self.machine()
        m.set_u8(m.followers[0]["info"] + 0xC0, 1)
        m.request(5, command=CMD_WALK_ONLY)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_carried_follower_is_refused(self):
        m = self.machine()
        m.set_u64(PLAYER + 0x2C0, m.followers[0]["obj"])
        m.request(5, command=CMD_WALK_ONLY)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.effects(), [])

    def test_missing_follower_and_away_from_base_are_refused(self):
        m = self.machine()
        m.request(5, follower=99, command=CMD_WALK_ONLY)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_MISSING"]))

    def test_picked_up_mid_walk_cancels_without_release(self):
        m = self.walking()
        m.set_u64(PLAYER + 0x2C0, m.followers[0]["obj"])
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.effects(), ["go_to", "abort_go_to"])

    def test_command_request_after_walk_only_holds_again(self):
        m = self.walking()
        m.end_walk((0.0, 0.0))
        m.run()
        walk_up(m, seq=6)
        self.assertEqual(m.effects(), ["go_to", "cache_and_set", "go_to"])


class CommandTests(unittest.TestCase):
    """The command after arrival: calling the game's handler, waiting for Close, release."""

    def machine(self):
        m = Machine()
        m.pos[m.followers[0]["obj"]] = (10.0, 5.0, 0.25)
        return m

    def running(self, command=COMMAND_BLESS):
        m = self.machine()
        walk_up(m, command=command)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        return m

    def unreached(self, m):
        self.assertEqual(m.handler_args, [])
        for name in ("sim_pause", "conversation_new", "array_new"):
            self.assertEqual(m.count(name), 0, name)

    def test_handler_called_with_command(self):
        m = self.running()
        inter = m.followers[0]["inter"]
        self.assertEqual(len(m.handler_args), 1)
        self_arg, array, length, first, method = m.handler_args[0]
        self.assertEqual((self_arg, length, first, method), (inter, 1, COMMAND_BLESS, 0))
        self.assertEqual(m.get_u64(array), m.kl["INT_ARRAY"][0])

    def test_state_running_then_done(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        self.assertEqual(m.status()[1], ST["WALKING"])
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        for _ in range(5):
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        m.close_game(reopen=0)
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        self.assertEqual(len(m.handler_args), 1)

    def test_handler_not_called_after_epoch_change(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.set_mb("EPOCH", 2)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))
        self.unreached(m)
        self.assertEqual(m.count("reset_follower"), 1)

    # --- Before the call: what the game otherwise does in OnInteract and b__4
    def test_order_and_arguments_of_the_calls(self):
        m = self.running()
        calls = dict(m.calls)
        self.assertEqual(m.effects()[-3:], ["sim_pause", "conversation_new", "command_finalized"])
        new = calls["conversation_new"]
        self.assertEqual((new["x0"], new["x1"] & 1, new["x2"] & 1, new["x3"], new["x4"]), (m.gm, 1, 1, 0, 0))
        self.assertEqual((calls["array_new"]["x0"], calls["array_new"]["x1"] & 0xFFFFFFFF),
                         (m.kl["INT_ARRAY"][0], 1))
        self.assertTrue(m.paused)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_INACTIVE)

    def test_interaction_is_marked_while_the_command_runs(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        inter = m.followers[0]["inter"]
        self.assertEqual(m.uc.mem_read(inter + INTER_ACTIVATED, 1)[0], 0)
        m.set_u8(inter + INTER_PREVENT_CLOSE, 1)
        m.end_walk()
        m.run()
        self.assertEqual(m.uc.mem_read(inter + INTER_ACTIVATED, 1)[0], 1)
        self.assertEqual(m.uc.mem_read(inter + INTER_PREVENT_CLOSE, 1)[0], 0)

    def test_previous_task_type_is_stored_before_the_hold(self):
        # OnInteract stores it before the coroutine swaps the task; the game reads it after the command
        m = self.machine()
        m.task_type = 0x2A
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.get_u32(m.followers[0]["inter"] + INTER_PREV_TASK), 0x2A)
        names = m.names()
        self.assertLess(names.index("current_task_type"), names.index("cache_and_set"))

    def test_array_holds_the_accepted_command_not_the_later_request(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.request(6, command=3)
        m.run()
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        m.end_walk()
        m.run()
        self.assertEqual(m.handler_args[0][3], COMMAND_BLESS)

    def test_array_class_is_resolved_when_the_slot_is_not(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.set_u64(MAIN + TI["INT_ARRAY"], 0x20000C81)  # Il2Cpp: when unresolved, a token is in the slot
        m.end_walk()
        m.run()
        self.assertEqual(dict(m.calls)["init_metadata"]["x0"], MAIN + TI["INT_ARRAY"])
        self.assertEqual(m.get_u64(m.handler_args[0][1]), m.kl["INT_ARRAY"][0])

    def test_unresolvable_array_class_refuses_without_effects(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.init_metadata_result = 0
        m.set_u64(MAIN + TI["INT_ARRAY"], 0)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]))
        self.unreached(m)
        self.assertEqual(m.count("reset_follower"), 1)

    def test_failed_array_allocation_cancels_before_any_effect(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        inter = m.followers[0]["inter"]
        m.set_u8(inter + INTER_PREVENT_CLOSE, 1)
        m.array_new_fails = True
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]))
        self.assertEqual(m.count("array_new"), 1)
        self.assertEqual(m.handler_args, [])
        for name in ("sim_pause", "conversation_new"):
            self.assertEqual(m.count(name), 0, name)
        self.assertEqual(m.uc.mem_read(inter + INTER_ACTIVATED, 1)[0], 0)
        self.assertEqual(m.uc.mem_read(inter + INTER_PREVENT_CLOSE, 1)[0], 1)
        self.assertEqual(m.mb("RUN_TASK"), 0)
        self.assertEqual(m.count("reset_follower"), 1)

    def test_what_would_throw_in_the_handler_cancels_before_any_effect(self):
        # An exception would go through the helper without unwind tables: check everything before the first call
        pokes = {
            "listener_null": lambda m, f: m.set_u64(f["inter"] + INTER_LISTENER, 0),
            "listener_destroyed": lambda m, f: m.set_u64(f["listener"] + 0x10, 0),
            "other_follower": lambda m, f: m.set_u64(f["inter"] + INTER_FOLLOWER, 0),
            "no_game_manager": lambda m, f: m.set_u64(m.kl["GM"][1] + 0x0, 0),
            "game_object_off": lambda m, f: m.go_off.add(f["inter"]),
        }
        for name, poke in pokes.items():
            m = self.machine()
            walk_up(m, command=COMMAND_BLESS)
            poke(m, m.followers[0])
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]), name)
            self.unreached(m)
            self.assertEqual(m.uc.mem_read(m.followers[0]["inter"] + INTER_ACTIVATED, 1)[0], 0, name)
            self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE, name)

    def test_interaction_disabled_by_the_hold_still_runs_the_command(self):
        # Follower.OnTaskChanged switches the interaction off with the hold, in the game itself too. StartCoroutine
        # only looks at the GameObject (0xB146A4), so switched off is the normal case on arrival.
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        self.assertIn(m.followers[0]["inter"], m.disabled)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        self.assertEqual(len(m.handler_args), 1)
        self.assertEqual(m.count("set_enabled"), 0)  # the game does not switch it on here either

    def test_interaction_disabled_before_the_hold_refuses(self):
        # Task with DisablePickUpInteraction (imprisoned, missionary, ...): the game offers nothing
        m = self.machine()
        m.disabled.add(m.followers[0]["inter"])
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.effects(), [])

    # --- Already blessed
    def test_blessed_follower_refuses_without_effects(self):
        m = self.machine()
        m.set_u8(m.followers[0]["info"] + INFO_RECEIVED_BLESSING, 1)
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["ALREADY_BLESSED"]))
        self.assertEqual(m.effects(), [])

    def test_blessed_flag_counts_only_for_bless(self):
        m = self.machine()
        m.set_u8(m.followers[0]["info"] + INFO_RECEIVED_BLESSING, 1)
        m.request(5, command=4)  # the helper now refuses GiveItem 3
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_second_tap_after_a_bless_refuses(self):
        m = self.running()
        m.close_game(reopen=0)
        m.run()
        n = len(m.effects())
        m.request(6, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (6, ST["REFUSED"], RS["ALREADY_BLESSED"]))
        self.assertEqual(len(m.effects()), n)

    def test_blessed_during_the_walk_cancels_and_releases(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.set_u8(m.followers[0]["info"] + INFO_RECEIVED_BLESSING, 1)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["ALREADY_BLESSED"]))
        self.unreached(m)
        self.assertEqual(m.count("reset_follower"), 1)

    # --- Daycare and sleep
    def daycare(self, m, ids):
        data = m.alloc(0x200)
        m.set_u64(data + 0x140, m.make_list(ids, 4))
        brain = m.alloc(0x40)
        m.set_u64(brain + 0x10, data)
        m.structures.append(brain)
        return brain, data

    def test_follower_in_daycare_is_refused_and_stays_there(self):
        m = self.machine()
        self.daycare(m, [3])
        self.daycare(m, [7])
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.effects(), [])
        args = dict(m.calls)["all_structures"]
        self.assertEqual((args["x0"] & 0xFFFFFFFF, args["x1"] & 0xFFFFFFFF, args["x2"] & 0xFFFFFFFF, args["x3"]),
                         (0x177, 0, 0, 0))

    def test_other_daycare_and_no_daycare_do_not_refuse(self):
        m = self.machine()
        self.daycare(m, [3])
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))

    def test_unreadable_daycare_refuses(self):
        # IsInDaycare throws on these nulls; the helper does not call it and refuses, but not as "busy"
        for name, poke in {"data_null": lambda m, b, d: m.set_u64(b + 0x10, 0),
                           "list_null": lambda m, b, d: m.set_u64(d + 0x140, 0)}.items():
            m = self.machine()
            brain, data = self.daycare(m, [3])
            poke(m, brain, data)
            m.request(5, command=COMMAND_BLESS)
            m.run()
            self.assertEqual(m.status(), (5, ST["REFUSED"], RS["NOT_AVAILABLE"]), name)
            self.assertEqual(m.effects(), [], name)

    def test_structure_manager_not_initialised_means_no_daycare(self):
        # The call would trigger the class constructor: unused class, so no building either
        m = self.machine()
        m.set_u32(m.kl["STRUCTURES"][0] + 0xE0, 0)
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]))
        self.assertEqual(m.count("all_structures"), 0)

    def test_sleep_task_without_brain_never_calls_is_awake(self):
        m = self.machine()
        task = m.alloc(0x100)
        m.set_u64(task, m.kl["SLEEP"][0])
        m.set_u32(task + TASK_STATE, TASK_STATE_DOING)
        m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, task)
        m.request(5, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.count("sleep_is_awake"), 0)

    # --- While the command runs: nothing interrupts it
    def test_nothing_interrupts_a_running_command(self):
        def menu(m):
            m.set_u64(m.kl["UIMENU"][1], m.make_list([0x7000F000], 8))
        pokes = {
            "epoch": lambda m: m.set_mb("EPOCH", 2),
            "stick": lambda m: m.axis.update(axis_h=1.0),
            "module_cancel": lambda m: m.set_mb("CANCEL_SEQ", 5),
            "menu": menu,
            "unlisted": lambda m: m.unlist(m.followers[0]),
            "conversation": lambda m: m.set_u8(PLAYER + 0x378, 1),
        }
        for name, poke in pokes.items():
            m = self.running()
            poke(m)
            n = len(m.effects())
            for _ in range(3):
                m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), name)
            self.assertEqual(len(m.effects()), n, name)

    def test_double_tap_runs_the_command_once(self):
        m = self.running()
        m.request(6, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        m.close_game(reopen=0)
        m.run()
        m.run()
        self.assertEqual(len(m.handler_args), 1)
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))

    def test_timeout_closes_through_the_game(self):
        limit = source_constant("kRunTimeoutFrames")
        self.assertGreater(limit / 60.0, 30.0)
        m = self.running()
        inter = m.followers[0]["inter"]
        m.set_mb("RUN_FRAMES", limit - 2)
        m.run()
        self.assertEqual(m.status()[1], ST["RUNNING"])
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["TIMEOUT"]))
        args = dict(m.calls)["close"]
        self.assertEqual((args["x0"], args["x1"] & 1, args["x2"] & 1, args["x3"] & 1, args["x4"]),
                         (inter, 1, 1, 0, 0))
        self.assertFalse(m.paused)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE)
        self.assertEqual(m.count("close"), 1)
        m.run()
        self.assertEqual(m.count("close"), 1)

    def test_interaction_destroyed_while_running_only_unpauses(self):
        for name, poke in {"inter": lambda m, f: m.set_u64(f["inter"] + 0x10, 0),
                           "follower": lambda m, f: m.set_u64(f["obj"] + 0x10, 0)}.items():
            m = self.running()
            poke(m, m.followers[0])
            n = len(m.effects())
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]), name)
            self.assertEqual(m.effects()[n:], ["sim_unpause"], name)
            self.assertFalse(m.paused, name)

    def test_scene_gone_after_an_epoch_change_is_stale(self):
        m = self.running()
        m.set_mb("EPOCH", 0)
        m.set_u64(m.followers[0]["inter"] + 0x10, 0)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["STALE_SESSION"]))

    # --- Close() reopens the interaction: the game leads the Lamb to the wheel again
    def reopened(self):
        m = self.running()
        m.close_game(reopen=1)
        return m

    def test_reopen_never_reaches_the_wheel(self):
        # At the first hook after that, the callback that opened the wheel is gone: EndGoToAndStop skips
        # null (0x1CFCFCC)
        m = self.reopened()
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]))
        self.assertEqual(m.get_u64(PLAYER + PF_GOTO_CB), 0)
        self.assertNotEqual(m.get_u64(PLAYER + PF_ABORT_CB), 0)

    def test_reopen_closes_only_after_the_coroutine_cached(self):
        m = self.reopened()
        m.run()                    # seen; at the end of the frame the coroutine swaps the task
        self.assertEqual(m.count("close"), 0)
        m.run()                    # task seen; at the end of this frame it caches
        self.assertEqual(m.count("close"), 0)
        self.assertEqual(m.status()[1], ST["RUNNING"])
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        self.assertEqual(m.reset_before_cache, 0)
        args = dict(m.calls)["close"]
        self.assertEqual((args["x0"], args["x1"] & 1, args["x2"] & 1, args["x3"] & 1),
                         (m.followers[0]["inter"], 1, 1, 0))
        self.assertTrue(m.followers[0]["animator_on"])
        self.assertEqual(m.uc.mem_read(PLAYER + PF_GOTO_FLAG, 1)[0], 0)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE)
        self.assertFalse(m.paused)

    def test_reopen_walk_that_ended_early_is_closed_too(self):
        # The Lamb is already at the target: the walk ends in InActive without a callback, Close frees it
        m = self.reopened()
        m.run()
        m.end_walk()
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_INACTIVE)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        self.assertEqual(m.count("abort_go_to"), 0)
        self.assertEqual(m.count("close"), 1)
        self.assertEqual(m.get_u32(m.state + 0x30), STATE_IDLE)

    def test_reopen_gives_up_waiting_for_the_task_and_closes(self):
        m = self.reopened()
        m.coroutine_runs = False
        limit = source_constant("kHoldTimeoutFrames")
        m.coroutines = []
        m.run()
        m.set_mb("RUN_FRAMES", limit - 1)
        m.run()
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))
        self.assertEqual(m.count("close"), 1)

    def test_game_walk_of_another_kind_is_left_alone(self):
        # Re-entry into another branch: the abort callback does not belong to this interaction
        for name, poke in {"other_method": lambda m: m.set_u64(m.get_u64(PLAYER + PF_ABORT_CB) + 0x10, MAIN + 0x1000),
                           "other_inter": lambda m: m.set_u64(m.get_u64(m.get_u64(PLAYER + PF_ABORT_CB) + 0x20) + 0x10, 0x70008000),
                           "no_abort_cb": lambda m: m.set_u64(PLAYER + PF_ABORT_CB, 0)}.items():
            m = self.reopened()
            poke(m)
            cb = m.get_u64(PLAYER + PF_GOTO_CB)
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), name)
            self.assertEqual(m.get_u64(PLAYER + PF_GOTO_CB), cb, name)
            self.assertEqual(m.count("close") + m.count("abort_go_to"), 0, name)

    def test_interaction_destroyed_during_reopen_only_unpauses(self):
        m = self.reopened()
        m.run()
        m.set_u64(m.followers[0]["inter"] + 0x10, 0)
        n = len(m.effects())
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_MISSING"]))
        self.assertEqual(m.effects()[n:], ["sim_unpause"])



class GeneratorTests(unittest.TestCase):
    def test_host_instruction_mask_is_mirrored(self):
        # The host (mod_load_plan.cpp) rejects 0xD4xxxxxx and 0xD5xxxxxx except NOP
        for word in (0xD4000001, 0xD4000002, 0xD4000003, 0xD4200000, 0xD5033FBF, 0xD53B4220,
                     0xD5033F9F, 0xD50343DF):
            with self.assertRaises(ValueError, msg=hex(word)):
                gen.check_instructions(struct.pack("<I", word))
        gen.check_instructions(struct.pack("<II", 0xD503201F, 0xD10483FF))  # NOP and sub sp

    def test_other_allocated_sections_fail_generation(self):
        # Statics in the helper would otherwise silently drop out on binary export
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            for body in ("static volatile unsigned counter = 5;\n"
                         "void cotl_action_tick(void) { counter++; }\n",       # .data
                         "static volatile unsigned counter;\n"
                         "void cotl_action_tick(void) { counter++; }\n"):      # .bss
                src = os.path.join(tmp, "bad.c")
                with open(src, "w") as f:
                    f.write('__asm__(".section .text.entry,\\"ax\\"\\n.global cotl_action_entry\\n'
                            'cotl_action_entry:\\n ret\\n.text\\n");\n' + body)
                with self.assertRaises(ValueError):
                    gen.compile_helper(src)

    def test_real_helper_has_only_text(self):
        gen.compile_helper()  # must not throw


@unittest.skipUnless(os.path.exists(REAL_MAIN), "work/exefs/main is missing")
class RealImageTests(unittest.TestCase):
    def test_walk_end_states_match_real_main(self):
        # The stubs set the state like these instructions; if the game reverses them, it shows up here
        with open(REAL_MAIN, "rb") as f:
            _, image = gen.load_main(f.read())

        def word(rva):
            return struct.unpack_from("<I", image, rva)[0]
        # EndGoToAndStop: ldrb w8, [x19, #0x2e9]; cmp w8, #0; mov w8, #13; csel w1, w8, wzr, eq
        self.assertEqual([word(0x1CFCF84 + i * 4) for i in range(4)],
                         [0x394BA668, 0x7100011F, 0x528001A8, 0x1A9F0101])
        # AbortGoTo: mov w1, wzr before set_CURRENT_STATE, so always Idle
        self.assertEqual(word(0x1CFC1F0), 0x2A1F03E1)

    def test_game_function_addresses_match_the_dump(self):
        path = os.path.join(ROOT, "work", "il2cpp", "script.json")
        if not os.path.exists(path):
            self.skipTest("work/il2cpp/script.json is missing")
        with open(path, encoding="utf-8") as f:
            known = {m["Address"]: m["Name"] for m in json.load(f)["ScriptMethod"]}
        for name, rva in GAME_FUNCS.values():
            self.assertEqual(known.get(rva), name, hex(rva))

    def test_command_facts_match_real_main(self):
        # What the helper and the stubs rely on; RUNTIME_FUNCS have no name in the dump
        with open(REAL_MAIN, "rb") as f:
            _, image = gen.load_main(f.read())

        def word(rva):
            return struct.unpack_from("<I", image, rva)[0]
        # il2cpp_array_new_specific-Thunk: mov w1, w1; b 0x46E330
        self.assertEqual([word(0x4806F0), word(0x4806F4)], [0x2A0103E1, 0x17FFB70F])
        # Close: strb wzr, [x19, #0x129], the signal for "command finished"
        self.assertEqual(word(0x2C606E0), 0x3904A67F)
        # Close(): mov w3, #1, reopens; b__5: mov w3, wzr, does not reopen
        self.assertEqual(word(0x2C688E8), 0x52800023)
        self.assertEqual(word(0x2C70B40), 0x2A1F03E3)
        # EndGoToAndStop: cbz x8, skips a missing GoToCallback
        self.assertEqual(word(0x1CFCFCC), 0xB40000A8)

    def test_hook_bytes_and_layout_match_real_main(self):
        with open(REAL_MAIN, "rb") as f:
            header, image = gen.load_main(f.read())
        self.assertEqual(image[HOOK:HOOK + 4].hex(), "ff8304d1")
        self.assertEqual(header["build_id"], BUILD)
        self.assertEqual(header["segments"], [(0, 90160584), (90161152, 23461888), (113623040, 7687040)])
        self.assertEqual(header["bss_size"], 3789952)
        gen.generate(image)  # accepts the real image


class SimpleCommandTests(unittest.TestCase):
    """Task 7, "simple" group: the game's refusals before any effect, and waking up."""

    PLAIN = [CMD_EXTORT, CMD_DANCE, CMD_INTIMIDATE, CMD_BRIBE, CMD_REASSURE, CMD_BULLY, CMD_ROMANCE, CMD_PET_DOG,
             CMD_SLEEP, CMD_WORSHIP]

    def machine(self):
        m = Machine()
        m.pos[m.followers[0]["obj"]] = (10.0, 5.0, 0.25)
        return m

    def sleeper(self, m, key="SLEEP", high=False):
        f = m.followers[0]
        t = m.alloc_high(0x100) if high else m.alloc(0x100)
        m.set_u64(t, m.kl[key][0])
        m.set_u32(t + TASK_STATE, TASK_STATE_DOING)
        m.set_u64(t + TASK_BRAIN, f["brain"])
        m.set_u64(f["brain"] + BRAIN_TASK, t)
        m.awake = 0
        return t

    def trait(self, m, trait, cult=False):
        lst = m.make_list([trait], 4)
        if cult:
            m.set_u64(m.dm + DM_CULT_TRAITS, lst)
        else:
            m.set_u64(m.followers[0]["info"] + 0x1E8, lst)

    def unreached(self, m):
        self.assertEqual(m.handler_args, [])
        for name in ("sim_pause", "conversation_new", "array_new"):
            self.assertEqual(m.count(name), 0, name)

    def refused(self, m, command, reason):
        m.request(5, command=command)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS[reason]), command)
        self.assertEqual(m.effects(), [], command)

    def accepted(self, m, command):
        m.request(5, command=command)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), command)

    # --- Daily flags
    def test_daily_flag_refuses_before_any_effect(self):
        for command, (flag, reason) in DAILY_FLAGS.items():
            m = self.machine()
            m.set_u8(m.followers[0]["info"] + flag, 1)
            self.refused(m, command, reason)

    def test_flag_of_another_command_does_not_refuse(self):
        for command, (flag, _) in DAILY_FLAGS.items():
            m = self.machine()
            for other, (other_flag, _) in DAILY_FLAGS.items():
                if other_flag != flag:
                    m.set_u8(m.followers[0]["info"] + other_flag, 1)
            self.accepted(m, command)

    def test_flag_set_during_the_walk_cancels_and_releases(self):
        for command, (flag, reason) in DAILY_FLAGS.items():
            m = self.machine()
            walk_up(m, command=command)
            m.set_u8(m.followers[0]["info"] + flag, 1)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS[reason]), command)
            self.unreached(m)
            self.assertEqual(m.count("reset_follower"), 1, command)

    def test_second_tap_after_the_command_refuses(self):
        for command, (flag, reason) in DAILY_FLAGS.items():
            if command == CMD_BRIBE:
                continue  # flag only in the callback
            m = self.machine()
            walk_up(m, command=command)
            m.end_walk()
            m.run()
            m.close_game(reopen=0)
            m.run()
            n = len(m.effects())
            m.request(6, command=command)
            m.run()
            self.assertEqual(m.status(), (6, ST["REFUSED"], RS[reason]), command)
            self.assertEqual(len(m.effects()), n, command)

    # --- The handler runs exactly once
    def test_each_command_reaches_the_handler_once(self):
        for command in self.PLAIN:
            m = self.machine()
            walk_up(m, command=command)
            m.end_walk()
            m.request(6, command=command)  # double tap on arrival
            m.run()
            m.request(7, command=command)
            m.run()
            self.assertEqual(len(m.handler_args), 1, command)
            self.assertEqual(m.handler_args[0][3], command)
            self.assertEqual(m.handler_args[0][2], 1)

    def test_coroutine_commands_wait_for_close_either_way(self):
        for command in (CMD_EXTORT, CMD_DANCE, CMD_INTIMIDATE, CMD_BRIBE, CMD_REASSURE, CMD_BULLY,
                        CMD_ROMANCE, CMD_PET_DOG):
            m = self.machine()
            walk_up(m, command=command)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), command)
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), command)
            m.close_game(reopen=0)  # level-up: Close(1, 1, 0)
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)
            self.assertEqual(m.count("close"), 0, command)

            m = self.machine()
            walk_up(m, command=command)
            m.end_walk()
            m.run()
            m.close_game(reopen=1)  # Close(): the wheel must not open
            for _ in range(3):
                m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)
            self.assertEqual(m.count("close"), 1, command)
            self.assertEqual(m.count("abort_go_to"), 1, command)
            self.assertEqual(m.calls[m.names().index("close")][1]["x3"], 0, command)

    def test_sleep_and_worship_close_inside_the_handler(self):
        for command in (CMD_SLEEP, CMD_WORSHIP):
            m = self.machine()
            walk_up(m, command=command)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), command)
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)
            self.assertEqual(m.count("close"), 0, command)
            self.assertEqual(m.count("sim_unpause"), 0, command)

    def test_missing_brain_parts_stop_before_the_handler(self):
        # The getters of Inspired, Bribed & Co. read via BrainStats, SkinName via BrainInfo
        for off in (BRAIN_BRAIN_INFO, BRAIN_STATS):
            m = self.machine()
            walk_up(m, command=CMD_DANCE)
            m.set_u64(m.followers[0]["brain"] + off, 0)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]), off)
            self.unreached(m)
            self.assertEqual(m.count("reset_follower"), 1)

    def test_brain_parts_holding_no_info_stop_before_the_handler(self):
        m = self.machine()
        walk_up(m, command=CMD_DANCE)
        m.set_u64(m.followers[0]["brain_stats"] + 0x18, 0)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]))
        self.unreached(m)

    # --- Bribing costs 3 gold (GetItemQuantity 0x1BBD440 <= 2 refuses, 0x2C5D8C0)
    def test_bribe_needs_three_gold(self):
        for items, ok in (([(ITEM_GOLD, 3)], True), ([(ITEM_GOLD, 2)], False), ([], False),
                          ([(1, 50)], False), ([(1, 50), (ITEM_GOLD, 3)], True),
                          ([(ITEM_GOLD, 2), (ITEM_GOLD, 50)], False)):
            m = self.machine()
            m.set_items(items)
            if ok:
                self.accepted(m, CMD_BRIBE)
            else:
                self.refused(m, CMD_BRIBE, "NOT_ENOUGH_GOLD")

    def test_bribe_with_unreadable_inventory_is_not_available(self):
        m = self.machine()
        m.set_u64(m.dm + DM_ITEMS, 0)
        self.refused(m, CMD_BRIBE, "NOT_AVAILABLE")
        m = self.machine()
        m.set_items([None, (ITEM_GOLD, 5)])
        self.refused(m, CMD_BRIBE, "NOT_AVAILABLE")

    def test_gold_is_only_read_for_bribe(self):
        m = self.machine()
        m.set_u64(m.dm + DM_ITEMS, 0)
        self.accepted(m, CMD_DANCE)

    def test_gold_spent_during_the_walk_cancels(self):
        m = self.machine()
        walk_up(m, command=CMD_BRIBE)
        m.set_items([(ITEM_GOLD, 1)])
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_ENOUGH_GOLD"]))
        self.unreached(m)

    # --- Blizzard: FollowerBrain.CanWork 0x2998ED0
    def test_worship_refused_in_a_blizzard(self):
        m = self.machine()
        m.set_u32(m.dm + DM_WEATHER, 1)
        self.refused(m, CMD_WORSHIP, "TOO_COLD")

    def test_worship_passes_when_the_follower_can_work(self):
        def own(trait):
            return lambda self_, m: self_.trait(m, trait)
        def cult(trait):
            return lambda self_, m: self_.trait(m, trait, cult=True)
        def snowman(self_, m):
            m.set_u8(m.followers[0]["info"] + INFO_IS_SNOWMAN, 1)
        for poke in (cult(0x57), own(0x79), cult(0x79), own(0x5A), cult(0x5A), snowman):
            m = self.machine()
            m.set_u32(m.dm + DM_WEATHER, 1)
            poke(self, m)
            self.accepted(m, CMD_WORSHIP)
        m = self.machine()
        self.trait(m, 0x57)  # 0x57 only counts as a cult trait
        m.set_u32(m.dm + DM_WEATHER, 1)
        self.refused(m, CMD_WORSHIP, "TOO_COLD")
        m = self.machine()
        m.set_u32(m.dm + DM_WEATHER, 2)  # different weather
        self.accepted(m, CMD_WORSHIP)

    def test_blizzard_only_stops_worship_and_wake_up(self):
        for command in (CMD_DANCE, CMD_SLEEP):
            m = self.machine()
            m.set_u32(m.dm + DM_WEATHER, 1)
            self.accepted(m, command)

    # --- TimedAction: the handler ran, the task is still there (TimedAnimation 0x2993D30)
    def test_wake_up_again_during_the_timed_action_refuses(self):
        for key in ("SLEEP", "SLEEP_BED_REST"):
            m = self.machine()
            task = self.sleeper(m, key)
            walk_up(m, command=CMD_WAKE_UP)
            m.end_walk()
            m.run()
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), key)
            m.close_game(reopen=0)
            m.run()
            self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), task, key)
            n = len(m.effects())
            m.request(6, command=CMD_WAKE_UP)
            m.run()
            self.assertEqual(m.status(), (6, ST["REFUSED"], RS["FOLLOWER_LOCKED"]), key)
            self.assertEqual(len(m.effects()), n, key)
            self.assertEqual(len(m.handler_args), 1, key)
            # The animation is over: back to normal
            m.set_u32(m.followers[0]["state_machine"] + 0x30, STATE_IDLE)
            m.request(7, command=CMD_WAKE_UP)
            m.run()
            self.assertEqual(m.status(), (7, ST["WALKING"], RS["NONE"]), key)

    def test_timed_action_refuses_every_command(self):
        for command in self.PLAIN + [COMMAND_BLESS]:
            m = self.machine()
            m.set_u32(m.followers[0]["state_machine"] + 0x30, STATE_TIMED_ACTION)
            self.refused(m, command, "FOLLOWER_LOCKED")

    def test_timed_action_starting_during_the_walk_stops_wake_up(self):
        # TimedAnimation ran a pending callback on our stack (0x2993DA0)
        m = self.machine()
        self.sleeper(m)
        walk_up(m, command=CMD_WAKE_UP)
        m.set_u32(m.followers[0]["state_machine"] + 0x30, STATE_TIMED_ACTION)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))
        self.unreached(m)

    def test_missing_state_machine_refuses(self):
        for command in self.PLAIN:
            m = self.machine()
            m.set_u64(m.followers[0]["obj"] + FOLLOWER_STATE, 0)
            self.refused(m, command, "NOT_AVAILABLE")
            m = self.machine()
            m.set_u64(m.followers[0]["state_machine"] + 0x10, 0)
            self.refused(m, command, "NOT_AVAILABLE")

    def test_parts_lost_during_the_walk_stop_before_the_handler(self):
        # TimedAnimation throws on null Spine (0x2993D50) and null State (0x2993D64)
        for off in (FOLLOWER_STATE, FOLLOWER_SPINE):
            m = self.machine()
            self.sleeper(m)
            walk_up(m, command=CMD_WAKE_UP)
            m.set_u64(m.followers[0]["obj"] + off, 0)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]), off)
            self.unreached(m)

    # --- Drowsy: DoDrowsySleepAfterCommandGiven throws without NotificationCentre.Instance (0x2C61C1C).
    # Sleep and Worship go there (0x2C5ED0C, 0x2C5F07C), no other command.
    def centre(self, m):
        centre = m.alloc(0x100)
        m.set_u64(centre + 0x10, 1)
        m.set_u64(m.kl["NOTIFICATION"][1] + NOTIFICATION_INSTANCE, centre)

    def test_drowsy_needs_the_notification_centre(self):
        for command in (CMD_SLEEP, CMD_WORSHIP):
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            self.refused(m, command, "NOT_AVAILABLE")
            m = self.machine()
            self.trait(m, TRAIT_DROWSY, cult=True)
            self.refused(m, command, "NOT_AVAILABLE")
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            self.centre(m)
            self.accepted(m, command)
            m = self.machine()  # without the trait the handler never goes there
            self.accepted(m, command)

    def test_drowsy_rechecks_on_arrival(self):
        for command in (CMD_SLEEP, CMD_WORSHIP):
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            self.centre(m)
            walk_up(m, command=command)
            m.set_u64(m.kl["NOTIFICATION"][1] + NOTIFICATION_INSTANCE, 0)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]), command)
            self.unreached(m)

    def test_worship_names_the_cold_before_drowsy(self):
        m = self.machine()
        self.trait(m, TRAIT_DROWSY)
        m.set_u32(m.dm + DM_WEATHER, 1)
        self.refused(m, CMD_WORSHIP, "TOO_COLD")

    def test_drowsy_does_not_touch_the_other_commands(self):
        for command in self.PLAIN + [COMMAND_BLESS]:
            if command in (CMD_SLEEP, CMD_WORSHIP):
                continue
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            self.accepted(m, command)

    # --- Extort: the branch calls AudioManager.get_Instance on our stack (0x2C5F400) and throws
    # if that returns nothing (0x2C6036C). The flag (0x2C5F3E0) and coroutine (0x2C5F3F8) have already run by then,
    # hence the guard. The getter otherwise creates it itself; if it is alive, that is moot.
    def test_extort_says_already_extorted_before_anything_else(self):
        m = self.machine()
        m.set_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE, 0)
        m.set_u8(m.followers[0]["info"] + INFO_PAID_TITHES, 1)
        self.refused(m, CMD_EXTORT, "ALREADY_EXTORTED")

    def test_extort_needs_the_audio_manager(self):
        m = self.machine()
        m.set_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE, 0)
        self.refused(m, CMD_EXTORT, "NOT_AVAILABLE")
        m = self.machine()
        m.set_u64(m.get_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE) + 0x10, 0)  # Unity object destroyed
        self.refused(m, CMD_EXTORT, "NOT_AVAILABLE")

    def test_extort_rechecks_the_audio_manager_on_arrival(self):
        m = self.machine()
        walk_up(m, command=CMD_EXTORT)
        m.set_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE, 0)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NOT_AVAILABLE"]))
        self.unreached(m)
        self.assertEqual(m.count("reset_follower"), 1)

    def test_only_extort_asks_for_the_audio_manager(self):
        for command in self.PLAIN + [COMMAND_BLESS]:
            if command == CMD_EXTORT:
                continue
            m = self.machine()
            m.set_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE, 0)
            self.accepted(m, command)

    def test_extort_flag_is_set_by_the_handler_not_by_us(self):
        m = self.machine()
        flag = m.followers[0]["info"] + INFO_PAID_TITHES
        walk_up(m, command=CMD_EXTORT)
        self.assertEqual(m.uc.mem_read(flag, 1)[0], 0)
        m.end_walk()
        m.run()
        self.assertEqual(len(m.handler_args), 1)
        self.assertEqual(m.handler_args[0][2:4], (1, CMD_EXTORT))
        self.assertEqual(m.uc.mem_read(flag, 1)[0], 1)

    # --- Waking up: like OnInteract without ManualControl (0x2C59D98)
    def test_wake_up_with_a_task_above_4_gib_runs(self):
        # The pointer in RUN_TASK is 8 bytes wide; its upper half must not be another word
        m = self.machine()
        task = self.sleeper(m, high=True)
        self.assertGreater(task >> 32, 0)
        walk_up(m, command=CMD_WAKE_UP)
        self.assertEqual(m.get_u64(MAILBOX + MB["RUN_TASK"]), task)
        m.end_walk()
        m.run()
        self.assertEqual(len(m.handler_args), 1)
        self.assertEqual(m.handler_args[0][3], CMD_WAKE_UP)
        m.run()
        self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]))

    def test_wake_up_holds_without_manual_control(self):
        for key in ("SLEEP", "SLEEP_BED_REST"):
            m = self.machine()
            task = self.sleeper(m, key)
            walk_up(m, command=CMD_WAKE_UP)
            calls = {n: a for n, a in m.calls}
            self.assertEqual(calls["cache_and_set"]["x2"], 0, key)
            self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), task, key)
            self.assertEqual(m.effects(), ["cache_and_set", "go_to"], key)
            m.end_walk()
            m.run()
            self.assertEqual(len(m.handler_args), 1, key)
            self.assertEqual(m.handler_args[0][3], CMD_WAKE_UP, key)
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), key)
            self.assertEqual(m.count("close"), 0, key)
            self.assertEqual(m.count("reset_follower"), 0, key)

    def test_wake_up_waits_for_the_coroutine_like_a_hold(self):
        m = self.machine()
        self.sleeper(m)
        m.request(5, command=CMD_WAKE_UP)
        m.run()
        self.assertEqual(m.effects(), ["cache_and_set"])
        m.run()
        self.assertEqual(m.effects(), ["cache_and_set"])
        m.run()
        self.assertEqual(m.effects(), ["cache_and_set", "go_to"])
        self.assertEqual(m.reset_before_cache, 0)

    def test_wake_up_refused_when_not_sleeping(self):
        m = self.machine()
        self.refused(m, CMD_WAKE_UP, "NOT_AVAILABLE")
        m = self.machine()
        self.sleeper(m)
        m.awake = 1
        self.refused(m, CMD_WAKE_UP, "NOT_AVAILABLE")

    def test_other_commands_still_refuse_sleepers(self):
        for command in self.PLAIN:
            for key in ("SLEEP", "SLEEP_BED_REST"):
                m = self.machine()
                self.sleeper(m, key)
                self.refused(m, command, "FOLLOWER_LOCKED")

    def test_wake_up_refused_while_hibernating(self):
        for trait, season in ((0x5E, 1), (0x5F, 0)):
            for cult in (False, True):
                m = self.machine()
                self.sleeper(m)
                self.trait(m, trait, cult)
                m.set_u32(m.dm + DM_SEASON, season)
                self.refused(m, CMD_WAKE_UP, "SLEEPS_UNTIL_SEASON")

    def test_wake_up_passes_out_of_season_or_with_work_through_night(self):
        m = self.machine()
        self.sleeper(m)
        self.trait(m, 0x5E)
        m.set_u32(m.dm + DM_SEASON, 0)
        self.accepted(m, CMD_WAKE_UP)
        m = self.machine()
        self.sleeper(m)
        self.trait(m, 0x5F)
        m.set_u32(m.dm + DM_SEASON, 1)
        self.accepted(m, CMD_WAKE_UP)
        m = self.machine()
        self.sleeper(m)
        self.trait(m, 0x5E)
        m.set_u32(m.dm + DM_SEASON, 1)
        m.set_u32(m.dm + DM_CURRENT_DAY, 12)
        m.set_u32(m.followers[0]["info"] + INFO_WORK_THROUGH_NIGHT, 12)
        self.accepted(m, CMD_WAKE_UP)

    def test_wake_up_refused_in_a_blizzard(self):
        m = self.machine()
        self.sleeper(m)
        m.set_u32(m.dm + DM_WEATHER, 1)
        self.refused(m, CMD_WAKE_UP, "TOO_COLD")

    def test_wake_up_refusal_on_arrival_releases(self):
        m = self.machine()
        task = self.sleeper(m)
        walk_up(m, command=CMD_WAKE_UP)
        m.set_u32(m.dm + DM_WEATHER, 1)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["TOO_COLD"]))
        self.unreached(m)
        self.assertEqual(m.count("reset_follower"), 1)
        self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), task)

    def test_wake_up_cancelled_walk_keeps_the_sleep(self):
        m = self.machine()
        task = self.sleeper(m)
        walk_up(m, command=CMD_WAKE_UP)
        m.axis["axis_h"] = 1.0
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["PLAYER_MOVED"]))
        self.assertEqual(m.count("reset_follower"), 1)
        self.assertEqual(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK), task)
        self.assertTrue(m.followers[0]["animator_on"])

    def test_wake_up_cancels_when_the_follower_wakes_during_the_walk(self):
        m = self.machine()
        self.sleeper(m)
        walk_up(m, command=CMD_WAKE_UP)
        m.awake = 1
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))
        # The sleep task continues, but ResetFollower is only for our hold: animator back
        self.assertEqual(m.count("reset_follower"), 0)
        self.assertEqual(m.count("set_enabled"), 1)
        self.assertTrue(m.followers[0]["animator_on"])
        self.assertTrue(m.get_u64(m.followers[0]["brain"] + BRAIN_TASK) != 0)

    def test_wake_up_cancels_when_the_task_changes_during_the_walk(self):
        m = self.machine()
        self.sleeper(m)
        walk_up(m, command=CMD_WAKE_UP)
        t = m.alloc(0x100)
        m.set_u64(t, m.kl["SLEEP"][0])
        m.set_u32(t + TASK_STATE, TASK_STATE_DOING)
        m.set_u64(t + TASK_BRAIN, m.followers[0]["brain"])
        m.set_u64(m.followers[0]["brain"] + BRAIN_TASK, t)
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["FOLLOWER_LOCKED"]))
        self.assertEqual(m.count("reset_follower"), 0)

    def test_wake_up_for_a_sleep_task_without_brain_never_asks_the_game(self):
        m = self.machine()
        task = self.sleeper(m)
        m.set_u64(task + TASK_BRAIN, 0)
        walk_up(m, command=CMD_WAKE_UP)
        self.assertEqual(m.count("sleep_is_awake"), 0)

    def test_sleep_has_no_refusal_of_its_own(self):
        m = self.machine()
        m.set_u32(m.dm + DM_WEATHER, 1)
        m.set_u64(m.dm + DM_ITEMS, 0)
        for flag, _ in DAILY_FLAGS.values():
            m.set_u8(m.followers[0]["info"] + flag, 1)
        self.accepted(m, CMD_SLEEP)



# Submenus Eat 25, Drink 89, Work 1 (tools/cotl/README.md, Food and work)
CMD_WORK, CMD_EAT, CMD_DRINK, CMD_BUILD = 1, 25, 89, 28
MEALS = [31, 32, 33, 34, 35, 36, 37, 38, 60, 61, 65, 66, 67, 68, 69, 70, 71, 72, 84, 87, 105, 112, 133, 134, 135]
DRINKS = [90, 91, 92, 93, 94, 95, 96, 127, 128, 129, 130, 177]
ROLES = [14, 17, 27, 30, 47, 58, 64, 81, 88, 113, 116, 117, 124, 126, 175]
BUILD_SITE, BUILD_SITE_PROJECT = 31, 118  # StructureBrain.TYPES


class EatAndWorkTests(unittest.TestCase):
    """Task 7, group "Food and work": sub-commands as [command, parent], refusals up front."""

    def machine(self, sites=True):
        m = Machine()
        m.pos[m.followers[0]["obj"]] = (10.0, 5.0, 0.25)
        if sites:
            m.structures_by_type[BUILD_SITE] = [m.alloc(0x40)]
        return m

    def trait(self, m, trait):
        m.set_u64(m.followers[0]["info"] + 0x1E8, m.make_list([trait], 4))

    def refused(self, m, command, parent, reason):
        m.request(5, command=command, parent=parent)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS[reason]), (command, parent))
        self.assertEqual(m.effects(), [], (command, parent))

    def accepted(self, m, command, parent):
        m.request(5, command=command, parent=parent)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), (command, parent))

    def pairs(self):
        return ([(c, CMD_EAT) for c in MEALS] + [(c, CMD_DRINK) for c in DRINKS] +
                [(c, CMD_WORK) for c in ROLES + [CMD_BUILD]])

    def test_sub_command_goes_to_the_handler_before_its_parent(self):
        # The wheel passes the chosen entry first (OnFollowerCommandFinalized reads [0], then [1])
        for command, parent in self.pairs():
            m = self.machine()
            walk_up(m, command=command, parent=parent)
            m.end_walk()
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), command)
            self.assertEqual(m.handler_arrays, [[command, parent]], command)
            self.assertEqual(m.handler_args[0][2], 2, command)
            # Close(1, 1, 0) in the handler: done in the next frame, without a Close of its own
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)
            self.assertEqual(m.count("close"), 0, command)

    def test_commands_without_parent_keep_a_single_element(self):
        m = self.machine()
        walk_up(m, command=COMMAND_BLESS)
        m.end_walk()
        m.run()
        self.assertEqual(m.handler_arrays, [[COMMAND_BLESS]])

    def test_parent_alone_is_refused(self):
        # [1], [25], [89] alone always end with CloseAndSpeak in the game
        for parent in (CMD_WORK, CMD_EAT, CMD_DRINK):
            self.refused(self.machine(), parent, 0, "NOT_AVAILABLE")

    def test_sub_command_needs_its_own_parent(self):
        for command, parent in ((31, 0), (90, 0), (14, 0), (CMD_BUILD, 0), (31, CMD_WORK), (90, CMD_EAT),
                                (14, CMD_DRINK), (31, CMD_DRINK), (COMMAND_BLESS, CMD_EAT), (31, 3),
                                (CMD_EAT, CMD_EAT)):
            self.refused(self.machine(), command, parent, "NOT_AVAILABLE")

    def test_worship_keeps_its_single_element_path(self):
        self.accepted(self.machine(), CMD_WORSHIP, 0)

    def test_build_needs_a_site(self):
        m = self.machine(sites=False)
        self.refused(m, CMD_BUILD, CMD_WORK, "NO_BUILD_SITES")
        kinds = sorted(a["x0"] & 0xFFFFFFFF for n, a in m.calls if n == "all_structures")
        self.assertIn(BUILD_SITE, kinds)
        self.assertIn(BUILD_SITE_PROJECT, kinds)
        for n, a in m.calls:
            if n == "all_structures":
                self.assertEqual((a["x1"] & 0xFFFFFFFF, a["x2"] & 0xFFFFFFFF, a["x3"]), (0, 0, 0))
        m = self.machine(sites=False)
        m.structures_by_type[BUILD_SITE_PROJECT] = [m.alloc(0x40)]
        self.accepted(m, CMD_BUILD, CMD_WORK)
        # Without a class constructor that has run there are no buildings, and the call would trigger it
        m = self.machine()
        m.set_u32(m.kl["STRUCTURES"][0] + 0xE0, 0)
        self.refused(m, CMD_BUILD, CMD_WORK, "NO_BUILD_SITES")
        self.assertEqual(m.count("all_structures"), 0)

    def test_build_site_gone_during_the_walk_cancels(self):
        m = self.machine()
        walk_up(m, command=CMD_BUILD, parent=CMD_WORK)
        m.structures_by_type[BUILD_SITE] = []
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["CANCELLED"], RS["NO_BUILD_SITES"]))
        self.assertEqual(m.handler_args, [])

    def test_meals_and_drinks_need_the_structure_manager(self):
        # CancelTargetedMeal/-Drink go through StructureManager.AccumulateOnAllStructures
        for command, parent in ((31, CMD_EAT), (90, CMD_DRINK)):
            m = self.machine()
            m.set_u32(m.kl["STRUCTURES"][0] + 0xE0, 0)
            self.refused(m, command, parent, "NOT_AVAILABLE")

    def test_roles_refused_in_a_blizzard_but_build_is_not(self):
        for command in ROLES:
            m = self.machine()
            m.set_u32(m.dm + DM_WEATHER, 1)
            self.refused(m, command, CMD_WORK, "TOO_COLD")
        m = self.machine()
        m.set_u32(m.dm + DM_WEATHER, 1)
        self.accepted(m, CMD_BUILD, CMD_WORK)
        for command, parent in ((31, CMD_EAT), (90, CMD_DRINK)):
            m = self.machine()
            m.set_u32(m.dm + DM_WEATHER, 1)
            self.accepted(m, command, parent)
        m = self.machine()
        m.set_u32(m.dm + DM_WEATHER, 1)
        m.set_u8(m.followers[0]["info"] + INFO_IS_SNOWMAN, 1)
        self.accepted(m, 14, CMD_WORK)

    def test_drowsy_guard_covers_every_sub_command(self):
        # With two entries every sub-command runs through the Drowsy block
        for command, parent in self.pairs():
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            self.refused(m, command, parent, "NOT_AVAILABLE")
            m = self.machine()
            self.trait(m, TRAIT_DROWSY)
            centre = m.alloc(0x100)
            m.set_u64(centre + 0x10, 1)
            m.set_u64(m.kl["NOTIFICATION"][1] + NOTIFICATION_INSTANCE, centre)
            self.accepted(m, command, parent)

    def test_double_tap_runs_the_sub_command_once(self):
        for command, parent in ((31, CMD_EAT), (90, CMD_DRINK), (14, CMD_WORK), (CMD_BUILD, CMD_WORK)):
            m = self.machine()
            walk_up(m, command=command, parent=parent)
            m.request(6, command=command, parent=parent)
            m.run()
            self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
            m.end_walk()
            m.run()
            m.run()
            m.run()
            self.assertEqual(len(m.handler_args), 1, command)
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)

    def test_run_keeps_its_own_parent(self):
        m = self.machine()
        walk_up(m, command=31, parent=CMD_EAT)
        self.assertEqual(m.mb("RUN_PARENT"), CMD_EAT)
        m.request(6, command=COMMAND_BLESS, parent=0)
        m.run()
        self.assertEqual(m.mb("ST_BUSY_SEQ"), 6)
        m.end_walk()
        m.run()
        self.assertEqual(m.handler_arrays, [[31, CMD_EAT]])
        # A later request without a parent sends a single entry again
        m.run()
        self.assertEqual(m.status()[1], ST["DONE"])
        m.request(7, command=COMMAND_BLESS)
        m.run()
        self.assertEqual(m.mb("RUN_PARENT"), 0)




# Gifts (tools/cotl/README.md, Gifts): Gift 11, Give Pet 148, necklaces 62/98/99, 120/122 also top level
CMD_GIFT, CMD_GIVE_PET, CMD_REMOVE_NECKLACE, CMD_HIDE_NECKLACE, CMD_SHOW_NECKLACE = 11, 148, 62, 98, 99
GIFTS = {51: 0x2B, 52: 0x2C, 53: 0x2D, 54: 0x2E, 55: 0x2F, 56: 0x30, 57: 0x31, 77: 0x7C, 82: 0x7F, 79: 0x7B,
         76: 0x7D, 80: 0x7A, 78: 0x7E, 119: 0xB4, 120: 0xB5, 121: 0xB6, 122: 0xB7, 123: 0xB8}
PETS = {141: 0xAA, 142: 0xBC, 143: 0xB2, 144: 0xBD, 146: 0xB3, 147: 0xB0, 145: 0xB1}
NECKLACES = [i for i in GIFTS.values() if i not in (0x2B, 0x2C)]
INFO_SHOWING_NECKLACE, INFO_NECKLACE, INFO_DLC_PETS = 0x60, 0x8C, 0xA0
DM_ALL_NECKLACES = 0x50  # DataManager.AllNecklaces (static)


class GiftTests(unittest.TestCase):
    """Task 7, group "Gifts": refusals before any effect; show/hide refused."""

    def machine(self, items=None):
        m = Machine()
        f = m.followers[0]
        m.pos[f["obj"]] = (10.0, 5.0, 0.25)
        m.set_items([(ITEM_GOLD, 10)] + [(i, 1) for i in (items or [])])
        m.set_u64(m.kl["DM"][1] + DM_ALL_NECKLACES, m.make_list(NECKLACES, 4))
        m.set_u64(f["info"] + INFO_DLC_PETS, m.make_list([], 4))
        return m

    def refused(self, m, command, parent, reason):
        m.request(5, command=command, parent=parent)
        m.run()
        self.assertEqual(m.status(), (5, ST["REFUSED"], RS[reason]), (command, parent))
        self.assertEqual(m.effects(), [], (command, parent))

    def accepted(self, m, command, parent):
        m.request(5, command=command, parent=parent)
        m.run()
        self.assertEqual(m.status(), (5, ST["WALKING"], RS["NONE"]), (command, parent))

    def ran(self, m, command, parent):
        walk_up(m, command=command, parent=parent)
        m.end_walk()
        m.run()
        self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), (command, parent))

    def test_gift_runs_the_handler_with_its_parent_and_ends_with_the_game(self):
        for command, item in GIFTS.items():
            m = self.machine([item])
            self.ran(m, command, CMD_GIFT)
            self.assertEqual(m.handler_arrays, [[command, CMD_GIFT]], command)
            # The coroutine closes by itself later: the command runs until then
            m.run()
            self.assertEqual(m.status(), (5, ST["RUNNING"], RS["NONE"]), command)
            m.close_game(reopen=1)
            for _ in range(3):
                m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)

    def test_pet_runs_the_handler_with_its_parent(self):
        for command, item in PETS.items():
            m = self.machine([item])
            self.ran(m, command, CMD_GIVE_PET)
            self.assertEqual(m.handler_arrays, [[command, CMD_GIVE_PET]], command)
            m.close_game(reopen=0)
            m.run()
            self.assertEqual(m.status(), (5, ST["DONE"], RS["NONE"]), command)

    def test_winter_and_weird_necklaces_also_go_alone(self):
        # SnowmanCommands and MutatedCommands offer 120 and 122 at the top level: the array has one entry
        for command in (120, 122):
            m = self.machine([GIFTS[command]])
            self.ran(m, command, 0)
            self.assertEqual(m.handler_arrays, [[command]], command)

    def test_parents_alone_and_leader_items_are_refused(self):
        # [11] alone says "NoGifts", [148] falls through (Unhandled); 83 ends in MMConversation, 3 offers
        # no wheel
        for command in (CMD_GIFT, CMD_GIVE_PET, 83, 3):
            self.refused(self.machine([0x2B, 0xAA]), command, 0, "NOT_AVAILABLE")

    def test_sub_commands_need_their_own_parent(self):
        for command, parent in ((51, 0), (53, 0), (51, CMD_GIVE_PET), (141, CMD_GIFT), (141, 0), (53, CMD_EAT),
                                (120, CMD_GIVE_PET), (CMD_REMOVE_NECKLACE, CMD_GIFT), (CMD_SHOW_NECKLACE, CMD_GIFT),
                                (31, CMD_GIFT), (CMD_GIFT, CMD_GIFT)):
            m = self.machine([0x2B, 0x2D, 0xB5, 0xAA, 0x20])
            m.set_u32(m.followers[0]["info"] + INFO_NECKLACE, 0x2D)
            self.refused(m, command, parent, "NOT_AVAILABLE")

    def test_gift_needs_the_item_in_the_first_stack(self):
        self.refused(self.machine([]), 51, CMD_GIFT, "NOT_AVAILABLE")
        self.refused(self.machine([]), 141, CMD_GIVE_PET, "NOT_AVAILABLE")
        self.refused(self.machine([]), 120, 0, "NOT_AVAILABLE")
        # GetItemQuantity counts the first stack (0x1BBD440)
        m = self.machine()
        m.set_items([(0x2B, 0), (0x2B, 5)])
        self.refused(m, 51, CMD_GIFT, "NOT_AVAILABLE")
        m = self.machine()
        m = self.machine()
        m.set_items([None, (0x2B, 1)])
        self.refused(m, 51, CMD_GIFT, "NOT_AVAILABLE")
        m = self.machine([0x2B])
        m.set_u64(m.dm + DM_ITEMS, 0)
        self.refused(m, 51, CMD_GIFT, "NOT_AVAILABLE")

    def test_necklace_refused_while_one_is_worn(self):
        # GiveItemRoutine 0x2C7E640: with a necklace and a gift from AllNecklaces, "AlreadyHaveNecklace"
        m = self.machine([0x2D, 0x2B])
        m.set_u32(m.followers[0]["info"] + INFO_NECKLACE, 0x31)
        self.refused(m, 53, CMD_GIFT, "HAS_NECKLACE")
        m = self.machine([0x2D, 0x2B])
        m.set_u32(m.followers[0]["info"] + INFO_NECKLACE, 0x31)
        self.accepted(m, 51, CMD_GIFT)
        # AllNecklaces null: the coroutine would throw (for a necklace, otherwise it does not ask)
        m = self.machine([0x2B])
        m.set_u32(m.followers[0]["info"] + INFO_NECKLACE, 0x31)
        m.set_u64(m.kl["DM"][1] + DM_ALL_NECKLACES, 0)
        self.refused(m, 51, CMD_GIFT, "NOT_AVAILABLE")
        m = self.machine([0x2B])
        m.set_u64(m.kl["DM"][1] + DM_ALL_NECKLACES, 0)
        self.accepted(m, 51, CMD_GIFT)

    def test_pet_refused_with_a_pet_or_without_the_list(self):
        # GivePetRoutine 0x2C7FAA0: DLCPets null throws, an entry says "AlreadyHavePet"
        m = self.machine([0xAA])
        m.set_u64(m.followers[0]["info"] + INFO_DLC_PETS, m.make_list([3], 4))
        self.refused(m, 141, CMD_GIVE_PET, "HAS_PET")
        m = self.machine([0xAA])
        m.set_u64(m.followers[0]["info"] + INFO_DLC_PETS, 0)
        self.refused(m, 141, CMD_GIVE_PET, "NOT_AVAILABLE")

    def test_gifts_and_pets_need_the_audio_manager(self):
        for command, parent, item in ((51, CMD_GIFT, 0x2B), (141, CMD_GIVE_PET, 0xAA), (120, 0, 0xB5)):
            m = self.machine([item])
            m.set_u64(m.kl["AUDIO"][1] + AUDIO_INSTANCE, 0)
            self.refused(m, command, parent, "NOT_AVAILABLE")

    def test_gifts_skip_the_drowsy_guard(self):
        # Their branch does not go through the Drowsy block
        m = self.machine([0x2B])
        m.set_u64(m.followers[0]["info"] + 0x1E8, m.make_list([TRAIT_DROWSY], 4))
        self.accepted(m, 51, CMD_GIFT)

    def test_remove_necklace_needs_one(self):
        self.refused(self.machine(), CMD_REMOVE_NECKLACE, 0, "NOT_AVAILABLE")
        m = self.machine()
        m.set_u32(m.followers[0]["info"] + INFO_NECKLACE, 0x2D)
        self.ran(m, CMD_REMOVE_NECKLACE, 0)
        self.assertEqual(m.handler_arrays, [[CMD_REMOVE_NECKLACE]])

    def necklace(self, showing):
        m = self.machine()
        info = m.followers[0]["info"]
        m.set_u32(info + INFO_NECKLACE, 0x2D)
        m.set_u8(info + INFO_SHOWING_NECKLACE, 1 if showing else 0)
        return m

    def test_hide_and_show_are_refused(self):
        # Ruling on show/hide (fix round 1): SetFollowerCostume looks up to 18 skins and passes
        # the result unchecked to Skin.AddSkin, which throws on null. On our stack, without unwinding,
        # the game would crash: the helper refuses 98 and 99 in every state, without a game call
        for showing in (False, True):
            for command in (CMD_HIDE_NECKLACE, CMD_SHOW_NECKLACE):
                m = self.necklace(showing)
                self.refused(m, command, 0, "NOT_AVAILABLE")
                self.assertEqual(m.calls, [], (command, showing))
                self.assertEqual(m.uc.mem_read(m.followers[0]["info"] + INFO_SHOWING_NECKLACE, 1)[0],
                                 1 if showing else 0, (command, showing))

    def test_hide_and_show_never_reach_the_game_on_arrival(self):
        # Even the state the wheel offers does not start: no walk, no handler
        for showing in (False, True):
            m = self.necklace(showing)
            command = CMD_HIDE_NECKLACE if showing else CMD_SHOW_NECKLACE
            m.request(5, command=command)
            m.run()
            self.assertNotEqual(m.status()[1], ST["WALKING"], command)
            self.assertNotIn("command_finalized", m.names(), command)

if __name__ == "__main__":
    unittest.main()
