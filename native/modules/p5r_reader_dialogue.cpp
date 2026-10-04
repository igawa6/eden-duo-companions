// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// Dialogue and choices (p5r_dialogue_reader.h).
//   - Dialogue: picks the message context the game shows (a foreground choice, else the script
//     MSG index, else the single live normal message) and decodes it.
//   - PublishDialogue: dialogue.*, choice.N.*, controls.choice, controls.message.
//   - Trace: research builds only (P5R_DIALOGUE_TRACE).

#include "p5r_reader.h"

namespace p5r_module {

bool Reader::Dialogue(p5r_dialogue::Snapshot& talk) const {
    auto decode = [&](u64 context) {
        talk = p5r_dialogue::ReadContext(
            [&](u64 at, void* dst, size_t size) { return ReadBytes(at, dst, size); }, context,
            &text_globals);
#ifdef P5R_DIALOGUE_TRACE
        Trace(talk);
#endif
    };
    // Choices have their own lifecycle and may not populate the normal MSG index.
    // Prefer the foreground choice over a normal message behind it.
    std::array<u64, 505> slots{};
    u64 renderer{};
    if (ReadAt(message_table, 0x48, slots)) {
        for (size_t i = 0; i < 64; ++i) {
            std::int16_t choice_state{};
            if (slots[i * 8] && ReadAt(slots[i * 8], 0x70, renderer) && renderer &&
                ReadAt(slots[i * 8], 0x80, choice_state) && choice_state > 0) {
                decode(slots[i * 8]);
                return true;
            }
        }
    }
    s32 index{-1};
    u64 context{};
    std::int8_t lifecycle{};
    if (Read(message_index_slot, index) && index >= 0 && index < 64 &&
        ReadAt(message_table, 0x48 + u64{0x40} * index, context) && context &&
        ReadAt(context, 0x38, renderer) && renderer && ReadAt(context, 0x40, lifecycle) &&
        lifecycle > 0) {
        decode(context);
        return true;
    }
    // Messages started by native code (event scripts, EE2030/EDCEA0 callers) never set the
    // script MSG index. Accept a slot only when it is the single live normal message.
    u64 only{};
    for (size_t i = 0; i < 64; ++i) {
        const u64 slot = slots[i * 8];
        if (!slot || !ReadAt(slot, 0x38, renderer) || !renderer || !ReadAt(slot, 0x40, lifecycle) ||
            lifecycle <= 0)
            continue;
        if (only)
            return false;
        only = slot;
    }
    if (!only)
        return false;
    decode(only);
    return true;
}
#ifdef P5R_DIALOGUE_TRACE
// Research build only: one log line per distinct displayed text state.
void Reader::Trace(const p5r_dialogue::Snapshot& t) const {
    if (!host.log || !t.active || t.torn)
        return;
    static constexpr std::array<const char*, 5> Reasons{"none", "control", "glyph", "byte",
                                                        "substitution"};
    char line[512];
    std::string bytes;
    for (size_t i = 0; i < t.fallback.size; ++i) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", t.fallback.bytes[i]);
        bytes += hex;
    }
    std::snprintf(line, sizeof(line),
                  "P5R dialogue trace: ctx=%llx msg=%u page=%u choice=%d ready=%d "
                  "reason=%s code=%04x bytes=%s speaker=[%s] text=[%.200s] choices=%zu",
                  static_cast<unsigned long long>(t.context), t.message, t.page,
                  t.choice_active ? 1 : 0, t.ready ? 1 : 0,
                  Reasons[static_cast<size_t>(t.fallback.reason)], t.fallback.code, bytes.c_str(),
                  t.speaker.c_str(), t.text.c_str(), t.choices.size());
    std::string key{line};
    if (key == last_trace)
        return;
    last_trace = key;
    for (char& c : key)
        if (c == '\n')
            c = '|';
    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, key.c_str());
    std::string raw = "P5R dialogue raw: ";
    for (u8 b : t.raw) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        raw += hex;
    }
    host.log(host.userdata, EDEN_DSMOD_LOG_INFO, raw.c_str());
}
#endif
// dialogue.*, choice.N.*, controls.choice / controls.message.
void Reader::PublishDialogue(const Snapshot& out) const {
    const bool talking = out.ready && !out.menu && out.dialogue;
    const bool text_ready = talking && out.talk.ready && !out.talk.torn;
    const bool choice_ready = text_ready && out.talk.choice_active;
    I64("dialogue.ready", talking);
    I64("dialogue.choice_count", choice_ready ? out.talk.choices.size() : 0);
    I64("dialogue.selected", choice_ready ? out.talk.selected : -1);
    I64("controls.choice", choice_ready);
    I64("controls.message", text_ready && out.talk.active && !out.talk.choice_active);
    Text("dialogue.speaker", text_ready ? out.talk.speaker.c_str() : "");
    // Hold-up/negotiation lines carry no message speaker (the battle UI draws that plate),
    // so the page hides its nameplate instead of showing an empty one.
    I64("dialogue.has_speaker", text_ready && !out.talk.speaker.empty());
    Text("dialogue.text", text_ready ? out.talk.text.c_str()
                          : talking  ? "Read the dialogue on the main screen."
                                     : "");
    for (size_t i = 0; i < 8; ++i) {
        const auto prefix = "choice." + std::to_string(i) + ".";
        const bool present = choice_ready && i < out.talk.choices.size();
        I64(prefix + "present", present);
        I64(prefix + "selected", present && out.talk.selected == static_cast<int>(i));
        Text(prefix + "text", present ? out.talk.choices[i].text.c_str() : "");
    }
}

} // namespace p5r_module
