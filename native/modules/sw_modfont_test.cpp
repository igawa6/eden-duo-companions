// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
// Module fonts (sw_modfont.h): the BFOTF container on a synthetic file, the measuring hook, the
// shared-system-font probe (Korean / Chinese offered only when the host returns font bytes), and --
// with SW_ROMFS_DIR pointing at an extracted romfs (never in the repository) -- the real faces:
// every ja / ja-kana string and every game text code point the module shows drawn by
// FOT-RodinNTLGPro-DB, the full Japanese atlas within 3 pages, and the module's BFFNT rebuild
// equal to the package font's own metrics (sw_font_metrics.h, generated from the same file). With
// SW_SYSFONT_DIR also set (decrypted firmware shared fonts, <name>.ttf, as the runtime's "sysfont:"
// source returns them; never in the repository): every ko / zh-Hans / zh-Hant string and game text
// code point in its face chain, the full Korean / Chinese atlases within 3 pages, and a live
// ko -> en -> zh-Hant switch.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sw_font_metrics.h"
#include "sw_lang.h"
#include "sw_modfont.h"
#include "sw_strings.h"

namespace {
using sw_lang::Lang;

std::vector<std::uint8_t> Encode(const std::vector<std::uint8_t>& payload, std::uint32_t magic = 0x7F9A0218) {
    constexpr std::uint32_t kKey = 0x49621806;
    std::vector<std::uint8_t> padded = payload;
    while (padded.size() % 4) padded.push_back(0);
    std::vector<std::uint8_t> out;
    const auto put = [&](std::uint32_t w) {
        w ^= kKey;
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(w >> s));
    };
    put(magic);
    put(static_cast<std::uint32_t>(payload.size()));
    for (std::size_t i = 0; i < padded.size(); i += 4)
        put((std::uint32_t{padded[i]} << 24) | (std::uint32_t{padded[i + 1]} << 16) | (std::uint32_t{padded[i + 2]} << 8) | padded[i + 3]);
    return out;
}

void TestBfotfContainer() {
    std::vector<std::uint8_t> otf = {'O', 'T', 'T', 'O', 0, 9, 0, 0x80, 0, 3, 0x42};
    for (int i = 0; i < 200; ++i) otf.push_back(static_cast<std::uint8_t>(i * 37));
    const auto file = Encode(otf);
    // the real files start 36 F8 1A 1E (0x7F9A0218 ^ 0x49621806)
    assert(file[0] == 0x36 && file[1] == 0xF8 && file[2] == 0x1A && file[3] == 0x1E);
    const auto back = sw_modfont::DecodeBfotf(file);
    assert(back && *back == otf);
    assert(!sw_modfont::DecodeBfotf(Encode(otf, 0x12345678)));            // not a BFOTF
    auto truncated = file;
    truncated.resize(truncated.size() - 8);
    assert(!sw_modfont::DecodeBfotf(truncated));                           // payload size past the end
    assert(!sw_modfont::DecodeBfotf(std::vector<std::uint8_t>(4, 0)));     // no header
}

const char* RomfsDir() {
    const char* d = std::getenv("SW_ROMFS_DIR");
    return d && *d ? d : nullptr;
}

