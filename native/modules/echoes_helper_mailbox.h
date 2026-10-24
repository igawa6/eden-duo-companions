// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Echoes of Wisdom (01008CF01BAAC000) module-side guest mailbox.
//
// Five request kinds now live here. Kinds 1-2 are served by BOTH real hooks:
// "ping" (kind 1, the original toolchain-proof request -- still exercised by
// echoes_dsmod_ping_hook/--hook-kind bl) and "equip_y" (kind 2, the first real
// feature route: "set the Y-button Echo/Automaton directly" -- served by
// echoes_helper/--hook-kind tail-b and echoes_helper_entry/--hook-kind entry).
// Kinds 3-6 are ENTRY HOOK ONLY (echoes_helper/tail-b never gained them):
// "accessory_set" (kind 3, equip/unequip one of up to 5 accessory slots),
// "outfit_set" (kind 4, switch the equipped outfit), "consume" (kind 5, eat a
// consumable: decrement its count, heal HP), and "warp" (kind 6, start the
// game's MapWarp event flow directly, exactly as the Map menu's confirm code
// does). Kinds 3-5 share equip_y's exact P/Q acquire-guard-chain and
// lock-release, byte-for-byte, and were ground-truthed live on the desktop by
// diffing memory around native pause-menu actions (not derived from a
// pre-made disassembly dump the way equip_y's own guard chain and the
// HUD-refresh block were). Kind 6 uses NO save-data P lock at all (this
// mechanism does not use it) and was ground-truthed byte-exact against a
// disassembly trace of the Map menu's own confirm code, not live-diffed.
// Modeled on the BOTW module's SelectionBridge
// (packaging/duo-110-docs-r18/native/modules/botw_bridge.h); see
// HELPER-TOOLCHAIN.md section 5 for the full field-by-field mapping back to
// BOTW's mailbox, and section 5a for the equip_y guard-chain pseudocode this
// class's SubmitEquipY()/Poll() pairing is built against.
//
// This header only touches the module side of the ABI (dsmod_module_extensions.h's
// EdenDsmodHostExtensions -- load_u32/u64, store_u32/u64 -- and dsmod_module_abi.h's
// EdenDsmodHostApi::get_i64 for "__mailbox_epoch"). It has no dependency on the
// Eden Duo build tree beyond those two headers and compiles standalone.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "core/mods/dsmod_module_abi.h"
#include "core/mods/dsmod_module_extensions.h"

