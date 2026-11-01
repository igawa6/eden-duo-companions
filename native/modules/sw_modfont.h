// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Japanese, Korean and Chinese companion text in the game's own fonts (2026-11-01, tier c;
// research: analysis/20261101-cjk-font). Japanese on runtime 22 as is; Korean / Chinese need the
// runtime 23 "sysfont:" source and are offered only where it returns the fonts:
//
//   * The manifest keeps "font": romfs:/bin/font/bmp/font_fs_72_00.bffnt. The host decodes that
//     BFFNT itself at boot (Latin languages look exactly as before) and caches its sheets under
//     the manifest's "font_atlas" key, "module:sw:font/{p}" (literally, braces included).
//   * Japanese: the module XOR-decodes romfs:/bin/font/FOT-RodinNTLGPro-DB.BFOTF in memory (never
//     written anywhere), rasterises only the code points the companion can show (strings.json
//     "ja"/"ja-kana" + the game text tables the module reads + whatever it publishes later) with
//     stb_truetype, Lumi's pipeline (lp_unity BuildFontAtlas: CJK at their own scale, shelf packed
//     into 2048 x 1024 pages, at most 3 pages = 24 MiB by lowering the raster size), and bumps
//     "__font_epoch" (runtime 18). The host then calls decode_font again; from then on the atlas is
//     paged ("font_page_h": 1024) and every page is "module:sw:font/<p>".
//   * Korean / Chinese (2026-11-01, runtime 23): Sword draws them -- Latin letters included --
//     from the console's shared system fonts (pl:u font types 4, 1+2, 3; PLU-EVIDENCE.md), so the
//     module reads the same TrueType faces through the "sysfont:" source (real firmware fonts
//     only, empty otherwise: then the language is never offered, sw_lang.h) and builds the atlas
//     the same way: Korean from nintendo_udsg-r_ko, Simplified Chinese from
//     nintendo_udsg-r_org_zh-cn then its ext_zh-cn companion, Traditional Chinese from
//     nintendo_udjxh-db_zh-tw; the Rodin face only for a code point none of them has. The font
//     bytes stay in memory (the host keeps its own decrypted copy for the session); nothing is
//     written anywhere.
//   * Back to a Latin language in the same session: the host cannot decode its BFFNT a second
//     time (a re-decode always goes to the module), so the module rebuilds the same BFFNT atlas
//     itself (CMAP/CWDH/TGLP + the BC4 sheets, mirroring the host's BuildBffntFont) and serves it as
//     pages 0..1. The pixels and metrics are the host's, so the text is identical.
//
// Threads: Update/Note/epoch on the sample thread, Decode on the host's font call (tick thread),
// LoadImage on the asset worker; builds run on one worker thread of their own.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/mods/dsmod_module_extensions.h"
#include "sw_lang.h"

