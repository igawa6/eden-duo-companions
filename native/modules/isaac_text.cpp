// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// See isaac_text.h. Owner: ASSETS lane.

#include "isaac_text.h"

#include "isaac_assets_xml.h"

#include <limits>
#include <map>
#include <unordered_map>
#include <vector>

namespace isaac_text {

int StageId(int stage, int type, bool greed) {
    // RoomConfig::GetStageID @Repentance.nro+0x49447C (eLevelStage, eStageType, eMode)
    if (stage < 1 || stage > 13 || type < 0 || type > 5)
        return -1;
    if (greed) {
        static constexpr int GreedTable[3] = {14, 24, 25}; // nro+0x8C1848, stages 5..7
        if (stage >= 5 && stage <= 7)
            return GreedTable[stage - 5];
        if (type == 4)
            return stage * 2 + 25;
        if (type == 5)
            return (stage - 1) * 2 + 28;
        return (stage - 1) * 3 + type + 1;
    }
    if (stage == 13)
        return 35;
    if (stage > 8) {
        if (stage == 12)
            return 26;
        if (stage == 9)
            return type == 4 ? 36 : 13;
        return type + stage * 2 - 6;
    }
    if (type == 5)
        return (stage + 27) & ~1;
    if (type == 4)
        return ((stage - 1) & ~1) + 27;
    return ((stage - 1) >> 1) * 3 + type + 1;
}

std::string_view FloorSuffix(int stage, int curses, bool greed, int challenge) {
    // Level::GetName @+0x3E3CD0: not for greed mode (mode 1) nor challenge 44
    if (greed || challenge == 0x2C)
        return {};
    if (curses & 2) // Curse of the Labyrinth
        return " XL";
    if (stage < 0 || stage > 8)
        return {};
    const unsigned bit = 1u << stage;
    if (bit & 0xAAu)
        return " I";
    if (bit & 0x154u)
        return " II";
    return {};
}

std::string PoolDisplayName(std::string_view pool) {
    // formatting only (isaac_text.h): camelCase / letter-digit split, capitalised words
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::string o;
    o.reserve(pool.size() + 4);
    bool start = true;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const char c = pool[i];
        if (c == '_' || c == ' ') {
            start = true;
            continue;
        }
        if (i > 0 && !start) {
            const char p = pool[i - 1];
            if ((lower(p) && (upper(c) || digit(c))) || (digit(p) && (lower(c) || upper(c))))
                start = true;
        }
        if (start && !o.empty())
            o.push_back(' ');
        o.push_back(start && lower(c) ? static_cast<char>(c - 'a' + 'A') : c);
        start = false;
    }
    return o;
}

namespace {

struct Rec {
    std::string name_key, desc_key, gfx;
    int max_charges{};
};

bool Unescape(std::string_view s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\') {
            if (++i == s.size())
                return false;
            c = s[i];
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
            else if (c != '\\')
                return false;
        } else if (static_cast<unsigned char>(c) < 32 || c == 127) {
            return false;
        }
        out.push_back(c);
    }
    return true;
}