namespace echoes::helper {

// ---- Mailbox layout (host virtual memory, allocated by the format-2 load plan) ---
//
// Total size 4096 bytes (one guest page -- the runtime requires mailbox_size to be
// page-aligned and non-zero; 4096 is the smallest legal value and is all a single-
// in-flight-request-at-a-time route needs, same as BOTW). The LAST 4 bytes of the
// mailbox are reserved for the host's own session epoch (set via
// "mailbox_epoch_offset" in the load plan JSON, here mailbox_size - 4 = 4092,
// exactly BOTW's convention) and must never be written by guest code or by this
// module -- only core.cpp's InvalidateDualScreenGuestMailbox touches it, on every
// emulation pause/resume and every debugger pause/resume.
//
//   0x000  u32  magic           guest-written every tick ("EOW1" = 0x31574F45)
//   0x004  u32  heartbeat       guest-written every tick (free-running tick counter)
//   0x008  u32  request_epoch   module->guest "go" signal (atomic store/ldar)
//   0x00C  u32  ack_epoch       guest->module "done" signal (atomic stlr/load);
//                               this IS the "echo the request epoch" the module
//                               matches a completed request against -- there is no
//                               separate echo field, the ack epoch literally equals
//                               the request epoch once handled
//   0x010  u32  status          guest-written: 0 idle, 1 ok, 2 refused, 3 already
//                                (kinds 2-4 only; ping and consume never produce 3;
//                                ping only ever produces 0/1/2)
//   0x014  u32  ping_counter    guest-written on a *successful ping* only (kind 1)
//   0x018  u32  session_epoch   module-written: the host session epoch this request
//                               was built against (checked against offset 4092)
//   0x01C  u32  scope_permit    module-written LAST (after every other field, before
//                               request_epoch): the "cancelable scope permit"
//   0x020  u32  request_kind    module-written (request-specific arg, written
//                                BEFORE the permit/epoch pair above): 1 = ping,
//                                2 = equip_y, 3 = accessory_set, 4 = outfit_set,
//                                5 = consume. Dispatches which guarded checks the
//                                guest hook runs before acknowledging.
//   0x024  u32  target          module-written (kinds 2-5): generic "target" field
//                                -- equip_y's native id (zero-extended u16; 0xFFFF
//                                is reserved, "never clear Y", always refused code
//                                14), accessory_set's single accessory bit mask,
//                                outfit_set's single outfit bit (zero-extended
//                                u16), or consume's quantity index (0..15).
//   0x028  u32  result_code     guest-written (kinds 2-5): the specific refuse
//                                reason (10..22, see RefuseCode below) when
//                                status == refused; 0 otherwise. Unused (always 0)
//                                for ping.
//   0x02C  u32  arg             module-written (kinds 3 and 5 only): accessory_set's
//                                equip(1)/unequip(0) flag, or consume's heal amount
//                                in quarter-hearts (read by the guest as SIGNED
//                                int32). Unused by every other kind.
//   0x030  u32  entry_len       module-written (kind 6/warp only): length of the
//                                entry-name string at 0x100 (1..32)
//   0x034  u32  level_len       module-written (kind 6/warp only): length of the
//                                level string at 0x120 (0..64; 0 means "no level",
//                                skipped entirely -- NOT a refusal)
//   0x038  u32  locator_len     module-written (kind 6/warp only): length of the
//                                locator string at 0x160 (1..64)
//   0x100  u8[32]  entry name bytes (kind 6/warp only; NOT nul-terminated --
//                                exactly entry_len bytes are meaningful)
//   0x120  u8[64]  level bytes (kind 6/warp only; exactly level_len bytes
//                                meaningful; ignored if level_len == 0)
//   0x160  u8[64]  locator bytes (kind 6/warp only; exactly locator_len bytes
//                                meaningful)
//   mailbox_size - 4            u32  host-owned session epoch (never write this)
//
// The guest takes POINTERS directly into the mailbox's 0x100/0x120/0x160
// string areas (not copies) when staging kind 6's native calls -- the
// strings must stay valid (the module must not overwrite them) until the
// request completes, i.e. until Poll() observes the ack epoch.
//
// A future request kind adds its own argument/result fields after 0x1A0 (the
// end of the locator string area) and its own scope/structure guards in the
// guest hook; the fields above and their write order do not change.
namespace offset {
constexpr std::uint32_t Magic = 0x000;
constexpr std::uint32_t Heartbeat = 0x004;
constexpr std::uint32_t RequestEpoch = 0x008;
constexpr std::uint32_t AckEpoch = 0x00C;
constexpr std::uint32_t Status = 0x010;
constexpr std::uint32_t PingCounter = 0x014;
constexpr std::uint32_t SessionEpochCopy = 0x018;
constexpr std::uint32_t ScopePermit = 0x01C;
constexpr std::uint32_t RequestKind = 0x020;
constexpr std::uint32_t Target = 0x024; // generic; EquipYTargetId is a same-value alias below
constexpr std::uint32_t EquipYTargetId = Target; // historical name, kept for compat
constexpr std::uint32_t ResultCode = 0x028;
constexpr std::uint32_t ArgWord = 0x02C; // kind 3 (equip/unequip flag), kind 5 (heal amount)
constexpr std::uint32_t WarpEntryLen = 0x030;
constexpr std::uint32_t WarpLevelLen = 0x034;
constexpr std::uint32_t WarpLocatorLen = 0x038;
constexpr std::uint32_t WarpEntryBytes = 0x100; // u8[32]
constexpr std::uint32_t WarpLevelBytes = 0x120; // u8[64]
constexpr std::uint32_t WarpLocatorBytes = 0x160; // u8[64]
constexpr std::uint32_t WarpMaxEntryLen = 32;
constexpr std::uint32_t WarpMaxLevelLen = 64;
constexpr std::uint32_t WarpMaxLocatorLen = 64;
} // namespace offset

constexpr std::uint64_t MailboxSize = 4096;
constexpr std::uint32_t MailboxEpochOffset = static_cast<std::uint32_t>(MailboxSize) - 4;
constexpr std::uint32_t HelperMagic = 0x31574F45; // "EOW1", little-endian u32

enum class Status : std::uint32_t { Idle = 0, Ok = 1, Refused = 2, Already = 3 };

enum class Kind : std::uint32_t {
    None = 0,
    Ping = 1,
    EquipY = 2,
    Accessory = 3, // ENTRY HOOK ONLY
    Outfit = 4, // ENTRY HOOK ONLY
    Consume = 5, // ENTRY HOOK ONLY
    Warp = 6, // ENTRY HOOK ONLY; no save-data P lock involved at all
};

// result_code values written by the guest on a Refused status. 10-13 are the
// COMMON P/Q acquire-guard-chain codes, shared by every kind that acquires the
// lock at all (everything except Ping): P == 0, the acquire call's bit0 clear,
// Q == 0, and Q's locked flag (byte +0x17 bit 6) -- byte-for-byte from the
// native ActorSelect release guard chain (main+0x24d9150 -> the bl at
// main+0x24d924c). 14-15 are equip_y-specific; 16-18 are accessory_set-specific
// (ENTRY HOOK ONLY); 19 is outfit_set-specific (ENTRY HOOK ONLY); 20-22 are
// consume-specific (ENTRY HOOK ONLY). Always 0 for Kind::Ping (which never
// acquires anything) and never meaningful unless Status::Refused.
enum class RefuseCode : std::uint32_t {
    NoActorSelectRoot = 10, // P == 0 (common)
    ReleaseGuardFailed = 11, // (call vtable[0x40](P) & 1) == 0 (common)
    NoSecondaryPointer = 12, // Q == *(P + 8) == 0 (common)
    LockedFlagSet = 13, // *(u8*)(Q + 0x17) & 0x40 (common)
    NeverClearY = 14, // equip_y: id == 0xFFFF
    NativeAssignFailed = 15, // equip_y: post-call verification mismatch
    AccessoryInvalidTarget = 16, // accessory_set: target == 0 or more than one bit set
    AccessoryNotAcquired = 17, // accessory_set: !(Q+0x187c & target)
    AccessorySlotsFull = 18, // accessory_set (equip): no empty slot before capacity
    OutfitNotOwned = 19, // outfit_set: !(Q+0x1878 & target)
    ConsumeInvalidIndex = 20, // consume: quantity index > 15
    ConsumeInvalidHealAmount = 21, // consume: heal arg < 0 or > 0x1000 (sanity)
    ConsumeNoneLeft = 22, // consume: count byte at Q+0x185e+index == 0
    WarpNoManager = 23, // warp: mgr == *(main+0x34bb8d8) == 0
    WarpInvalidEntryLen = 24, // warp: entry_len == 0 or > 32
    WarpInvalidLevelLen = 25, // warp: level_len > 64
    WarpInvalidLocatorLen = 26, // warp: locator_len == 0 or > 64
    WarpNoResourceManager = 27, // warp: *(main+0x34d23b0) == 0 (resource mgr ptr)
    WarpEmptyName = 28, // warp: name.obj == 0 after warp_name_ctor (empty flow name)
};
// Historical name from the equip_y-only era, kept as an alias.
using EquipYRefuseCode = RefuseCode;

// One in-flight request at a time, shared across every kind -- exactly like
// BOTW's SelectionBridge: "BOTW keeps one shared epoch/permit pair and several
// pending_<kind> booleans, since only one guest hook invocation can run at a
// time anyway." This class keeps that same one shared epoch/permit pair and a
// single `pending_kind_` discriminant instead of per-kind booleans, now that a
// second kind (equip_y) actually needs sharing it with ping.
class HelperMailbox {
public:
    using Clock = std::chrono::steady_clock;