std::size_t ReadRomfs(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    std::string p = path;
    if (p.rfind("romfs:", 0) == 0) p = p.substr(6);
    std::ifstream f(std::string(RomfsDir()) + p, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const auto total = static_cast<std::size_t>(f.tellg());
    if (!out) return total;
    if (offset >= total) return 0;
    const std::size_t n = std::min<std::size_t>(size, total - offset);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return n;
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// The host's "sysfont:" reads for the probe test: which names answer with which bytes.
std::map<std::string, std::vector<std::uint8_t>> g_sysfont;
int g_sysfont_reads = 0;
std::size_t FakeSysfont(void*, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    ++g_sysfont_reads;
    const auto it = g_sysfont.find(path);
    if (it == g_sysfont.end()) return 0;
    if (!out) return it->second.size();
    if (offset >= it->second.size()) return 0;
    const std::size_t n = std::min<std::size_t>(size, it->second.size() - offset);
    std::memcpy(out, it->second.data() + offset, n);
    return n;
}

// Korean / Chinese are offered only when the runtime 23 source returns real font bytes: no
// capability (runtime 22) -> never asked; empty reads (no firmware fonts) or non-font bytes -> not
// offered; each language by its own primary font.
void TestSysfontProbe() {
    EdenDsmodHostApi host{};
    host.read_romfs = &FakeSysfont;
    std::vector<std::uint8_t> ttf(64, 0);
    ttf[1] = 1; // 00 01 00 00: TrueType
    g_sysfont.clear();
    g_sysfont[std::string(sw_modfont::kSysfontKo)] = ttf;
    g_sysfont[std::string(sw_modfont::kSysfontZhCn)] = ttf;
    g_sysfont[std::string(sw_modfont::kSysfontZhTw)] = ttf;
    // runtime 22: the capability bit is not set -- nothing offered, nothing read
    g_sysfont_reads = 0;
    host.capabilities = 0;
    assert(sw_modfont::ProbeSysfonts(host) == 0 && g_sysfont_reads == 0);
    assert(!sw_lang::Offered(7) && !sw_lang::Offered(8) && !sw_lang::Offered(9));
    assert(sw_lang::FromSetting(7) == Lang::English);
    // runtime 23 with real fonts: all three
    host.capabilities = sw_modfont::kCapSourceSysfont;
    assert(sw_modfont::ProbeSysfonts(host) == ((1u << 7) | (1u << 8) | (1u << 9)));
    assert(sw_lang::Offered(7) && sw_lang::Offered(8) && sw_lang::Offered(9));
    assert(sw_lang::FromSetting(8) == Lang::ChineseSimplified);
    // runtime 23 without firmware fonts: the source reads empty -> none (and a persisted 7 is English)
    g_sysfont.clear();
    assert(sw_modfont::ProbeSysfonts(host) == 0);
    assert(!sw_lang::Offered(7) && sw_lang::FromSetting(7) == Lang::English && sw_lang::NextOffered(6) == 0);
    // only Traditional Chinese's font; Korean's bytes are not a font
    g_sysfont[std::string(sw_modfont::kSysfontZhTw)] = ttf;
    g_sysfont[std::string(sw_modfont::kSysfontKo)] = std::vector<std::uint8_t>(64, 0x5A);
    assert(sw_modfont::ProbeSysfonts(host) == (1u << 9));
    assert(sw_lang::NextOffered(6) == 9 && sw_lang::NextOffered(9) == 0);
    // the ext_zh-cn companion alone does not offer Simplified Chinese
    g_sysfont.clear();
    g_sysfont[std::string(sw_modfont::kSysfontZhCnExt)] = ttf;
    assert(sw_modfont::ProbeSysfonts(host) == 0);
    assert(sw_modfont::SysfontsOf(Lang::ChineseSimplified).size() == 2 && sw_modfont::SysfontsOf(Lang::English).empty());
    // a Font asked for Korean without the font: no build, the epoch stays
    sw_modfont::Font font;
    font.Update(host, Lang::Korean, 0);
    assert(font.epoch() == 0 && !font.stats().module_font);
    sw_lang::SysfontOfferRef() = 0;
}

void TestRealFaces() {
    if (!RomfsDir()) {
        std::cout << "SW_ROMFS_DIR not set: real-font checks skipped\n";
        return;
    }
    const std::string root = RomfsDir();
    auto otf = sw_modfont::DecodeBfotf(ReadFile(root + "/bin/font/FOT-RodinNTLGPro-DB.BFOTF"));
    assert(otf && otf->size() > 1000000 && std::memcmp(otf->data(), "OTTO", 4) == 0);
    sw_modfont::Face face;
    assert(face.Open(*otf, sw_modfont::kCapPx));
    // every Japanese companion string is drawable by the game's own face (with the game lines
    // the companion shows, read from the romfs as the module does)
    int checked = 0;
    for (const Lang lang : {Lang::JapaneseKanji, Lang::Japanese}) {
        EdenDsmodHostApi romfs{};
        romfs.read_romfs = &ReadRomfs;
        sw_str::LoadGame(romfs, lang);
        for (int i = 0; i < static_cast<int>(sw_str::Id::kCount); ++i) {
            std::set<std::uint32_t> cps;
            sw_modfont::AddCodepoints(sw_str::S(static_cast<sw_str::Id>(i), lang), cps);
            for (const auto cp : cps) {
                if (!face.Covers(cp)) {
                    std::cerr << "no Rodin glyph U+" << std::hex << cp << std::dec << " in " << sw_str::kKeys[i] << " "
                              << sw_lang::Code(lang) << "\n";
                    assert(false);
                }
                ++checked;
            }
        }
    }
    assert(checked > 1000);
    // The module's own measuring follows the face while it is set.
    assert(!sw_font::CoversText("\xE3\x83\x94\xE3\x82\xAB\xE3\x83\x81\xE3\x83\xA5\xE3\x82\xA6")); // ピカチュウ
    sw_modfont::SetMeasuringFace(&face);
    assert(sw_font::CoversText("\xE3\x83\x94\xE3\x82\xAB\xE3\x83\x81\xE3\x83\xA5\xE3\x82\xA6"));
    const int w = sw_font::MeasureText("\xE3\x83\x94\xE3\x82\xAB\xE3\x83\x81\xE3\x83\xA5\xE3\x82\xA6", 5);
    assert(w > 5 * 20 && w < 5 * 40); // five kana at a 25 px cap
    sw_modfont::SetMeasuringFace(nullptr);
    assert(!sw_font::CoversText("\xE3\x83\x94"));

    // The whole Japanese set the module builds (strings + every text table it reads), through the
    // real Font with a romfs-backed host, for both scripts: built, served, within 3 pages.
    EdenDsmodHostApi host{};
    host.read_romfs = &ReadRomfs;
    for (const Lang lang : {Lang::JapaneseKanji, Lang::Japanese}) {
        sw_modfont::Font font;
        font.Update(host, lang, 0);
        for (int i = 0; i < 600 && font.epoch() == 0; ++i) {
            struct timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        assert(font.epoch() == 1);
        struct Sink {
            std::uint32_t line = 0, first = 0, count = 0;
        } sink;
        assert(font.Decode(&sink, [](void* r, std::uint32_t line, std::uint32_t first, const EdenDsmodFontGlyph*, std::uint32_t n) {
            auto& s = *static_cast<Sink*>(r);
            s.line = line;
            s.first = first;
            s.count = n;
        }));
        const auto st = font.stats();
        std::cout << sw_lang::Code(lang) << ": " << st.glyphs << " glyphs (" << st.missing << " missing), cap " << st.cap_px
                  << " px, " << st.pages << " page(s) " << st.width << "x" << st.height << ", " << (st.atlas_bytes >> 10)
                  << " KiB, " << st.build_ms << " ms" << std::endl;
        assert(st.pages >= 1 && st.pages <= sw_modfont::kMaxPages && st.cap_px >= sw_modfont::kMinCapPx);
        assert(sink.line == st.cap_px && sink.first == 0x20 && sink.count > 0x3000);
        assert(st.missing <= 4); // U+E300-style private glyphs at most
        // page 0 is served as "module:sw:font/0"
        bool handled = false;
        struct Img {
            std::uint32_t w = 0, h = 0;
        } img;
        assert(font.LoadImage(host, "module:sw:font/0", &img, [](void* r, std::uint32_t w, std::uint32_t h, const std::uint8_t*, std::size_t) {
            static_cast<Img*>(r)->w = w;
            static_cast<Img*>(r)->h = h;
        }, handled) && handled && img.w == st.width && img.h == std::min(sw_modfont::kPageH, st.height));
        assert(!font.LoadImage(host, "module:sw:font/9", &img, [](void*, std::uint32_t, std::uint32_t, const std::uint8_t*, std::size_t) {}, handled) && handled);
        assert(!font.LoadImage(host, "module:sw:other", &img, [](void*, std::uint32_t, std::uint32_t, const std::uint8_t*, std::size_t) {}, handled) && !handled);
        // ... and back to English: the module's BFFNT copy, epoch 2, the measuring hook off again
        font.Update(host, Lang::English, 10000);
        for (int i = 0; i < 600 && font.epoch() == 1; ++i) {
            struct timespec ts{0, 10 * 1000 * 1000};
            nanosleep(&ts, nullptr);
        }
        // the module logs an epoch's stats before the host decodes it: they describe the new build
        assert(font.epoch() == 2 && font.stats().kind == sw_modfont::Kind::Latin);
        assert(font.Decode(&sink, [](void*, std::uint32_t, std::uint32_t, const EdenDsmodFontGlyph*, std::uint32_t) {}));
        assert(!sw_font::CoversText("\xE3\x83\x94"));
        assert(font.stats().kind == sw_modfont::Kind::Latin && font.stats().pages == 2);
    }

    // The BFFNT rebuild == the package font's own table (the same numbers the host computes).
    const auto bffnt = ReadFile(root + "/bin/font/bmp/font_fs_72_00.bffnt");
    const auto atlas = sw_modfont::BuildBffntAtlas(bffnt);
    assert(atlas && atlas->line_height == static_cast<std::uint32_t>(sw_font::kCap) && atlas->first == 0x20);
    assert(atlas->width == 1024 && atlas->page_h == 1024 && atlas->height == 2048);
    for (char32_t cp = 0x20; cp <= 0xFF; ++cp)
        assert(atlas->glyphs[cp - 0x20].advance == sw_font::Advance(cp));
    for (const auto& e : sw_font::kExtra)
        if (sw_font::Covers(e.cp)) assert(atlas->glyphs[e.cp - 0x20].advance == e.adv);
    // 'H' has ink, and the sheets are upright (some ink in its top half)
    const auto& h = atlas->glyphs['H' - 0x20];
    std::uint32_t ink = 0;
    for (std::uint32_t y = 0; y < h.h / 2u; ++y)
        for (std::uint32_t x = 0; x < h.w; ++x) ink += atlas->rgba[((h.y + y) * atlas->width + h.x + x) * 4 + 3] > 128;
    assert(ink > 50);
}
// Review 3 C2: noted glyphs rebuild the atlas at most once per 3 s and at most kMaxExtra of them;
// once a grown set no longer fits the caps, no further rebuild is ever attempted (the set only
// grows, so each retry would fail again) until a language change resets it.
const char* SysfontDir() {
    const char* d = std::getenv("SW_SYSFONT_DIR");
    return d && *d ? d : nullptr;
}
// "sysfont:<name>" -> SW_SYSFONT_DIR/<name>.ttf (what the runtime 23 source returns), the rest romfs.
std::size_t ReadRomfsAndSysfont(void* u, const char* path, std::uint64_t offset, void* out, std::size_t size) {
    std::string p = path;
    if (p.rfind("sysfont:", 0) != 0) return ReadRomfs(u, path, offset, out, size);
    std::ifstream f(std::string(SysfontDir()) + "/" + p.substr(8) + ".ttf", std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const auto total = static_cast<std::size_t>(f.tellg());
    if (!out) return total;
    if (offset >= total) return 0;
    const std::size_t n = std::min<std::size_t>(size, total - offset);
    f.seekg(static_cast<std::streamoff>(offset));
    f.read(static_cast<char*>(out), static_cast<std::streamsize>(n));
    return n;
}

bool WaitEpoch(const sw_modfont::Font& font, std::int64_t want) {
    for (int i = 0; i < 1500 && font.epoch() < want; ++i) {
        struct timespec ts{0, 10 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    return font.epoch() == want;
}

// The real firmware 22.5.0 shared fonts (decrypted, outside the repository).
void TestRealSysfonts() {
    if (!RomfsDir() || !SysfontDir()) {
        std::cout << "SW_ROMFS_DIR / SW_SYSFONT_DIR not set: real shared-font checks skipped\n";
        return;
    }
    EdenDsmodHostApi host{};
    host.read_romfs = &ReadRomfsAndSysfont;
    host.capabilities = sw_modfont::kCapSourceSysfont;
    assert(sw_modfont::ProbeSysfonts(host) == ((1u << 7) | (1u << 8) | (1u << 9)));
    const std::string root = RomfsDir();
    auto rodin = sw_modfont::DecodeBfotf(ReadFile(root + "/bin/font/FOT-RodinNTLGPro-DB.BFOTF"));
    assert(rodin);
    const auto rodin_bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(*rodin));
    for (const Lang lang : {Lang::Korean, Lang::ChineseSimplified, Lang::ChineseTraditional}) {
        // every companion string in the language's chain, from its shared fonts (not the Rodin fallback)
        std::vector<sw_modfont::Face::Bytes> chain;
        for (const auto name : sw_modfont::SysfontsOf(lang)) {
            auto bytes = ReadFile(std::string(SysfontDir()) + "/" + std::string(name.substr(8)) + ".ttf");
            assert(bytes.size() > 100000);
            chain.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
        }
        sw_modfont::Face shared;
        assert(shared.Open(chain, sw_modfont::kCapPx));
        sw_str::LoadGame(host, lang); // the game lines the companion shows, from the romfs
        int checked = 0, from_rodin = 0;
        for (int i = 0; i < static_cast<int>(sw_str::Id::kCount); ++i) {
            std::set<std::uint32_t> cps;
            sw_modfont::AddCodepoints(sw_str::S(static_cast<sw_str::Id>(i), lang), cps);
            for (const auto cp : cps) {
                ++checked;
                if (shared.Covers(cp)) continue;
                ++from_rodin;
                std::cerr << "no shared-font glyph U+" << std::hex << cp << std::dec << " in " << sw_str::kKeys[i] << " "
                          << sw_lang::Code(lang) << "\n";
            }
        }
        assert(checked > 1000 && from_rodin == 0);
        // the Latin letters come from the shared face too (the game's KO / ZH look)
        assert(shared.Covers('A') && shared.Advance('A') > 0);
        // the whole set the module builds (strings + every text table it reads): within 3 pages
        sw_modfont::Font font;
        font.Update(host, lang, 0);
        assert(WaitEpoch(font, 1));
        assert(font.Decode(nullptr, [](void*, std::uint32_t, std::uint32_t, const EdenDsmodFontGlyph*, std::uint32_t) {}));
        const auto st = font.stats();
        std::cout << sw_lang::Code(lang) << ": " << st.glyphs << " glyphs (" << st.missing << " missing), cap " << st.cap_px
                  << " px, " << st.pages << " page(s) " << st.width << "x" << st.height << ", " << (st.atlas_bytes >> 10)
                  << " KiB, " << st.build_ms << " ms" << std::endl;
        assert(st.kind == sw_modfont::KindOf(lang) && st.pages >= 1 && st.pages <= sw_modfont::kMaxPages &&
               st.cap_px >= sw_modfont::kMinCapPx && st.missing <= 4);
        // measuring follows the shared face while it is served
        assert(sw_font::CoversText(sw_str::S(sw_str::Id::kTabMap, lang)));
    }
    // a live switch: Korean -> English (the module's BFFNT copy) -> Traditional Chinese
    sw_modfont::Font font;
    font.Update(host, Lang::Korean, 0);
    assert(WaitEpoch(font, 1));
    assert(font.Decode(nullptr, [](void*, std::uint32_t, std::uint32_t, const EdenDsmodFontGlyph*, std::uint32_t) {}) &&
           font.stats().kind == sw_modfont::Kind::Korean);
    font.Update(host, Lang::English, 100);
    assert(WaitEpoch(font, 2));
    struct Sink {
        std::uint32_t first = 0, count = 0;
    } sink;
    const auto take = [](void* r, std::uint32_t, std::uint32_t first, const EdenDsmodFontGlyph*, std::uint32_t n) {
        static_cast<Sink*>(r)->first = first;
        static_cast<Sink*>(r)->count = n;
    };
    assert(font.Decode(&sink, take) && font.stats().kind == sw_modfont::Kind::Latin && !sw_font::CoversText("\xEB\xB3\xBC"));
    font.Update(host, Lang::ChineseTraditional, 200);
    assert(WaitEpoch(font, 3));
    assert(font.Decode(&sink, take) && font.stats().kind == sw_modfont::Kind::Hant && sink.first == 0x20);
    assert(sw_font::CoversText("\xE5\xAF\xB6\xE5\x8F\xAF\xE5\xA4\xA2")); // 寶可夢
    sw_lang::SysfontOfferRef() = 0;
}

void TestGlyphGrowthStopsAtSaturation() {
    using sw_modfont::GlyphGrowth;
    GlyphGrowth g;
    g.Reset(1000);
    assert(!g.Due(1000));                 // nothing noted
    assert(g.Accept() && g.Accept());
    assert(!g.Due(3999) && g.Due(4000));  // paced: 3 s after the base build
    assert(!g.Due(4001));                 // consumed
    g.Result(true);                       // that rebuild fit
    assert(g.Accept() && !g.Due(6999) && g.Due(7000));
    g.Result(false);                      // this one did not: saturated
    assert(g.saturated());
    int attempts = 0;
    for (std::uint64_t t = 7000; t < 7000 + 3600 * 1000; t += 500) { // an hour of new rare glyphs
        if (g.Accept()) assert(false);
        attempts += g.Due(t) ? 1 : 0;
    }
    assert(attempts == 0 && g.extra() == 3);
    // the cap on noted glyphs, without any failed build
    GlyphGrowth c;
    c.Reset(0);
    std::size_t taken = 0;
    for (int i = 0; i < 5000; ++i) taken += c.Accept() ? 1 : 0;
    assert(taken == GlyphGrowth::kMaxExtra && !c.saturated());
    // a language change starts over
    g.Reset(50000);
    assert(!g.saturated() && g.Accept() && g.Due(53000));
}
} // namespace

int main() {
    TestBfotfContainer();
    TestGlyphGrowthStopsAtSaturation();
    TestSysfontProbe();
    TestRealFaces();
    TestRealSysfonts();
    std::cout << "pokemon-sword font: ok\n";
    return 0;
}
