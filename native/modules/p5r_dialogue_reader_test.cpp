// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// p5r_dialogue_reader.h tests: control-code classification, EN glyph map, native substitutions
// and fail-closed paths. The kPage_* messages are synthetic: each keeps the control-code layout of
// a real EN page type (the codes and their arguments are format facts the decoder handles), with
// made-up text. Every substituted value (names, variables, tables) is synthetic as well.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "p5r_dialogue_reader.h"

using U = uint64_t;
using Bytes = std::vector<uint8_t>;
namespace {
const std::vector<uint8_t> kHead{0xf2, 0x05, 0xff, 0xff, 0xf1, 0x41};
std::vector<uint8_t> MakePage(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> b = kHead;
    for (const auto& p : parts)
        b.insert(b.end(), p.begin(), p.end());
    return b;
}
std::vector<uint8_t> T(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}
// Page layouts (synthetic text). F2 44 = context variable, F28B/F28C = stat icon / name, F38D =
// stat level label, F181/F182 = name buffers, F19C = thieves name, F445 = item from a variable,
// F39F = unit name, F22A = button icon, F201 = text style, F7 xx / F6 86 / F4 8A = no-text
// controls.
const std::vector<uint8_t> kPage_var_date = MakePage({T("The draw is held in a few days,\non "),
                                                      {0xf2, 0x44, 0x01, 0x01},
                                                      T("/"),
                                                      {0xf2, 0x44, 0x02, 0x01},
                                                      T(".\nDo not miss it.\n"),
                                                      {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_stat_icon_name =
    MakePage({T("I think my "),
              {0xf2, 0x8b, 0x02, 0x01, 0xf2, 0x01, 0x1b, 0x01, 0xf2, 0x8c, 0x03, 0x01, 0xf2, 0x01,
               0x1c, 0x01},
              T("\nwill grow soon...\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_name_a =
    MakePage({{0xf7, 0x97, 0x02, 0x01, 0x13, 0x01, 0x04, 0x01, 0x0b, 0x01, 0x01, 0x01, 0x0b, 0x01,
               0xf7, 0x61, 0x09, 0x01, 0x01, 0x01, 0x02, 0x01, 0x02, 0x01, 0x13, 0x01, 0x01, 0x01},
              T("Hey, "),
              {0xf1, 0x81},
              T("-san? Need\nsomething?\n"),
              {0xf2, 0x23, 0x00, 0x00, 0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_item_var =
    MakePage({{0xf7, 0x97, 0x02, 0x01, 0x15, 0x01, 0x16, 0x01, 0x06, 0x01, 0x15, 0x01, 0x01, 0x01,
               0xf7, 0x61, 0x09, 0x01, 0x03, 0x01, 0x02, 0x01, 0x01, 0x01, 0x15, 0x01, 0x01, 0x01},
              T("Our "),
              {0xf4, 0x45, 0x04, 0x01, 0x10, 0x28, 0x01, 0x01},
              T(" sells well...\nWe get more on "),
              {0xf2, 0x01, 0x03, 0x01},
              T("Mondays"),
              {0xf2, 0x01, 0x1c, 0x01},
              T(",\nmost weeks.\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_glyph_cafe = MakePage(
    {T("The sign reads\n\"Tea and Caf"), {0x84, 0x9e}, T("\" corner...\n"), {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_thieves =
    MakePage({{0xf7, 0x61, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
              T("Get some rest. From\nnext week on we work as\n"),
              {0xf1, 0x9c},
              T(".\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_stat_level =
    MakePage({T("I cannot leave the\nshop until I have\nenough "),
              {0xf2, 0x8b, 0x04, 0x01, 0xf2, 0x01, 0x1b, 0x01, 0xf2, 0x8c, 0x04, 0x01, 0xf2, 0x01,
               0x1c, 0x01},
              T(" to be "),
              {0xf2, 0x01, 0x1b, 0x01, 0xf3, 0x8d, 0x04, 0x01, 0x04, 0x01, 0xf2, 0x01, 0x1c, 0x01},
              T(".\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_unit =
    MakePage({T("Pass it to "), {0xf3, 0x9f, 0x0b, 0x01, 0x02, 0x01, 0}});
const std::vector<uint8_t> kPage_button_icon = MakePage({T("Walk up to a foe and press\n"),
                                                         {0xf2, 0x2a, 0x17, 0x01},
                                                         T(" to begin a fight.\n"),
                                                         {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_name_b =
    MakePage({{0xf1, 0x82},
              T("'s gift lets him keep \nmany masks. So you may "),
              {0xf2, 0x01, 0x03, 0x01},
              T("freely"),
              {0xf2, 0x01, 0x1c, 0x01},
              T(" \n"),
              {0xf2, 0x01, 0x03, 0x01},
              T("swap which mask"),
              {0xf2, 0x01, 0x1c, 0x01},
              T(" you wear.\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_end124 =
    MakePage({T("You lack the insight\nto pick that.\n"), {0xf1, 0x24, 0}});
// Crafting pages (the layout of a live capture: F686 / F48A / F761 controls, context variables).
const std::vector<uint8_t> kCraftControls{0xf6, 0x86, 0x01, 0x01, 0x04, 0x01, 0x01, 0x01, 0x01,
                                          0x01, 0x01, 0x01, 0xf4, 0x8a, 0x00, 0x00, 0x01, 0x01,
                                          0x01, 0x01, 0xf7, 0x61, 0x09, 0x01, 0x03, 0x01};
const std::vector<uint8_t> kPage_craft_left =
    MakePage({kCraftControls,
              {0x01, 0x01, 0x97, 0x05, 0x00, 0x00, 0x01, 0x01},
              T("Maybe you can build "),
              {0xf2, 0x44, 0x01, 0x01},
              T(".\nHave fun.\n"),
              {0xf1, 0x21, 0}});
const std::vector<uint8_t> kPage_craft_confirm =
    MakePage({kCraftControls,
              {0x02, 0x01, 0x98, 0x05, 0x00, 0x00, 0x01, 0x01},
              T("So, build "),
              {0xf2, 0x44, 0x03, 0x01},
              T("\npieces of "),
              {0xf4, 0x45, 0x04, 0x01, 0x10, 0x28, 0x01, 0x01},
              T("?\n"),
              {0xf1, 0x21, 0}});
// Two-byte name buffers (0x80, ASCII + 0x60 per character), tag byte 0: "Kaito" / "Mori".
const std::vector<uint8_t> kName2_a{0x80, 0xab, 0x80, 0xc1, 0x80, 0xc9, 0x80, 0xd4, 0x80, 0xcf, 0};
const std::vector<uint8_t> kName2_b{0x80, 0xad, 0x80, 0xcf, 0x80, 0xd2, 0x80, 0xc9, 0};
int failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL " << what << '\n';
        ++failures;
    }
}
struct Mem {
    std::map<U, uint8_t> ram;
    U flip_at{}; // flip one byte on the Nth read of this address (torn-read test)
    unsigned flip_read{}, flip_count{};
    void put(U at, const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i)
            ram[at + i] = static_cast<const uint8_t*>(p)[i];
    }
    template <class T>
    void put(U at, T v) {
        static_assert(!std::is_pointer_v<T>);
        put(at, &v, sizeof v);
    }
    void bytes(U at, const Bytes& b) {
        put(at, b.data(), b.size());
    }
    void str(U at, const std::string& s) {
        put(at, s.c_str(), s.size() + 1);
    }
    bool read(U at, void* out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            auto it = ram.find(at + i);
            if (it == ram.end())
                return false;
            static_cast<uint8_t*>(out)[i] = it->second;
        }
        if (flip_at && at == flip_at && ++flip_count == flip_read)
            static_cast<uint8_t*>(out)[0] ^= 1;
        return true;
    }
};
constexpr U Ctx = 0x10000000, Bmd = 0x11000000, Vars = Ctx + 0x198;
constexpr U Main = 0x20000000;
// One message (record 0) with the given pages; speaker name table entry 0 = "Guide".
void Message(Mem& m, const std::vector<Bytes>& pages, uint16_t speaker = 0xffff,
             uint16_t page = 0) {
    m.ram.clear();
    m.put(Ctx, std::array<uint8_t, 0x90>{});
    m.put(Bmd, std::array<uint8_t, 0x2000>{});
    m.put(Bmd + 4, uint32_t(0x2000));
    m.put(Bmd + 8, uint32_t(0x3147534d));
    m.put(Bmd + 0x18, uint32_t(1));
    m.put(Bmd + 0x24, int32_t(0x100 - 0x24));
    m.put(Bmd + 0x28, int32_t(0x80 - 0x28));
    m.put(Bmd + 0x2c, uint32_t(1));
    m.put(Bmd + 0x80, int32_t(0x90 - 0x80));
    m.str(Bmd + 0x90, "Guide");
    m.put(Bmd + 0x118, uint16_t(pages.size()));
    m.put(Bmd + 0x11a, speaker);
    for (size_t i = 0; i < pages.size(); ++i) {
        const U at = Bmd + 0x200 + 0x200 * i;
        m.put(Bmd + 0x11c + 4 * i, int32_t(at - (Bmd + 0x11c + 4 * i)));
        m.bytes(at, pages[i]);
    }
    m.put(Ctx + 8, Bmd);
    m.put(Ctx + 0x30, Bmd + 0x100);
    m.put(Ctx + 0x38, U(0x12000000));
    m.put(Ctx + 0x40, int8_t(1));
    m.put(Ctx + 0x4a, page);
    m.put(Vars, std::array<U, 32>{});
}
// Synthetic data in the native layouts behind each accessor (values are made up).
p5r_dialogue::Globals World(Mem& m, uint32_t order = 0, int8_t tag = 2, uint32_t text_lang = 2) {
    p5r_dialogue::Globals g;
    g.order_language_slot = Main + 0x10;
    g.text_language_slot = Main + 0x18;
    m.put(Main + 0x10, U(Main + 0x20));
    m.put(Main + 0x20, order);
    m.put(Main + 0x18, U(Main + 0x28));
    m.put(Main + 0x28, text_lang);
    g.name_a = Main + 0x100;
    g.name_b = Main + 0x114;
    g.full_name = Main + 0x128;
    g.name_language = Main + 0x1b8;
    m.put(Main + 0x100, std::array<uint8_t, 0xc0>{});
    m.str(g.name_a, "Kai");
    m.str(g.name_b, "Tanaka");
    m.str(g.full_name, "Kai Tanaka");
    m.put(g.name_language, tag);
    g.thieves = Main + 0x200;
    g.thieves_language = Main + 0x269;
    m.put(g.thieves, std::array<uint8_t, 0x70>{});
    m.str(g.thieves, "Night Crew");
    m.put(g.thieves_language, tag);
    // Item category 3 table {base, offsets}: 0x3005 -> "Rice", 0x3006 -> "Caf" 849E.
    g.item_tables[3] = Main + 0x300;
    m.put(Main + 0x300, U(Main + 0x400));
    m.put(Main + 0x308, U(Main + 0x380));
    m.put(Main + 0x380 + 2 * 5, uint16_t(0x10));
    m.put(Main + 0x380 + 2 * 6, uint16_t(0x20));
    m.str(Main + 0x410, "Rice");
    m.bytes(Main + 0x420, {'C', 'a', 'f', 0x84, 0x9e, 0});
    // Unit table 1 (894030 -> 89BD70): unit 2 -> "Momo", unit 10 unused (flag path).
    g.unit_tables[1] = Main + 0x500;
    m.put(Main + 0x500, U(Main + 0x600));
    m.put(Main + 0x508, U(Main + 0x580));
    m.put(Main + 0x580 + 2 * 2, uint16_t(8));
    m.str(Main + 0x608, "Momo");
    // FTD-shaped stat tables: t+0x10 = data offset; names data+0x10+20*i, count at data+8.
    g.stat_names_slot = Main + 0x700;
    m.put(Main + 0x700, U(Main + 0x800));
    m.put(Main + 0x810, uint32_t(0x20));
    m.put(Main + 0x828, uint32_t(5));
    const char* stats[] = {"Wisdom", "Nerve", "Skill", "Warmth", "Style"};
    for (int i = 0; i < 5; ++i) {
        m.put(Main + 0x830 + 20 * i, std::array<uint8_t, 20>{});
        m.str(Main + 0x830 + 20 * i, stats[i]);
    }
    g.stat_levels_slot = Main + 0x708;
    m.put(Main + 0x708, U(Main + 0x900));
    m.put(Main + 0x910, uint32_t(0x20));
    m.str(Main + 0x920 + 100 * 3 + 20 * 3 - 4, "Gentle"); // stat 3, level 3
    m.str(Main + 0x920 + 100 * 3 + 20 * 4 - 4, "Kindly"); // stat 3, level 4
    return g;
}
p5r_dialogue::Snapshot Read(Mem& m, const p5r_dialogue::Globals* g) {
    return p5r_dialogue::ReadContext([&](U a, void* p, size_t n) { return m.read(a, p, n); }, Ctx,
                                     g);
}
Bytes B(std::initializer_list<int> v) {
    Bytes b;
    for (int x : v)
        b.push_back(uint8_t(x));
    return b;
}
Bytes S(const std::string& s) {
    return Bytes(s.begin(), s.end());
}
Bytes Cat(std::initializer_list<Bytes> parts) {
    Bytes b;
    for (auto& p : parts)
        b.insert(b.end(), p.begin(), p.end());
    return b;
}
std::string Page(Mem& m, const p5r_dialogue::Globals* g, const Bytes& page,
                 p5r_dialogue::Snapshot* out = nullptr) {
    Message(m, {Cat({page, B({0})})});
    auto s = Read(m, g);
    if (out)
        *out = s;
    return s.ready ? s.text : std::string("<fallback>");
}
} // namespace

int main() {
    using p5r_dialogue::Reason;
    Mem m;
    // ---- glyph map (atlas-verified entries) and fail-closed glyphs, no guest access
    {
        std::string out;
        check(p5r_dialogue::Decode(B({'C', 'a', 'f', 0x84, 0x9e, 0}).data(), 6, out) &&
                  out == "Caf\xc3\xa9",
              "e-acute glyph 849E");
        check(p5r_dialogue::Decode(B({'W', 0x83, 0xd2, 0x80, 0x80, 'x', 0x80, 0x86, 0}).data(), 9,
                                   out) &&
                  out == "W\xe2\x80\x94 x&",
              "em dash, ASCII duplicates 8080/8086");
        check(p5r_dialogue::Decode(B({0x82, 0x9d, 0}).data(), 3, out) && out == "\xe2\x80\xa6",
              "ellipsis 829D");
        check(p5r_dialogue::Decode(B({0x82, 0xbc, '1', '5', '0', 0x9c, 0xa3, 0}).data(), 8, out) &&
                  out == "\xc2\xa5"
                         "150\xe2\x99\xaa",
              "yen and note glyphs");
        p5r_dialogue::Fallback why;
        check(!p5r_dialogue::Decode(B({'a', 0x80, 0xe2, 0}).data(), 4, out, {}, &why) &&
                  out.empty() && why.reason == Reason::Glyph && why.code == 0x80e2,
              "unlisted Japanese glyph falls back with its code");
        why = {};
        check(!p5r_dialogue::Decode(B({'a', 0x80, 0xdf, 0}).data(), 4, out, {}, &why) &&
                  why.reason == Reason::Glyph,
              "atlas index 95 is not ASCII DEL");
        check(!p5r_dialogue::Decode(B({'a', 'b'}).data(), 2, out), "unterminated text fails");
    }
    // ---- pages whose controls are all native no-text handlers
    check(Page(m, nullptr, kPage_glyph_cafe) == "The sign reads\n\"Tea and Caf\xc3\xa9\" corner...",
          "glyph page (e-acute)");
    check(Page(m, nullptr, kPage_end124) == "You lack the insight\nto pick that.", "F124 page end");
    {
        const Bytes page =
            Cat({B({0xf1, 0xae}), B({0xf7, 0x97, 2, 1, 0x15, 1, 0x16, 1, 6, 1, 0x15, 1, 1, 1}),
                 S("Hi"), B({0xf2, 0x22, 0x2e, 1}), B({0xf1, 0x23}), S(" there"),
                 B({0xfd, 0x91, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                    1,    1,    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}),
                 B({0xf4, 0x08, 3, 1, 2, 1, 0xaa, 9}), B({0xf2, 0x04, 0x81, 1}),
                 B({0xf5, 0x47, 1, 1, 1, 1, 0x6b, 1, 2, 1}), B({0xf4, 0xad, 4, 1, 3, 1, 7, 1}),
                 B({0xf3, 0x9a, 1, 1, 1, 1}), B({0xf1, 0xaf}), B({0xf2, 0x23, 0, 0}), S("!\n"),
                 B({0xf1, 0x24}), S("hidden")});
        check(Page(m, nullptr, page) == "Hi there!",
              "skip set preserves text, NUL inside control args, F124 ends the page");
    }
    {
        p5r_dialogue::Snapshot s;
        check(Page(m, nullptr, kPage_button_icon, &s) == "<fallback>" && s.unsupported &&
                  s.fallback.reason == Reason::Control && s.fallback.code == 0xf22a &&
                  s.fallback.size == 4 && s.fallback.bytes[2] == 0x17,
              "button icon F22A stays a control fallback with its argument bytes");
        check(Page(m, nullptr, Cat({S("x"), B({0xf2, 0x03, 2, 1}), S("y")}), &s) == "<fallback>" &&
                  s.fallback.code == 0xf203,
              "font switch F203 falls back");
        check(Page(m, nullptr, Cat({S("x"), B({0xf1, 0x25}), S("y\n")})) == "<fallback>",
              "line-capture F125 falls back");
        check(Page(m, nullptr, kPage_name_b, &s) == "<fallback>" &&
                  s.fallback.reason == Reason::Substitution && s.fallback.code == 0xf182,
              "names without resolved globals fall back as substitution");
    }
    // ---- names / thieves / items / stats / units through the native layouts
    p5r_dialogue::Globals g;
    auto text = [&](const Bytes& page, uint32_t order = 0, int8_t tag = 2,
                    p5r_dialogue::Snapshot* s = nullptr) {
        Message(m, {Cat({page, B({0})})});
        g = World(m, order, tag);
        auto r = Read(m, &g);
        if (s)
            *s = r;
        return r.ready ? r.text : std::string("<fallback>");
    };
    check(text(kPage_name_a) == "Hey, Kai-san? Need\nsomething?", "F181 = 880B10 buffer");
    check(text(kPage_name_b).rfind("Tanaka's gift lets him", 0) == 0, "F182 = 880BA0 buffer");
    check(text(B({0xf1, 0x83})) == "Kai Tanaka", "F183 = A ' ' B");
    check(text(B({0xf1, 0x83}), 1) == "Tanaka Kai", "F183 order swaps when language==1");
    check(text(B({0xf1, 0x81}), 1) == "Kai", "F181 is buffer A in both orders");
    check(text(B({0xf1, 0x81}), 0, -1) == "Kai", "unset name language tag accepted");
    check(text(B({0xf1, 0x81}), 0, 9) == "<fallback>", "foreign name language fails closed");
    check(text(B({0xf1, 0x81}), 3) == "<fallback>", "language 3 parenthesis mode fails closed");
    {
        Message(m, {B({0xf1, 0x81, 0})});
        g = World(m);
        m.put(g.name_a, uint8_t(0));
        check(!Read(m, &g).ready, "empty name (native default string) fails closed");
    }
    check(text(kPage_thieves) == "Get some rest. From\nnext week on we work as\nNight Crew.",
          "F19C thieves name");
    check(text(kPage_stat_icon_name) == "I think my Skill\nwill grow soon...",
          "F28B icon omitted, F28C stat name");
    check(text(kPage_stat_level) ==
              "I cannot leave the\nshop until I have\nenough Warmth to be Gentle.",
          "F38D stat level label");
    check(text(Cat({S("["), B({0xf2, 0x8c, 6, 1}), S("]")})) == "[]",
          "F28C stat >4 appends nothing natively");
    check(text(Cat({B({0xf3, 0x8d, 4, 1, 5, 1})})) == "Kindly", "F38D level argument is 1-based");
    check(text(Cat({S("["), B({0xf3, 0x8d, 4, 1, 7, 1}), S("]")})) == "[]",
          "F38D level >5 appends nothing natively");
    check(text(Cat({S("Send to "), B({0xf3, 0x9f, 3, 1, 2, 1})})) == "Send to Momo",
          "F39F unit table");
    check(text(B({0xf3, 0x9f, 2, 1, 1, 1})) == "Kai Tanaka", "F39F unit 1 full name");
    check(text(B({0xf3, 0x9f, 2, 1, 2, 1})) == "Kai", "F39F unit 1 kind 1");
    check(text(B({0xf3, 0x9f, 2, 1, 3, 1}), 1) == "Kai", "F39F unit 1 kind 2 swapped order");
    check(text(kPage_unit) == "<fallback>", "F39F unit 10 needs flag 0x849");
    check(text(B({0xf4, 0x84, 1, 1, 1, 1, 7, 0x31})) == "Caf\xc3\xa9",
          "F484 item name, nested glyph");
    check(text(B({0xf4, 0x84, 1, 1, 1, 1, 6, 0x31})) == "Rice", "F484 item id argument 3");
    check(text(B({0xf4, 0x84, 1, 1, 1, 1, 6, 0x81})) == "<fallback>",
          "F484 category 8 (language path) fails closed");
    // ---- context string variables (context+198, installed by EE15B0)
    auto vars = [&](const Bytes& page, std::initializer_list<std::pair<int, Bytes>> v,
                    p5r_dialogue::Snapshot* s = nullptr) {
        Message(m, {Cat({page, B({0})})});
        g = World(m);
        U at = 0x30000000;
        for (auto& [i, b] : v) {
            m.put(Vars + 8 * i, at);
            m.bytes(at, b);
            at += 0x100;
        }
        auto r = Read(m, &g);
        if (s)
            *s = r;
        return r.ready ? r.text : std::string("<fallback>");
    };
    auto z = [](const std::string& s) { return Bytes(s.c_str(), s.c_str() + s.size() + 1); };
    check(vars(kPage_var_date, {{0, z("5")}, {1, z("20")}}) ==
              "The draw is held in a few days,\non 5/20.\nDo not miss it.",
          "F244 context variables");
    check(vars(Cat({S("["), B({0xf2, 0x44, 3, 1}), S("]")}), {}) == "[]",
          "null variable appends nothing");
    check(vars(B({0xf2, 0x44, 0x21, 1}), {}) == "<fallback>", "variable index >=32 fails closed");
    check(vars(B({0xf2, 0x44, 1, 1}), {{0, B({0xf2, 0x05, 0xff, 0xff, 'O', 'K', 0})}}) == "OK",
          "skip controls allowed inside a variable");
    check(vars(B({0xf2, 0x44, 1, 1}), {{0, B({0xf1, 0x81, 0})}}) == "<fallback>",
          "no nested substitution inside a variable");
    check(vars(kPage_item_var, {{0, B({0x05, 0x30, 0, 0})}}) ==
              "Our Rice sells well...\nWe get more on Mondays,\nmost weeks.",
          "F445 item id from variable, icon omitted");
    check(vars(B({0xf4, 0x45, 4, 1, 2, 1, 1, 1}), {{0, z("Latte")}}) == "Latte",
          "F445 non-item mode prints the variable string");
    {
        p5r_dialogue::Snapshot s;
        Message(m, {B({'H', 'i', 0})}, 0x8001);
        g = World(m);
        m.put(Vars + 8, U(0x30000000));
        m.str(0x30000000, "Keeper");
        s = Read(m, &g);
        check(s.ready && s.speaker == "Keeper" && s.text == "Hi",
              "high-bit speaker from context+198");
        m.put(Vars + 8, U(0));
        s = Read(m, &g);
        check(s.ready && s.speaker.empty(), "null high-bit speaker shows no name");
        Message(m, {B({'H', 'i', 0})}, 0x8021);
        g = World(m);
        s = Read(m, &g);
        check(!s.ready && s.unsupported && s.fallback.reason == Reason::Substitution,
              "speaker variable >=32 fails closed");
        Message(m, {B({'H', 'i', 0}), B({'2', 0})}, 0, 1);
        s = Read(m, &g);
        check(s.ready && s.speaker == "Guide" && s.text == "2" && s.message == 0 && s.page == 1,
              "plain speaker, message and page ids");
        // A variable changing between the two reads of one sample is reported as torn.
        Message(m, {B({0xf2, 0x44, 1, 1, 0})});
        g = World(m);
        m.put(Vars, U(0x30000000));
        m.str(0x30000000, "7");
        m.flip_at = Vars;
        m.flip_read = 2;
        m.flip_count = 0;
        s = Read(m, &g);
        check(s.torn && !s.ready && s.text.empty(), "variable table change between reads is torn");
        m.flip_at = 0;
    }
    // ---- crafting-page layout and two-byte name buffers
    {
        check(vars(kPage_craft_left, {{0, z("3")}}) == "Maybe you can build 3.\nHave fun.",
              "F244 variable page");
        Message(m, {kPage_craft_confirm});
        g = World(m);
        m.put(Vars + 8 * 2, U(0x30000000));
        m.str(0x30000000, "1");
        m.put(Vars, U(0x30000100));
        m.put(0x30000100, uint32_t(0x3006)); // synthetic item id; table name is synthetic too
        auto s = Read(m, &g);
        check(s.ready && s.text == "So, build 1\npieces of Caf\xc3\xa9?",
              "F244+F445 crafting page decodes with item icon omitted");
        // Two-byte buffers, language state order 1 (F183 = B ' ' A), tag 0 == text 0.
        Message(m, {B({0xf1, 0x81, ' ', 0xf1, 0x82, ' ', '/', ' ', 0xf1, 0x83, 0})});
        g = World(m, 1, 0, 0);
        m.put(g.name_a, std::array<uint8_t, 0x14>{});
        m.put(g.name_b, std::array<uint8_t, 0x14>{});
        m.bytes(g.name_a, kName2_a);
        m.bytes(g.name_b, kName2_b);
        s = Read(m, &g);
        check(s.ready && s.text == "Kaito Mori / Mori Kaito", "two-byte name buffers");
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "PASS p5r dialogue reader tests (synthetic pages and values)\n";
    return 0;
}