    // Called once from the module's `configure()` extension callback, right after
    // the host hands over EdenDsmodHostExtensions. Mirrors BOTW's
    // SelectionBridge::configure: validate the struct before trusting any of its
    // function pointers, and cancel any in-flight request (there cannot be a live
    // one yet, but this also resets state on a manifest hot-reload, which calls
    // configure() again with a fresh struct).
    void Configure(const EdenDsmodHostExtensions* extensions) {
        Drop();
        ext_ = {};
        healthy_ = false;
        if (extensions != nullptr && extensions->version == EDEN_DSMOD_EXT_VERSION &&
            extensions->struct_size == sizeof(*extensions) &&
            extensions->abi_hash == EDEN_DSMOD_EXT_HASH && extensions->mailbox_size >= MailboxSize &&
            extensions->load_u32 != nullptr && extensions->store_u32 != nullptr) {
            ext_ = *extensions;
            Drop();
        }
    }

    // Called once per sample()/tick() with the host API the module was handed for
    // that call. Refreshes `healthy_` (helper present and beating) and, if a
    // request is in flight, checks for its acknowledgement. Mirrors BOTW's
    // SelectionBridge::poll.
    void Poll(const EdenDsmodHostApi& host) {
        host_ = host;
        const auto now = Clock::now();
        const std::uint32_t current_session = SessionEpoch();
        if (current_session != session_epoch_) {
            // The host paused/resumed, a save/state loaded, or a debugger stepped
            // through this window: any request built against the old session is
            // dead. Cancel rather than let a late ack land against a different
            // session's state.
            Drop();
            session_epoch_ = current_session;
        }
        std::uint32_t magic = 0, beat = 0, guest_epoch = 0;
        healthy_ = session_epoch_ != 0 && Load(offset::Magic, magic) && magic == HelperMagic &&
                   Load(offset::Heartbeat, beat) && Load(MailboxEpochOffset, guest_epoch) &&
                   guest_epoch == session_epoch_;
        if (!healthy_) {
            return;
        }
        if (beat != last_beat_) {
            last_beat_ = beat;
            last_beat_at_ = now;
        }
        // The helper is "alive" while it beat within the last ~3 ticks (50 ms at 60
        // Hz); this is a fixed window, unlike BOTW's frame-rate-adaptive
        // beat_window(), because neither route here has a feature-specific latency
        // budget to protect -- widen this once a real route needs to.
        healthy_ = last_beat_at_.time_since_epoch().count() != 0 &&
                   now - last_beat_at_ <= std::chrono::milliseconds(50);
        if (pending_epoch_ == 0) {
            return;
        }
        std::uint32_t ack = 0;
        if (!Load(offset::AckEpoch, ack) || ack != pending_epoch_) {
            if (now - requested_at_ > std::chrono::milliseconds(200)) {
                // The helper did not beat our request within the ack window (it may
                // not be installed at all, or the request arrived between two
                // invocations and the guest never ran again in time): give up rather
                // than hold `pending_epoch_` forever.
                Drop();
            }
            return;
        }
        std::uint32_t status = 0, result_code = 0, counter = 0;
        Load(offset::Status, status);
        Load(offset::ResultCode, result_code);
        last_status_ = static_cast<Status>(status);
        last_result_code_ = result_code;
        last_completed_kind_ = pending_kind_;
        if (pending_kind_ == Kind::Ping) {
            Load(offset::PingCounter, counter);
            last_ping_counter_ = counter;
        }
        pending_epoch_ = 0;
        pending_kind_ = Kind::None;
        Cancel();
    }