bool AttrIntChecked(const isaac_xml::Node& node, std::string_view key, int& value, int minimum = 0,
                    int maximum = std::numeric_limits<int>::max()) {
    long long parsed;
    if (!isaac_xml::AttrInt(node, key, parsed) || parsed < minimum || parsed > maximum)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

class TextImpl final : public GameText {
public:
    bool Load(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
              const std::atomic<bool>* cancel) override;
    bool Ready() const override {
        return ready_;
    }
    ItemInfo Item(Kind kind, int id) const override;
    std::string StageName(int stage, int stage_type) const override {
        return StageNameMode(stage, stage_type, false);
    }
    std::string Eid(Kind kind, int id) const override {
        if (static_cast<int>(kind) < 0 || static_cast<int>(kind) > 3 || id < 0)
            return {};
        const auto it = eid_.find(Key(kind, id));
        return it == eid_.end() ? std::string{} : it->second;
    }
    std::string FloorName(int stage, int stage_type, int curses, bool greed,
                          int challenge) const override {
        std::string n = StageNameMode(stage, stage_type, greed);
        if (n.empty())
            return n;
        return n + std::string{FloorSuffix(stage, curses, greed, challenge)};
    }
    std::string Text_(std::string_view cat, std::string_view key) const {
        const auto it = strings_.find(std::string{cat} + '\x1f' + std::string{key});
        return it == strings_.end() ? std::string{} : it->second;
    }
    std::string Text(std::string_view category, std::string_view key) const override {
        return Text_(category, key);
    }
    bool HasEid() const override {
        return !eid_.empty();
    }

private:
    static long long Key(Kind k, int id) {
        return (static_cast<long long>(k) << 32) | static_cast<unsigned>(id);
    }
    std::string Resolve(std::string_view cat, std::string_view v) const {
        if (v.empty() || v[0] != '#')
            return std::string{v};
        return Text_(cat, v.substr(1));
    }
    std::string StageNameMode(int stage, int type, bool greed) const {
        const int sid = StageId(stage, type, greed);
        const auto it = stages_.find(sid);
        return it == stages_.end() ? std::string{} : Resolve("Stages", it->second);
    }
    bool LoadData(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
                  const std::atomic<bool>* cancel);
    bool LoadStrings(const EdenDsmodHostApi* host, isaac_romfs::Lang lang, std::string_view file,
                     bool required);
    void LoadEid(const EdenDsmodHostApi* host);

    bool ready_{};
    std::unordered_map<std::string, std::string> strings_; // "cat\x1fkey" -> text
    std::map<int, Rec> coll_, trink_, card_, pill_;
    std::map<int, int> quality_;
    std::map<int, std::string> pools_;
    std::map<int, std::string> stages_; // stages.xml id -> name key
    std::string gfxroot_;
    std::unordered_map<long long, std::string> eid_;
};

bool TextImpl::LoadStrings(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
                           std::string_view file, bool required) {
    std::vector<std::uint8_t> b;
    if (!isaac_romfs::ReadResource(host, file, lang, b))
        return !required;
    isaac_xml::Node root;
    if (!isaac_xml::Parse(b, root) || root.name != "stringtable")
        return false;
    b.clear();
    b.shrink_to_fit();
    // <languages><language id index name>: the string list skips index 0 ("Key")
    int want = static_cast<int>(lang), en = 0;
    for (const auto& c : root.children) {
        if (c->name != "category")
            continue;
        const std::string cat{c->AttrOr("name")};
        for (const auto& k : c->children) {
            if (k->name != "key")
                continue;
            std::vector<const isaac_xml::Node*> s;
            for (const auto& e : k->children)
                if (e->name == "string")
                    s.push_back(e.get());
            std::string v;
            if (want < static_cast<int>(s.size()))
                v = s[want]->text;
            if (v.empty() && en < static_cast<int>(s.size()))
                v = s[en]->text;
            strings_.emplace(cat + '\x1f' + std::string{k->AttrOr("name")}, std::move(v));
        }
    }
    return true;
}

void TextImpl::LoadEid(const EdenDsmodHostApi* host) {
    constexpr std::size_t MaxBytes = 4 << 20;
    constexpr std::size_t MaxEntries = 4096;
    constexpr std::size_t MaxLine = 16384;
    std::vector<std::uint8_t> b;
    const bool abp =
        host && host->get_i64 && host->get_i64(host->userdata, "__source:aoc", -1) == 0;
    const char* path = abp ? "file:eid_abp_en.dat" : "file:eid_en.dat";
    // Reject oversized input before the generic romfs reader allocates it.
    if (!host || !host->read_romfs)
        return;
    const auto size = host->read_romfs(host->userdata, path, 0, nullptr, 0);
    if (!size || size > MaxBytes)
        return;
    b.resize(size);
    if (host->read_romfs(host->userdata, path, 0, b.data(), size) != size)
        return;
    std::string_view s{reinterpret_cast<const char*>(b.data()), b.size()};
    std::size_t nl = s.find('\n');
    if (nl == std::string_view::npos || s.substr(0, nl) != "ISAACEID 1")
        return;
    s.remove_prefix(nl + 1);
    std::unordered_map<long long, std::string> out;
    while (!s.empty()) {
        nl = s.find('\n');
        std::string_view line = s.substr(0, nl);
        s.remove_prefix(nl == std::string_view::npos ? s.size() : nl + 1);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.size() > MaxLine || out.size() >= MaxEntries)
            return;
        const std::size_t t1 = line.find('\t');
        const std::size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos)
            return;
        const std::string_view kind = line.substr(0, t1), id = line.substr(t1 + 1, t2 - t1 - 1);
        if (kind.size() != 1 || kind[0] < '0' || kind[0] > '3' || id.empty() || id.size() > 6)
            return;
        int v = 0;
        for (const char c : id) {
            if (c < '0' || c > '9')
                return;
            v = v * 10 + (c - '0');
        }
        std::string text;
        if (!Unescape(line.substr(t2 + 1), text) ||
            !out.emplace(Key(static_cast<Kind>(kind[0] - '0'), v), std::move(text)).second)
            return;
    }
    eid_ = std::move(out);
}

