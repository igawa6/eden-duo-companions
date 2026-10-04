// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "acnh_catalog.h"

#include <algorithm>
#include <charconv>

#include "acnh_romfs.h"

namespace acnh {
namespace {
// Enum columns: their key scheme is unknown, so these are the raw keys found in the data; the
// names in comments are leads (Treeki's spec), the joins they take part in are data-verified.
constexpr uint32_t ColItemKind = 0x5270eb75;   // ItemParam "ItemKind"
constexpr uint32_t ColItemMenu = 0x348d7b06;   // ItemParam "ItemMenu"
constexpr uint32_t ColItemUi = 0x28297660;     // ItemParam "ItemUICategory"
constexpr uint32_t ColRecipeType = 0xffe9b35b; // RecipeCraftParam "RecipeDataType" (u32 enum cell)

uint32_t K(std::string_view name, std::string_view type) {
    return Bcsv::Key(name, type);
}

bool ParseId(std::string_view label, uint32_t& kind_crc, int& id, bool& plural) {
    plural = label.ends_with("_pl");
    if (plural)
        label.remove_suffix(3);
    if (label.size() < 7 || label[label.size() - 6] != '_')
        return false;
    const auto digits = label.substr(label.size() - 5);
    if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;
    std::from_chars(digits.data(), digits.data() + digits.size(), id);
    kind_crc = Crc32(label.substr(0, label.size() - 6));
    return true;
}
} // namespace

struct Catalog::Group {
    std::shared_ptr<const Archive> archive;
    std::map<std::string, std::shared_ptr<const Msbt>, std::less<>> parsed;
};

struct Catalog::Names {
    std::unordered_map<uint16_t, std::string> singular, plural; ///< UTF-8, tags removed
    /// STR_ItemName_* strings with their control tags (the grammar composer, acnh_lang_grammar.h)
    std::unordered_map<uint16_t, std::u16string> raw_singular, raw_plural;
};

Catalog::Catalog(Romfs& r) : romfs{r} {}
Catalog::~Catalog() = default;

bool Catalog::Load() {
    std::scoped_lock lock{mutex};
    return LoadLocked();
}

const Bcsv* Catalog::Table(std::string_view name) {
    std::scoped_lock lock{mutex};
    if (const auto it = tables.find(name); it != tables.end())
        return it->second.get();
    std::vector<uint8_t> bytes;
    if (!romfs.Read("Bcsv/" + std::string{name} + ".bcsv", bytes))
        return nullptr;
    auto t = std::make_unique<Bcsv>();
    if (!t->Parse(std::move(bytes)))
        return nullptr;
    const Bcsv* raw = t.get();
    tables.emplace(std::string{name}, std::move(t));
    return raw;
}

bool Catalog::LoadLocked() {
    if (loaded)
        return true;
    const Bcsv* ip = Table("ItemParam");
    const Bcsv* ik = Table("ItemKind");
    const Bcsv* im = Table("ItemMenuIcon");
    // Acquire every required table before publishing immutable vectors. Otherwise a
    // transient missing table would become a permanently empty domain after loaded=true.
    const Bcsv* insect = Table("InsectStatusParam");
    const Bcsv* fish = Table("FishStatusParam");
    const Bcsv* seafood = Table("SeafoodStatusParam");
    const Bcsv* npc = Table("NmlNpcParam");
    const Bcsv* recipe = Table("RecipeCraftParam");
    if (!ip || !ik || !im || !insect || !fish || !seafood || !npc || !recipe)
        return false;
    std::unordered_map<uint32_t, int> stack;
    for (size_t r = 0; r < ik->Rows(); ++r)
        stack[Crc32(ik->Str(r, K("Label", "string32")))] = ik->S(r, K("MultiHoldMaxNum", "s16"));
    std::unordered_map<uint32_t, std::string> menu;
    for (size_t r = 0; r < im->Rows(); ++r)
        menu[Crc32(im->Str(r, K("Label", "string32")))] = im->Str(r, K("ResName", "string32"));
    // ItemParam.ItemSize enum labels and dimensions from the owned 3.0.3 table.
    static constexpr struct {
        const char* label;
        int w2, h2;
    } footprints[] = {
        {"0_5x0_5_Pillar", 1, 1}, {"0_5x1_0_Wall", 1, 2},  {"1_0x0_5", 2, 1},
        {"1_0x0_5_Wall", 2, 1},   {"1_0x1_0", 2, 2},       {"1_0x1_0_Rug", 2, 2},
        {"1_0x1_0_Wall", 2, 2},   {"1_0x1_5_Wall", 2, 3},  {"1_0x2_0_Wall", 2, 4},
        {"1_5x1_5", 3, 3},        {"2_0x0_5", 4, 1},       {"2_0x1_0", 4, 2},
        {"2_0x1_0_Rug", 4, 2},    {"2_0x1_0_Wall", 4, 2},  {"2_0x1_5_Wall", 4, 3},
        {"2_0x2_0", 4, 4},        {"2_0x2_0_Rug", 4, 4},   {"2_0x2_0_Wall", 4, 4},
        {"3_0x1_0", 6, 2},        {"3_0x2_0", 6, 4},       {"3_0x2_0_Rug", 6, 4},
        {"3_0x3_0", 6, 6},        {"3_0x3_0_Rug", 6, 6},   {"4_0x3_0_Rug", 8, 6},
        {"4_0x4_0_Rug", 8, 8},    {"5_0x5_0_Rug", 10, 10},
    };
    std::unordered_map<uint32_t, std::pair<int, int>> dimensions;
    dimensions.reserve(std::size(footprints));
    for (const auto& f : footprints)
        dimensions.emplace(Crc32(f.label), std::pair{f.w2, f.h2});
    items.reserve(ip->Rows());
    item_index.reserve(ip->Rows());
    for (size_t r = 0; r < ip->Rows(); ++r) {
        Item it;
        it.id = static_cast<uint16_t>(ip->U(r, K("UniqueID", "u16")));
        it.kind = ip->U(r, ColItemKind);
        it.menu = ip->U(r, ColItemMenu);
        it.ui = ip->U(r, ColItemUi);
        const uint32_t size = ip->U(r, 0xe06fb090);
        if (const auto f = dimensions.find(size); f != dimensions.end()) {
            it.width2 = f->second.first;
            it.height2 = f->second.second;
        }
        it.remake = static_cast<int16_t>(ip->S(r, K("RemakeID", "s16")));
        it.label = ip->Str(r, K("Label", "string50"));
        it.res_name = ip->Str(r, K("ResName", "string64"));
        if (const auto s = stack.find(it.kind); s != stack.end())
            it.stack_max = s->second;
        if (const auto m = menu.find(it.menu); m != menu.end() && !m->second.empty())
            it.menu_icon = "Layout_MenuIcon_" + m->second;
        item_index[it.id] = items.size();
        items.push_back(std::move(it));
    }
    struct Kind {
        const char* res_col;
        const char* res_type;
        const char* family;
    };
    static constexpr std::array<Kind, 3> kinds{{
        {"ResNameField", "string32", "Layout_BookInsectIcon_"},
        {"ResName", "string64", "Layout_BookFishIcon_"},
        {"ResName", "string64", "Layout_BookDiveFishIcon_"},
    }};
    const std::array<const Bcsv*, 3> status{insect, fish, seafood};
    for (size_t k = 0; k < kinds.size(); ++k) {
        const Bcsv* t = status[k];
        critters[k].reserve(t->Rows());
        for (size_t r = 0; r < t->Rows(); ++r) {
            Critter c;
            c.uid = static_cast<uint16_t>(t->U(r, K("UniqueID", "u16")));
            c.item = static_cast<uint16_t>(t->U(r, K("ItemID", "u16")));
            c.label = t->Str(r, K("Label", "string32"));
            c.res = t->Str(r, K(kinds[k].res_col, kinds[k].res_type));
            if (!c.res.empty())
                c.book_icon = kinds[k].family + c.res;
            critters[k].push_back(std::move(c));
        }
    }
    if (const Bcsv* t = npc)
        for (size_t r = 0; r < t->Rows(); ++r) {
            Villager v;
            v.uid = static_cast<uint16_t>(t->U(r, K("UniqueID", "u16")));
            v.label = t->Str(r, K("Label", "string8"));
            v.birth_month = static_cast<uint8_t>(t->U(r, K("BirthMonth", "u8")));
            v.birth_day = static_cast<uint8_t>(t->U(r, K("BirthMDay", "u8")));
            villagers.push_back(std::move(v));
        }
    if (const Bcsv* t = recipe)
        for (size_t r = 0; r < t->Rows(); ++r) {
            Recipe rc;
            rc.uid = static_cast<uint16_t>(t->U(r, K("UniqueID", "u16")));
            rc.item = static_cast<uint16_t>(t->U(r, K("Item", "u16")));
            rc.serial = static_cast<int16_t>(t->S(r, K("SerialID", "s16")));
            rc.board = t->U(r, K("BoardColorID", "u32"));
            rc.data_type = t->U(r, ColRecipeType);
            rc.remake_art = t->Has(0xad0c787a) && t->U(r, 0xad0c787a) != 0;
            rc.season = t->Has(0x7f5b6179) ? t->U(r, 0x7f5b6179) : 0;
            rc.season_type = t->Has(0x478b74e4) ? t->U(r, 0x478b74e4) : 0;
            rc.event = t->Str(r, K("SelectCalendarEventSeason", "string64"));
            for (int m = 1; m <= 6; ++m) {
                const auto mat =
                    static_cast<uint16_t>(t->U(r, K("Material" + std::to_string(m), "u16")));
                const auto n =
                    static_cast<uint16_t>(t->U(r, K("Amount" + std::to_string(m), "u16")));
                if (mat != 65534)
                    rc.materials.emplace_back(mat, n);
            }
            recipes.push_back(std::move(rc));
        }
    loaded = true;
    return true;
}

const Catalog::Item* Catalog::FindItem(uint16_t id) {
    std::scoped_lock lock{mutex};
    if (!LoadLocked())
        return nullptr;
    const auto it = item_index.find(id);
    return it == item_index.end() ? nullptr : &items[it->second];
}

const std::vector<Catalog::Item>& Catalog::Items() {
    static const std::vector<Item> none;
    std::scoped_lock lock{mutex};
    return LoadLocked() ? items : none;
}

std::vector<std::string> Catalog::ItemIcons(uint16_t id, bool big) {
    std::vector<std::string> out;
    const Item* it = FindItem(id);
    if (!it)
        return out;
    if (big && !it->res_name.empty())
        for (const char* fam :
             {"Layout_FtrIcon_", "Layout_ClosetIcon_", "Layout_DIYRecipeIcon_",
              "Layout_CookingIcon_", "Layout_GardeningIcon_", "Layout_DoorOrnamentIcon_"})
            out.push_back(fam + it->res_name);
    if (!it->menu_icon.empty())
        out.push_back(it->menu_icon);
    return out;
}

const std::vector<Catalog::Critter>& Catalog::Critters(int kind) {
    static const std::vector<Critter> none;
    std::scoped_lock lock{mutex};
    return LoadLocked() ? critters[static_cast<size_t>(std::clamp(kind, 0, 2))] : none;
}

const Catalog::Critter* Catalog::FindCritter(int kind, uint16_t uid) {
    if (kind < 0 || kind > 2)
        return nullptr;
    for (const auto& c : Critters(kind))
        if (c.uid == uid)
            return &c;
    return nullptr;
}

const std::vector<Catalog::Villager>& Catalog::Villagers() {
    static const std::vector<Villager> none;
    std::scoped_lock lock{mutex};
    return LoadLocked() ? villagers : none;
}

const Catalog::Villager* Catalog::FindVillager(std::string_view label) {
    for (const auto& v : Villagers())
        if (v.label == label)
            return &v;
    return nullptr;
}

namespace {
// Minimal BYML (v2-v7, little endian) navigation: dictionaries by key string, arrays, strings.
struct Byml {
    const std::vector<uint8_t>& b;
    std::vector<std::string> keys, strs;
    explicit Byml(const std::vector<uint8_t>& bytes) : b{bytes} {
        if (b.size() < 16 || b[0] != 'Y' || b[1] != 'B')
            return;
        Strings(U32(4), keys);
        Strings(U32(8), strs);
    }
    uint32_t U32(size_t o) const {
        if (o + 4 > b.size())
            return 0;
        return uint32_t{b[o]} | uint32_t{b[o + 1]} << 8 | uint32_t{b[o + 2]} << 16 |
               uint32_t{b[o + 3]} << 24;
    }
    uint32_t U24(size_t o) const {
        return o + 3 <= b.size()
                   ? (uint32_t{b[o]} | uint32_t{b[o + 1]} << 8 | uint32_t{b[o + 2]} << 16)
                   : 0;
    }
    int Type(size_t o) const {
        return o < b.size() ? b[o] : -1;
    }
    void Strings(uint32_t o, std::vector<std::string>& out) const {
        if (!o || Type(o) != 0xC2)
            return;
        const uint32_t n = U24(o + 1);
        if (uint64_t{o} + 4 + (uint64_t{n} + 1) * 4 > b.size())
            return;
        for (uint32_t i = 0; i < n; ++i) {
            const size_t a = size_t{o} + U32(size_t{o} + 4 + i * 4);
            size_t e = a;
            while (e < b.size() && b[e])
                ++e;
            out.emplace_back(a < b.size()
                                 ? std::string(reinterpret_cast<const char*>(b.data() + a), e - a)
                                 : "");
        }
    }
    uint32_t Root() const {
        return U32(0xC);
    }
    /// dictionary node `d`: (type, value word offset) of `key`, or (-1, 0)
    std::pair<int, size_t> Get(uint32_t d, std::string_view key) const {
        if (Type(d) != 0xC1)
            return {-1, 0};
        const uint32_t n = U24(d + 1);
        if (uint64_t{d} + 4 + uint64_t{n} * 8 > b.size())
            return {-1, 0};
        for (uint32_t i = 0; i < n; ++i) {
            const size_t e = d + 4 + i * 8;
            const uint32_t k = U24(e);
            if (k < keys.size() && keys[k] == key)
                return {Type(e + 3), e + 4};
        }
        return {-1, 0};
    }
};
} // namespace

std::vector<int> Catalog::WeatherHours(int pattern) {
    std::scoped_lock lock{mutex};
    std::vector<uint8_t> pack, file;
    // tried once the pack is read (an unreadable romfs is asked again next time)
    if (!weather_tried && romfs.Read("Pack/StaticParam.pack", pack)) {
        weather_tried = true;
        if (Romfs::SarcFind(pack, "Param/Weather/WeatherPattern.byml", file)) {
            // WeatherType enum names as the game's string table (0x482fec8) spells them
            static const char* const Types[] = {"\xE5\xBF\xAB\xE6\x99\xB4",
                                                "\xE6\x99\xB4\xE3\x82\x8C",
                                                "\xE6\x9B\x87\xE3\x82\x8A",
                                                "\xE9\x9B\xA8\xE9\x9B\xB2",
                                                "\xE9\x9B\xA8",
                                                "\xE5\xA4\xA7\xE9\x9B\xA8",
                                                "\xE9\x9B\xAA",
                                                "\xE5\xA4\xA7\xE9\x9B\xAA"};
            const Byml y{file};
            // root {"b6717c2f" (WeatherPattern): {"4ead583d" (mParams): {<object>: {"20e90aef":
            // label, "c1ac9e9e": [24 x WeatherType name]}}}} (key strings as stored in the file)
            const auto [t1, v1] = y.Get(y.Root(), "b6717c2f");
            const auto [t2, v2] =
                t1 == 0xC1 ? y.Get(y.U32(v1), "4ead583d") : std::pair<int, size_t>{-1, 0};
            if (t2 == 0xC1) {
                const uint32_t objs = y.U32(v2);
                for (uint32_t i = 0, n = y.U24(objs + 1); i < n && i < 256; ++i) {
                    const size_t e = objs + 4 + i * 8;
                    if (y.Type(e + 3) != 0xC1)
                        continue;
                    const uint32_t o = y.U32(e + 4);
                    const auto [tl, vl] = y.Get(o, "20e90aef");
                    const auto [ta, va] = y.Get(o, "c1ac9e9e");
                    if (tl != 0xA0 || ta != 0xC0 || y.U32(vl) >= y.strs.size())
                        continue;
                    const uint32_t arr = y.U32(va), cnt = y.U24(arr + 1);
                    if (y.Type(arr) != 0xC0 || cnt != 24 ||
                        uint64_t{arr} + 28 + 24 * 4 > file.size())
                        continue;
                    const size_t vals = arr + 4 + ((cnt + 3) & ~3u);
                    std::vector<int> hours;
                    for (uint32_t h = 0; h < 24; ++h) {
                        int v = -1;
                        const uint32_t si = y.U32(vals + h * 4);
                        if (y.Type(arr + 4 + h) == 0xA0 && si < y.strs.size())
                            for (int k = 0; k < 8; ++k)
                                if (y.strs[si] == Types[k])
                                    v = k;
                        hours.push_back(v);
                    }
                    weather[y.strs[y.U32(vl)]] = std::move(hours);
                }
            }
        }
    }
    const Bcsv* t = Table("WeatherPatternParam");
    if (!t)
        return {};
    for (size_t r = 0; r < t->Rows(); ++r)
        if (static_cast<int>(t->U(r, K("UniqueID", "u16"))) == pattern) {
            const auto it = weather.find(t->Str(r, K("Label", "string32")));
            return it == weather.end() ? std::vector<int>{} : it->second;
        }
    return {};
}

const std::vector<Catalog::Recipe>& Catalog::Recipes() {
    static const std::vector<Recipe> none;
    std::scoped_lock lock{mutex};
    return LoadLocked() ? recipes : none;
}

std::shared_ptr<Catalog::Group> Catalog::GroupFor(Lang lang, std::string_view group) {
    std::string key = std::string{group} + "_" + std::string{LangFolder(lang)};
    {
        std::scoped_lock lock{mutex};
        if (const auto it = groups.find(key); it != groups.end())
            return it->second;
    }
    // decompressed outside the lock (MBs: the timing thread's lookups must not wait for it)
    auto arc = romfs.SarcFile("Message/" + key + ".sarc.zs");
    if (!arc)
        return nullptr;
    std::scoped_lock lock{mutex};
    auto& g = groups[key];
    if (!g) {
        g = std::make_shared<Group>();
        g->archive = std::move(arc);
    }
    return g;
}

std::shared_ptr<const Msbt> Catalog::Message(Lang lang, std::string_view path) {
    const auto slash = path.find('/');
    if (slash == std::string_view::npos)
        return nullptr;
    const auto g = GroupFor(lang, path.substr(0, slash));
    if (!g)
        return nullptr;
    const std::string file = std::string{path.substr(slash + 1)} + ".msbt";
    std::scoped_lock lock{mutex};
    if (const auto it = g->parsed.find(file); it != g->parsed.end())
        return it->second;
    std::shared_ptr<Msbt> m;
    if (const auto mem = g->archive->members.find(file); mem != g->archive->members.end()) {
        m = std::make_shared<Msbt>();
        if (!m->Parse(mem->second))
            m.reset();
    }
    g->parsed[file] = m;
    return m;
}

std::vector<std::string> Catalog::MessagePaths(Lang lang, std::string_view group) {
    std::vector<std::string> out;
    const auto g = GroupFor(lang, group);
    if (!g)
        return out;
    for (const auto& [name, bytes] : g->archive->members)
        if (name.ends_with(".msbt"))
            out.push_back(std::string{group} + "/" + name.substr(0, name.size() - 5));
    return out;
}

std::string Catalog::Text(Lang lang, std::string_view path, std::string_view label) {
    const auto m = Message(lang, path);
    return m ? m->Text(label) : std::string{};
}

std::shared_ptr<const Catalog::Names> Catalog::NamesFor(Lang lang) {
    {
        std::scoped_lock lock{mutex};
        if (const auto it = names.find(static_cast<int>(lang)); it != names.end())
            return it->second;
    }
    auto n = std::make_shared<Names>();
    std::map<std::pair<std::string, int>, std::string> group_name; // (kind suffix, group) -> name
    std::unordered_map<uint16_t, std::pair<std::string, int>> outfit;
    const auto paths = MessagePaths(lang, "String");
    if (paths.empty())
        return n; // romfs not readable (yet): empty, and not remembered
    for (const auto& path : paths) {
        const auto m = Message(lang, path);
        if (!m)
            continue;
        if (path.starts_with("String/Item/STR_ItemName_")) {
            for (const auto& [label, raw] : m->All()) {
                uint32_t kind;
                int id;
                bool plural;
                if (!ParseId(label, kind, id, plural) || id > 0xFFFF)
                    continue;
                (plural ? n->plural : n->singular)[static_cast<uint16_t>(id)] = Msbt::ToUtf8(raw);
                (plural ? n->raw_plural : n->raw_singular)[static_cast<uint16_t>(id)] = raw;
            }
        } else if (path.starts_with("String/Outfit/GroupName/")) {
            const std::string fam = path.substr(path.rfind('_') + 1);
            for (const auto& [label, raw] : m->All()) {
                int g = 0;
                std::from_chars(label.data(), label.data() + label.size(), g);
                group_name[{fam, g}] = Msbt::ToUtf8(raw);
            }
        } else if (path.starts_with("String/Outfit/GroupColor/")) {
            const std::string fam = path.substr(path.rfind('_') + 1);
            for (const auto& [label, raw] : m->All()) {
                // "<group>_<ItemKind>_<id:05d>"
                const auto u = label.find('_');
                if (u == std::string::npos || label.size() < 7)
                    continue;
                int g = 0, id = 0;
                std::from_chars(label.data(), label.data() + u, g);
                std::from_chars(label.data() + label.size() - 5, label.data() + label.size(), id);
                outfit[static_cast<uint16_t>(id)] = {fam, g};
            }
        }
    }
    for (const auto& [id, fg] : outfit)
        if (!n->singular.contains(id))
            if (const auto it = group_name.find(fg); it != group_name.end())
                n->singular[id] = it->second;
    std::scoped_lock lock{mutex};
    names[static_cast<int>(lang)] = n;
    return n;
}

std::string Catalog::ItemName(Lang lang, uint16_t id, bool plural) {
    const auto n = NamesFor(lang);
    const auto& map = plural ? n->plural : n->singular;
    const auto it = map.find(id);
    return it == map.end() ? std::string{} : it->second;
}

std::string Catalog::PocketItemName(Lang lang, uint16_t id, uint32_t contents, bool plural) {
    const auto* item = FindItem(id);
    if (item && item->label == "DIYRecipe") {
        // Native recipe construction at 0x1a059d4 stores the recipe UID at Item+4.
        // LIVE: pocket card 5794 / UID 471 is named "Woodland wall", recipe item 5223.
        for (const auto& recipe : Recipes()) {
            if (recipe.uid == static_cast<uint16_t>(contents)) {
                const auto name = ItemName(lang, recipe.item);
                if (!name.empty())
                    return name;
                break;
            }
        }
        return Text(lang, "String/STR_Common", "956");
    }
    auto name = plural ? ItemName(lang, id, true) : std::string{};
    return name.empty() ? ItemName(lang, id) : name;
}

std::u16string Catalog::ItemNameRaw(Lang lang, uint16_t id, bool plural) {
    const auto n = NamesFor(lang);
    const auto& map = plural ? n->raw_plural : n->raw_singular;
    const auto it = map.find(id);
    return it == map.end() ? std::u16string{} : it->second;
}

std::string Catalog::VillagerName(Lang lang, std::string_view label) {
    return Text(lang, "String/Npc/STR_NNpcName", label);
}

} // namespace acnh