    // Arms exactly one ping. Returns false (and arms nothing) if a request of
    // EITHER kind is already in flight, the helper is not healthy, or any store
    // fails -- the same "validate first, write second, re-validate right before
    // the irreversible write" discipline every BOTW route uses (see
    // botw_bridge.h request_warp for the fullest worked example).
    bool SubmitPing() {
        return SubmitRequest(Kind::Ping, [this](std::uint32_t session_now) {
            (void)session_now;
            return true; // ping has no argument fields of its own to write
        });
    }

    // Arms exactly one equip_y request: "set the Y-button Echo/Automaton
    // directly" to `native_id`. Same in-flight/healthy guard as SubmitPing(); the
    // guest (eh_handle_equip_y) is the authoritative check for every guard in the
    // native ActorSelect release chain, including id == 0xFFFF ("never clear Y",
    // EquipYRefuseCode::NeverClearY) -- the early reject below is only a
    // convenience to avoid wasting a round trip on a value the guest can never
    // honour, not a substitute for the guest's own check.
    bool SubmitEquipY(std::uint16_t native_id) {
        if (native_id == 0xFFFF) {
            return false;
        }
        return SubmitRequest(Kind::EquipY, [this, native_id](std::uint32_t session_now) {
            (void)session_now;
            return Store(offset::EquipYTargetId, static_cast<std::uint32_t>(native_id));
        });
    }