namespace sw_modfont {

inline constexpr std::string_view kBfotfPath = "romfs:/bin/font/FOT-RodinNTLGPro-DB.BFOTF";
inline constexpr std::string_view kBffntPath = "romfs:/bin/font/bmp/font_fs_72_00.bffnt";
inline constexpr std::string_view kKeyPrefix = "module:sw:font/";
inline constexpr std::uint32_t kPageH = 1024;     // = the manifest's font_page_h
inline constexpr std::uint32_t kMaxPages = 3;     // 3 x 2048 x 1024 x 4 = 24 MiB < the 32 MiB page budget
inline constexpr std::uint32_t kCapPx = 44;       // raster cap height ('H'), stepped down to fit
inline constexpr std::uint32_t kMinCapPx = 28;
// The runtime 23 shared-font source (EDEN_DSMOD_CAP_SOURCE_SYSFONT; spelled out so the module also
// builds against an older SDK, where the host simply never sets it).
inline constexpr std::uint64_t kCapSourceSysfont = std::uint64_t{1} << 12;
inline constexpr std::string_view kSysfontKo = "sysfont:nintendo_udsg-r_ko_003";
inline constexpr std::string_view kSysfontZhCn = "sysfont:nintendo_udsg-r_org_zh-cn_003";
inline constexpr std::string_view kSysfontZhCnExt = "sysfont:nintendo_udsg-r_ext_zh-cn_003";
inline constexpr std::string_view kSysfontZhTw = "sysfont:nintendo_udjxh-db_zh-tw_003";

/// The shared system fonts a sysfont language is drawn from, in priority order (the first one
/// decides whether the language is offered); empty for every other language.
std::vector<std::string_view> SysfontsOf(sw_lang::Lang lang);

/// Once per boot, before the language is applied: for Korean and both Chinese, whether this host
/// returns real font bytes for the language's primary shared font (the capability bit, a non-empty
/// read, a TrueType/OpenType header -- the runtime returns nothing rather than a substitute).
/// Sets sw_lang::SetSysfontOffered for each and returns the offer mask (bit = sw_lang value).
std::uint32_t ProbeSysfonts(const EdenDsmodHostApi& host);

/// BFOTF/BFTTF container: every big-endian u32 XOR 0x49621806; word 0 = 0x7F9A0218, word 1 = the
/// payload size, the payload (an OpenType font) from byte 8. nullopt on a bad magic or size.
std::optional<std::vector<std::uint8_t>> DecodeBfotf(std::span<const std::uint8_t> file);

struct Atlas {
    std::uint32_t width = 0, height = 0, page_h = 0;
    std::vector<std::uint8_t> rgba; ///< RGBA8, RGB 0xFF, A = coverage
    std::uint32_t line_height = 0;  ///< the cap height in atlas px (the runtime's text scale unit)
    std::uint32_t first = 0;
    std::vector<EdenDsmodFontGlyph> glyphs; ///< dense from `first`
    std::vector<std::uint32_t> missing;     ///< asked for, not in the face
    std::uint32_t Pages() const { return page_h ? (height + page_h - 1) / page_h : 1; }
};

/// stb_truetype raster of `cps` (a port of lp_unity::BuildFontAtlas). `faces` in priority order:
/// each code point comes from the first face that has it, at that face's own scale (Latin: its
/// 'H' at `cap_px`; a face without Latin capitals: the first face's scale by units-per-em).
std::optional<Atlas> BuildOtfAtlas(std::span<const std::span<const std::uint8_t>> faces, std::uint32_t cap_px,
                                   std::span<const std::uint32_t> cps, std::uint32_t page_h);
std::optional<Atlas> BuildOtfAtlas(std::span<const std::uint8_t> otf, std::uint32_t cap_px,
                                   std::span<const std::uint32_t> cps, std::uint32_t page_h);
/// The host's BFFNT font (mod_nx_assets.cpp BuildBffntFont), rebuilt in the module: same glyph
/// boxes, advances and cap height; sheets stacked as pages of their own height.
std::optional<Atlas> BuildBffntAtlas(std::span<const std::uint8_t> bffnt);

/// A face chain's advances as the atlas builder computes them (for measuring before / without an
/// atlas): the first face with the code point, at its own scale.
class Face {
public:
    using Bytes = std::shared_ptr<const std::vector<std::uint8_t>>;
    bool Open(std::vector<std::uint8_t> otf, std::uint32_t cap_px);
    bool Open(std::vector<Bytes> fonts, std::uint32_t cap_px);
    bool Covers(char32_t cp) const;
    int Advance(char32_t cp) const; ///< atlas px at cap_px; -1 when no face has it
    std::uint32_t cap() const { return cap_; }
    bool ok() const { return ok_; }
    /// The chain's font bytes, for BuildOtfAtlas.
    std::vector<std::span<const std::uint8_t>> spans() const;
    ~Face();
    Face() = default;
    Face(const Face&) = delete;
    Face& operator=(const Face&) = delete;

private:
    void Close();
    struct Member {
        Bytes bytes;
        void* info = nullptr; // stbtt_fontinfo
        float scale = 0, cjk_scale = 0;
    };
    std::vector<Member> members_;
    std::uint32_t cap_ = 0;
    bool ok_ = false;
};

enum class Kind { Latin, Kanji, Kana, Korean, Hans, Hant };
inline constexpr int kKinds = 6;
inline Kind KindOf(sw_lang::Lang lang) {
    switch (lang) {
    case sw_lang::Lang::JapaneseKanji: return Kind::Kanji;
    case sw_lang::Lang::Japanese: return Kind::Kana;
    case sw_lang::Lang::Korean: return Kind::Korean;
    case sw_lang::Lang::ChineseSimplified: return Kind::Hans;
    case sw_lang::Lang::ChineseTraditional: return Kind::Hant;
    default: return Kind::Latin;
    }
}
/// strings.json column of a kind's language, for logs ("latin" for the package font).
inline const char* KindCode(Kind kind) {
    switch (kind) {
    case Kind::Kanji: return "ja";
    case Kind::Kana: return "ja-kana";
    case Kind::Korean: return "ko";
    case Kind::Hans: return "zh-Hans";
    case Kind::Hant: return "zh-Hant";
    default: return "latin";
    }
}

/// Every code point of UTF-8 `text` (invalid bytes skipped).
void AddCodepoints(std::string_view text, std::set<std::uint32_t>& out);

/// When noted glyphs (nicknames, OT names beyond the base set) rebuild the module's (CJK) atlas: at most
/// once every 3 s, at most kMaxExtra of them, and never again once a rebuild no longer fits the
/// atlas caps (kMaxPages at kMinCapPx, the byte limit) -- the set only grows, so every later
/// attempt would fail too (review 3 C2). A saturated set keeps its last fitting atlas; new glyphs
/// draw as the package font's missing glyph. A language change starts over.
class GlyphGrowth {
public:
    static constexpr std::size_t kMaxExtra = 512;
    static constexpr std::uint64_t kIntervalMs = 3000;
    void Reset(std::uint64_t now_ms) { *this = {}; next_ms_ = now_ms + kIntervalMs; }
    /// A new glyph was seen: false = do not add it (saturated or at the cap).
    bool Accept() {
        if (saturated_ || extra_ >= kMaxExtra) return false;
        ++extra_;
        pending_ = true;
        return true;
    }
    /// A rebuild is due now (consumes the pending glyphs).
    bool Due(std::uint64_t now_ms) {
        if (!pending_ || saturated_ || now_ms < next_ms_) return false;
        pending_ = false;
        next_ms_ = now_ms + kIntervalMs;
        return true;
    }
    /// The last build of the grown set did (not) fit the caps.
    void Result(bool fits) { saturated_ = saturated_ || !fits; }
    bool saturated() const { return saturated_; }
    std::size_t extra() const { return extra_; }

private:
    std::size_t extra_ = 0;
    bool pending_ = false, saturated_ = false;
    std::uint64_t next_ms_ = 0;
};

class Font {
public:
    ~Font();
    /// Sample thread, every sample: follows the companion language. Reads the font file and the
    /// language's text tables through `host` when a new atlas is due.
    void Update(const EdenDsmodHostApi& host, sw_lang::Lang lang, std::uint64_t now_ms);
    /// Sample thread: text the module publishes (a nickname the base set lacks adds its glyphs).
    void Note(std::string_view text);
    std::int64_t epoch() const { return epoch_.load(); }
    /// True once per finished build that changed what is measured (the reader republishes).
    bool TakeChanged() { return changed_.exchange(false); }
    /// decode_font: the newest finished build becomes the served font.
    bool Decode(void* receiver, EdenDsmodFontSink sink);
    /// load_image for "module:sw:font/..." keys; false for any other key.
    bool LoadImage(const EdenDsmodHostApi& host, std::string_view key, void* receiver, EdenDsmodImageSink sink,
                   bool& handled);