bool TextImpl::Load(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
                    const std::atomic<bool>* cancel) {
    if (static_cast<int>(lang) < 0 ||
        static_cast<int>(lang) > static_cast<int>(isaac_romfs::Lang::Fr) ||
        (cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    if (ready_)
        return true;
    TextImpl pending;
    if (!pending.LoadData(host, lang, cancel))
        return false;
    *this = std::move(pending);
    return true;
}

bool TextImpl::LoadData(const EdenDsmodHostApi* host, isaac_romfs::Lang lang,
                        const std::atomic<bool>* cancel) {
    const bool abp =
        host && host->get_i64 && host->get_i64(host->userdata, "__source:aoc", -1) == 0;
    if (!LoadStrings(host, lang, "stringtable.sta", !abp) ||
        (cancel && cancel->load(std::memory_order_relaxed)) ||
        !LoadStrings(host, lang, "stringtable_nx.sta", false))
        return false;
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    std::vector<std::uint8_t> b;
    isaac_xml::Node root;
    // items.xml
    if (!isaac_romfs::ReadResource(host, "items.xml", lang, b) || !isaac_xml::Parse(b, root) ||
        root.name != "items")
        return false;
    gfxroot_ = std::string{root.AttrOr("gfxroot")};
    for (const auto& e : root.children) {
        int id;
        if (!AttrIntChecked(*e, "id", id))
            continue;
        int max_charges = 0;
        if (e->Attr("maxcharges") && !AttrIntChecked(*e, "maxcharges", max_charges))
            continue;
        Rec r{std::string{e->AttrOr("name")}, std::string{e->AttrOr("description")},
              std::string{e->AttrOr("gfx")}, max_charges};
        if (e->name == "passive" || e->name == "active" || e->name == "familiar")
            coll_.emplace(static_cast<int>(id), std::move(r));
        else if (e->name == "trinket")
            trink_.emplace(static_cast<int>(id), std::move(r));
    }
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    // pocketitems.xml
    if (!isaac_romfs::ReadResource(host, "pocketitems.xml", lang, b) ||
        !isaac_xml::Parse(b, root) || root.name != "pocketitems")
        return false;
    for (const auto& e : root.children) {
        int id;
        if (!AttrIntChecked(*e, "id", id))
            continue;
        Rec r{std::string{e->AttrOr("name")}, std::string{e->AttrOr("description")}, {}, 0};
        if (e->name == "card" || e->name == "rune")
            card_.emplace(static_cast<int>(id), std::move(r));
        else if (e->name == "pilleffect")
            pill_.emplace(static_cast<int>(id), std::move(r));
    }
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    // items_metadata.xml (optional: quality)
    if (isaac_romfs::ReadResource(host, "items_metadata.xml", lang, b) && isaac_xml::Parse(b, root))
        for (const auto& e : root.children) {
            int id, q;
            if (e->name == "item" && AttrIntChecked(*e, "id", id) &&
                AttrIntChecked(*e, "quality", q, 0, 4))
                quality_[static_cast<int>(id)] = static_cast<int>(q);
        }
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    // itempools.xml (optional)
    if (isaac_romfs::ReadResource(host, "itempools.xml", lang, b) && isaac_xml::Parse(b, root))
        for (const auto& p : root.children) {
            if (p->name != "Pool")
                continue;
            const std::string pool = PoolDisplayName(p->AttrOr("Name"));
            for (const auto& it : p->children) {
                int id;
                if (it->name != "Item" || !AttrIntChecked(*it, "Id", id))
                    continue;
                std::string& s = pools_[static_cast<int>(id)];
                // each pool once, file order
                bool dup = false;
                for (std::size_t at = 0; at <= s.size();) {
                    const std::size_t e = s.find(", ", at);
                    const std::string_view part = std::string_view{s}.substr(
                        at, (e == std::string::npos ? s.size() : e) - at);
                    if (part == pool) {
                        dup = true;
                        break;
                    }
                    if (e == std::string::npos)
                        break;
                    at = e + 2;
                }
                if (!dup)
                    s += (s.empty() ? "" : ", ") + pool;
            }
        }
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    // stages.xml
    if (isaac_romfs::ReadResource(host, "stages.xml", lang, b) && isaac_xml::Parse(b, root))
        for (const auto& e : root.children) {
            int id;
            if (e->name == "stage" && AttrIntChecked(*e, "id", id))
                stages_[static_cast<int>(id)] = std::string{e->AttrOr("name")};
        }
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    LoadEid(host);
    if ((cancel && cancel->load(std::memory_order_relaxed)))
        return false;
    ready_ = true;
    return true;
}

ItemInfo TextImpl::Item(Kind kind, int id) const {
    ItemInfo out;
    const std::map<int, Rec>* table = nullptr;
    const char* cat = "Items";
    switch (kind) {
    case Kind::Collectible:
        table = &coll_;
        break;
    case Kind::Trinket:
        table = &trink_;
        break;
    case Kind::Card:
        table = &card_;
        cat = "PocketItems";
        break;
    case Kind::PillEffect:
        table = &pill_;
        cat = "PocketItems";
        break;
    }
    if (!ready_ || !table)
        return out;
    const auto it = table->find(id);
    if (it == table->end())
        return out;
    const Rec& r = it->second;
    out.name = Resolve(cat, r.name_key);
    out.quote = Resolve(cat, r.desc_key);
    if (!r.gfx.empty()) {
        std::string g = gfxroot_ + (kind == Kind::Trinket ? "trinkets/" : "collectibles/") + r.gfx;
        for (char& c : g)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        out.gfx = std::move(g);
    }
    out.max_charges = r.max_charges;
    if (kind == Kind::Collectible) {
        const auto q = quality_.find(id);
        out.quality = q == quality_.end() ? -1 : q->second;
        const auto p = pools_.find(id);
        if (p != pools_.end())
            out.pools = p->second;
    }
    out.valid = !out.name.empty();
    return out;
}

} // namespace

std::unique_ptr<GameText> CreateGameText() {
    return std::make_unique<TextImpl>();
}

} // namespace isaac_text