    // Arms exactly one accessory_set request (ENTRY HOOK ONLY -- see
    // en_handle_accessory in hook.S; echoes_helper/tail-b never gained kinds
    // 3-5). `mask` must be exactly one accessory bit; `equip` selects
    // equip(true)/unequip(false). Same early-reject convention as
    // SubmitEquipY(): the guest is the authoritative check
    // (RefuseCode::AccessoryInvalidTarget), this just avoids a round trip on a
    // value it can never honour.
    bool SubmitAccessory(std::uint32_t mask, bool equip) {
        if (mask == 0 || (mask & (mask - 1)) != 0) {
            return false;
        }
        return SubmitRequest(Kind::Accessory, [this, mask, equip](std::uint32_t session_now) {
            (void)session_now;
            return Store(offset::Target, mask) && Store(offset::ArgWord, equip ? 1u : 0u);
        });
    }

    // Arms exactly one outfit_set request (ENTRY HOOK ONLY). `mask` must be
    // exactly one outfit bit; no early-reject here since (unlike equip_y's
    // 0xFFFF and accessory_set's zero/multi-bit cases) outfit_set's own guard
    // chain already refuses a mask that is not owned (RefuseCode::OutfitNotOwned)
    // for every value this method would otherwise want to pre-filter, including
    // 0 (never owned) -- see HELPER-TOOLCHAIN.md section 5c.
    bool SubmitOutfit(std::uint16_t mask) {
        return SubmitRequest(Kind::Outfit, [this, mask](std::uint32_t session_now) {
            (void)session_now;
            return Store(offset::Target, static_cast<std::uint32_t>(mask));
        });
    }

    // Arms exactly one consume request (ENTRY HOOK ONLY). `quantity_index`
    // selects which of the 16 consumable slots at Q+0x185e; `heal` is the
    // amount to add to current HP, in quarter-hearts, as a SIGNED int32 the
    // module has already computed (the same truncation as native's own
    // fcvtzs) -- the guest takes it as given, floors the result at 0, and
    // clamps it to max HP. Early-rejects the same two sanity checks the guest
    // itself refuses with (RefuseCode::ConsumeInvalidIndex/
    // ConsumeInvalidHealAmount), again only to avoid a wasted round trip.
    bool SubmitConsume(std::uint8_t quantity_index, std::int32_t heal) {
        if (quantity_index > 15 || heal < 0 || heal > 0x1000) {
            return false;
        }
        return SubmitRequest(Kind::Consume, [this, quantity_index, heal](std::uint32_t session_now) {
            (void)session_now;
            return Store(offset::Target, static_cast<std::uint32_t>(quantity_index)) &&
                   Store(offset::ArgWord, static_cast<std::uint32_t>(heal));
        });
    }

    // Arms exactly one warp request (ENTRY HOOK ONLY -- see en_handle_warp in
    // hook.S; echoes_helper/tail-b never gained kind 6 either, and unlike
    // kinds 2-5 it uses NO save-data P lock at all). `entry` is the MapWarp
    // flowchart entry-point name (1-32 bytes); `level` is optional (empty
    // means "no level", NOT a refusal -- the guest skips that Set() call
    // entirely); `locator` is required (1-64 bytes). The guest takes
    // POINTERS directly into the mailbox's own string bytes -- do not
    // overwrite/reuse these buffers (by calling Submit* again) until this
    // request completes (Poll() observes the ack epoch).
    bool SubmitWarp(std::string_view entry, std::string_view level, std::string_view locator) {
        if (entry.empty() || entry.size() > offset::WarpMaxEntryLen) {
            return false;
        }
        if (level.size() > offset::WarpMaxLevelLen) {
            return false;
        }
        if (locator.empty() || locator.size() > offset::WarpMaxLocatorLen) {
            return false;
        }
        return SubmitRequest(Kind::Warp, [this, entry, level, locator](std::uint32_t session_now) {
            (void)session_now;
            // Store order matters (the task's own instruction): the strings
            // and their lengths are written here, in SubmitRequest's
            // write_args callback, which always runs strictly BEFORE its
            // permit/epoch writes -- the guest must never be able to observe
            // a new request epoch before the strings it will read are fully
            // written. Write the bytes before the lengths for the same
            // reason, one level down (not load-bearing today, since nothing
            // reads these mid-write, but it costs nothing to order
            // defensively the same way every other field in this class is).
            return StoreBytes(offset::WarpEntryBytes, entry, offset::WarpMaxEntryLen) &&
                   StoreBytes(offset::WarpLevelBytes, level, offset::WarpMaxLevelLen) &&
                   StoreBytes(offset::WarpLocatorBytes, locator, offset::WarpMaxLocatorLen) &&
                   Store(offset::WarpEntryLen, static_cast<std::uint32_t>(entry.size())) &&
                   Store(offset::WarpLevelLen, static_cast<std::uint32_t>(level.size())) &&
                   Store(offset::WarpLocatorLen, static_cast<std::uint32_t>(locator.size()));
        });
    }