    struct Stats {
        Kind kind = Kind::Latin;
        bool module_font = false;
        std::uint32_t cap_px = 0, pages = 0, width = 0, height = 0, glyphs = 0, missing = 0;
        std::uint64_t build_ms = 0, atlas_bytes = 0;
    };
    Stats stats() const;

private:
    struct Built {
        Kind kind = Kind::Latin;
        std::vector<std::uint32_t> cps;
        Atlas atlas;
        std::uint64_t build_ms = 0;
    };
    void Start(Kind kind, std::vector<std::uint8_t> source, std::vector<std::uint32_t> cps);
    void StartModule(Kind kind, std::vector<std::uint32_t> cps);
    /// The face chain of a module-font kind (opened on first use, kept): nullptr when its font
    /// cannot be read (no Rodin, or the shared font vanished since the probe).
    Face* EnsureFace(const EdenDsmodHostApi& host, Kind kind);
    std::vector<std::uint32_t> BaseCodepoints(const EdenDsmodHostApi& host, sw_lang::Lang lang);

    mutable std::mutex mutex_;
    std::shared_ptr<const Built> prepared_, served_;
    std::shared_ptr<const Atlas> host_latin_; // the BFFNT atlas for the literal "{p}" key
    std::atomic<std::int64_t> epoch_{0};
    std::atomic<bool> changed_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    // sample thread only
    std::optional<Kind> target_;       ///< what the newest started build is (nullopt = the host's BFFNT)
    sw_lang::Lang lang_ = sw_lang::Lang::English;
    bool lang_seen_ = false;
    std::set<std::uint32_t> known_;    ///< the current module-font set
    std::vector<std::uint32_t> extra_; ///< noted code points beyond the base set (kept across rebuilds)
    GlyphGrowth growth_;
    std::set<std::uint32_t> fitting_;  ///< the newest set whose atlas fit the caps (a saturated set returns to it)
    std::size_t fitting_extra_ = 0;    ///< extra_.size() of that set
    std::vector<std::uint32_t> building_; ///< the set of the build in flight (sample thread)
    std::size_t building_extra_ = 0;
    bool awaiting_ = false;            ///< a module-font build was started and its result not taken yet
    std::atomic<int> result_{0};       ///< worker -> sample thread: 1 fits, -1 does not fit the caps
    Face::Bytes rodin_;                ///< the decoded Rodin face (memory only), in every chain
    bool rodin_failed_ = false;
    std::array<bool, kKinds> face_failed_{};
    /// Per kind (Kanji and Kana share the Rodin chain), shared by the builds and the measuring
    /// hook; never closed before the Font is destroyed.
    std::array<std::unique_ptr<Face>, kKinds> faces_;
};

/// sw_font (the module's own text measuring, sw_font_metrics.h) follows the font the companion
/// draws: the module face chain while a module atlas is prepared or served, the BFFNT table otherwise.
void SetMeasuringFace(const Face* face);

} // namespace sw_modfont