    // Zeroes the scope permit only. This is deliberately NOT a full reset of
    // request_epoch/ack_epoch: the guest hook keys "is there a new request" purely
    // off request_epoch changing, so cancelling must work by making an in-flight
    // epoch's permit fail to match (mailbox+0x1C != mailbox+0x08), never by trying to
    // roll request_epoch itself backward (that could resurrect an epoch value the
    // guest already acked, which would look like a new request that never arrives).
    void Cancel() {
        if (ext_.store_u32 != nullptr) {
            Store(offset::ScopePermit, 0);
        }
    }
    // Abandon the in-flight request as well: without clearing it every later
    // Submit*() would be refused until an ack that may never come.
    void Drop() {
        Cancel();
        pending_epoch_ = 0;
        pending_kind_ = Kind::None;
    }

    bool Healthy() const {
        return healthy_;
    }
    bool Configured() const { return ext_.load_u32 != nullptr; }
    std::uint32_t RawWord(std::uint32_t at) const { std::uint32_t v = 0; Load(at, v); return v; }
    std::uint32_t SessionEpochValue() const { return session_epoch_; }
    bool RequestInFlight() const {
        return pending_epoch_ != 0;
    }
    bool PingInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::Ping;
    }
    bool EquipYInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::EquipY;
    }
    bool AccessoryInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::Accessory;
    }
    bool OutfitInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::Outfit;
    }
    bool ConsumeInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::Consume;
    }
    bool WarpInFlight() const {
        return pending_epoch_ != 0 && pending_kind_ == Kind::Warp;
    }
    // Valid only right after Poll() observes a completed ping (check
    // last_completed_kind_ == Kind::Ping if several kinds may complete between
    // reads).
    bool LastPingOk() const {
        return last_completed_kind_ == Kind::Ping && last_status_ == Status::Ok;
    }
    std::uint32_t LastPingCounter() const {
        return last_ping_counter_;
    }
    Kind LastCompletedKind() const {
        return last_completed_kind_;
    }
    // Generic accessors for the last completed request of ANY kind (2-5; kind
    // 1/ping has no "refused"/"already" shape of its own, see LastPingOk()
    // above instead). Added alongside the equip_y-era, kind-specific names
    // below, which now just delegate to these -- the underlying state was
    // already generic (last_status_/last_result_code_ never were
    // equip_y-only fields), only the accessor names were.
    Status LastStatus() const {
        return last_status_;
    }
    // Only meaningful when LastStatus() == Status::Refused.
    std::uint32_t LastRefuseCode() const {
        return last_result_code_;
    }
    Status LastEquipYStatus() const {
        return LastStatus();
    }
    // Only meaningful when LastEquipYStatus() == Status::Refused.
    EquipYRefuseCode LastEquipYRefuseCode() const {
        return static_cast<EquipYRefuseCode>(LastRefuseCode());
    }

private:
    // Shared submit path for every request kind: cancel defensively, zero the
    // result fields, write the kind + any kind-specific argument fields (via
    // `write_args`), then the permit/epoch pair, each re-validated against the
    // session epoch right before its own write -- BOTW's universal write-order
    // rule (HELPER-TOOLCHAIN.md section 5's "Write order on submit"), now shared
    // by every kind instead of duplicated per SubmitXxx().
    template <typename WriteArgs>
    bool SubmitRequest(Kind kind, WriteArgs&& write_args) {
        if (!healthy_ || pending_epoch_ != 0) {
            return false;
        }
        Cancel();
        if (!Store(offset::Status, static_cast<std::uint32_t>(Status::Idle)) ||
            !Store(offset::ResultCode, 0)) {
            return false;
        }
        const std::uint32_t session_now = SessionEpoch();
        if (session_now == 0 || session_now != session_epoch_) {
            return false; // the session moved on while we were about to submit
        }
        if (!Store(offset::SessionEpochCopy, session_now)) {
            return false;
        }
        if (!Store(offset::RequestKind, static_cast<std::uint32_t>(kind))) {
            return false;
        }
        if (!write_args(session_now)) {
            Cancel();
            return false;
        }
        std::uint32_t next_epoch = sequence_ + 1;
        if (next_epoch == 0) {
            next_epoch = 1; // never hand out epoch 0; the guest treats 0 as "none"
        }
        // Re-validate immediately before each of the two commit writes. The
        // permit is written BEFORE the epoch (the guest loads epoch first, then
        // checks the permit matches), so a torn read of the mailbox never shows a
        // live epoch with a permit that has not caught up yet.
        if (SessionEpoch() != session_now || !Store(offset::ScopePermit, next_epoch)) {
            Cancel();
            return false;
        }
        if (SessionEpoch() != session_now || !Store(offset::RequestEpoch, next_epoch)) {
            Cancel();
            return false;
        }
        sequence_ = next_epoch;
        pending_epoch_ = next_epoch;
        pending_kind_ = kind;
        requested_at_ = Clock::now();
        return true;
    }

    bool Load(std::uint32_t at, std::uint32_t& value) const {
        return ext_.load_u32 != nullptr && ext_.load_u32(ext_.userdata, at, &value) != 0;
    }
    bool Store(std::uint32_t at, std::uint32_t value) {
        return ext_.store_u32 != nullptr && ext_.store_u32(ext_.userdata, at, value) != 0;
    }
    // Packs `data` (up to `max_len` bytes, zero-padded beyond data.size())
    // into little-endian u32 chunks and Store()s each one -- the base host
    // extensions this class uses (dsmod_module_extensions.h) only offer
    // u32/u64-granularity stores, no generic byte-array write.
    bool StoreBytes(std::uint32_t base_offset, std::string_view data, std::uint32_t max_len) {
        for (std::uint32_t i = 0; i < max_len; i += 4) {
            std::uint32_t word = 0;
            for (std::uint32_t b = 0; b < 4 && i + b < max_len; ++b) {
                const std::uint8_t byte = (i + b < data.size()) ? static_cast<std::uint8_t>(data[i + b]) : 0;
                word |= static_cast<std::uint32_t>(byte) << (8 * b);
            }
            if (!Store(base_offset + i, word)) {
                return false;
            }
        }
        return true;
    }
    // "__mailbox_epoch": published by the host (mod_module_host.cpp
    // ModRuntime::SourceQuery) from System::GetDualScreenGuestMailboxEpoch(), the
    // SAME counter core.cpp writes into the mailbox's last 4 bytes. Reading it
    // through get_i64 rather than through the mailbox extension is deliberate: it is
    // available even before the helper has ever beaten once, which is what lets
    // Poll() tell "no helper installed" (session_epoch_ stays 0) apart from "helper
    // installed but has not reached the guest hook's atomic store yet".
    std::uint32_t SessionEpoch() const {
        if (host_.get_i64 == nullptr) {
            return 0;
        }
        const std::int64_t value = host_.get_i64(host_.userdata, "__mailbox_epoch", 0);
        return value > 0 && static_cast<std::uint64_t>(value) <= 0xFFFFFFFFull
                   ? static_cast<std::uint32_t>(value)
                   : 0;
    }

    EdenDsmodHostExtensions ext_{};
    EdenDsmodHostApi host_{};
    bool healthy_{};
    std::uint32_t session_epoch_{};
    std::uint32_t sequence_{};
    std::uint32_t pending_epoch_{};
    Kind pending_kind_{Kind::None};
    Kind last_completed_kind_{Kind::None};
    std::uint32_t last_beat_{};
    Clock::time_point last_beat_at_{};
    Clock::time_point requested_at_{};
    Status last_status_{Status::Idle};
    std::uint32_t last_result_code_{};
    std::uint32_t last_ping_counter_{};
};

// Historical name from the ping-only skeleton, kept as an alias so any earlier
// note/snippet referencing PingMailbox still compiles unchanged.
using PingMailbox = HelperMailbox;

} // namespace echoes::helper
