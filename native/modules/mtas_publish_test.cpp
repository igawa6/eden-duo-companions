// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

// My Time at Sandrock publish invariants that need no running game: full-list output and
// selection, calendar decode, text tables and the art decode guards.
#include "mtas_art.h"
#include "mtas_reader.h"
#include "mtas_text.h"

#include "0100B63016916000.cpp"

#include <map>
#include <chrono>
#include <ctime>
#include <set>
#include <fstream>
#include <cstdlib>

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint64_t TicksPerDay = 24ULL * 60 * 60 * 1000 * 10000;

struct Published {
    std::map<std::string, std::int64_t> ints;
    std::map<std::string, double> floats;
    std::map<std::string, std::string> texts;
    std::map<std::string, std::int64_t> flags;
    const char* page{"today"};
    const char* orientation{"landscape"};
    EdenDsmodHostApi host{};
    std::uint64_t calls{};

    Published() {
        host.userdata = this;
        host.get_i64 = [](void* p, const char* key, std::int64_t fallback) {
            const auto& flags = static_cast<Published*>(p)->flags;
            const auto it = flags.find(key);
            return it == flags.end() ? fallback : it->second;
        };
        host.get_text = [](void* p, const char* key) -> const char* {
            const auto& out = *static_cast<Published*>(p);
            if (std::strcmp(key, "__page") == 0) return out.page;
            if (std::strcmp(key, "@screen.orientation") == 0) return out.orientation;
            return nullptr;
        };
        host.publish_i64 = [](void* p, const char* key, std::int64_t value) {
            ++static_cast<Published*>(p)->calls;
            static_cast<Published*>(p)->ints[key] = value;
        };
        host.publish_f64 = [](void* p, const char* key, double value) { ++static_cast<Published*>(p)->calls; static_cast<Published*>(p)->floats[key] = value; };
        host.publish_text = [](void* p, const char* key, const char* value) {
            ++static_cast<Published*>(p)->calls;
            static_cast<Published*>(p)->texts[key] = value;
        };
    }

    void CheckList(const char* count, const char* prefix, std::size_t size) const {
        assert(ints.at(count) == static_cast<std::int64_t>(size));
        for (std::size_t i = 0; i < size; ++i) {
            const auto key = std::string(prefix) + std::to_string(i) + ".";
            assert(ints.at(key + "v") == 1);
            assert(!texts.at(key + (std::strcmp(prefix, "mission") == 0 ? "title" : "name")).empty());
        }
        assert(ints.count(std::string(prefix) + std::to_string(size) + ".v") == 0);
    }
};

void TestLanguageMapping() {
    const char* expected[] = {"chinese", "english", "german", "french", "traditional_chinese",
        "italian", "spanish", "japanese", "russain", "english", "korean", "thailand", "portuguese"};
    for (int id = 0; id < 13; ++id)
        assert(std::strcmp(mtas::LanguageBundle(id), expected[id]) == 0);
    for (const int id : {-100, -1, 13, 100})
        assert(std::strcmp(mtas::LanguageBundle(id), "english") == 0);
    assert(mtas::Utf16ToUtf8(u"Latin é / 中文 / ミアン / 한 / ไทย / \U0001F600") ==
           "Latin é / 中文 / ミアン / 한 / ไทย / \U0001F600");
    assert(mtas::Utf16ToUtf8(std::u16string(1, 0xD800)) == "\xEF\xBF\xBD");
    assert(mtas::Utf16ToUtf8(std::u16string(1, 0xDC00)) == "\xEF\xBF\xBD");
    std::puts("TestLanguageMapping ok (13 enum IDs and invalid-ID English fallback)");
}

void TestRomfsLanguages() {
    const char* root = std::getenv("MTAS_ROMFS");
    if (!root)
        return;
    struct Files {
        std::string root;
        bool refuse_french{false};
        std::size_t reads{};
    } files{root};
    EdenDsmodHostApi host{};
    host.userdata = &files;
    host.read_romfs = [](void* p, const char* path, std::uint64_t offset, void* out,
                         std::size_t size) -> std::size_t {
        auto& f = *static_cast<Files*>(p);
        ++f.reads;
        if (f.refuse_french && std::string_view{path}.ends_with("localization/french"))
            return 0;
        std::ifstream in(f.root + "/" + path, std::ios::binary);
        if (!out && size == 0) {
            in.seekg(0, std::ios::end);
            return in ? static_cast<std::size_t>(in.tellg()) : 0;
        }
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(in.gcount());
    };
    mtas::Text text;
    assert(text.Load(host, 1));
    const auto english = text.ItemName(19800019);
    assert(!english.empty());
    assert(!text.SocialLevelTitle(0, 2).empty());
    assert(text.SocialLevelTitle(99, 99).empty());
    const auto reads = files.reads;
    assert(text.Load(host, 1) && files.reads == reads);
    mtas::Art art(ce_unity::MakeHostRangeReader(&host));
    const auto weekday = art.LabelImage("Wednesday", 130, 40, 18);
    assert(weekday && weekday->width == 130 && weekday->height == 40);
    const auto version = art.VersionLabel();
    assert(version && version->width == 100 && version->height == 32);
    bool version_ink = false;
    for (std::size_t i = 3; i < version->rgba.size(); i += 4) {
        if (!version->rgba[i]) continue;
        version_ink = true;
        const auto pixel = i / 4;
        assert(pixel % 100 > 0 && pixel % 100 < 99 && pixel / 100 > 0 && pixel / 100 < 31);
        assert(version->rgba[i - 3] == 0xFF || version->rgba[i - 3] == 0x1B);
    }
    assert(version_ink);
    assert(art.FitLabel("Fang", 114, 20) == "Fang");
    assert(art.FitLabel("Wednesday", 138, 18) == "Wednesday");
    assert(art.FitLabel("Wednesday", 80, 20).ends_with("..."));
    std::uint32_t baseline_first{}, baseline_line{};
    std::vector<EdenDsmodFontGlyph> baseline;
    assert(art.FontGlyphs(baseline_first, baseline, baseline_line));
    const auto mi_an_id = text.NpcIdByName("Mi-an");
    assert(mi_an_id > 0 && text.NpcPortrait(mi_an_id) == "npc_8017");
    for (int id = 0; id < 13; ++id) {
        assert(text.Load(host, id));
        assert(text.NpcPortrait(mi_an_id) == "npc_8017");
        const auto portrait = mtas::RenderPortrait(text.NpcPortrait(mi_an_id), 308, 112);
        assert(portrait.src == "module:mtas:fit:npc_8017:308:112");
        if (id == 7) assert(text.NpcName(mi_an_id) != "Mi-an");
        assert(!text.ItemName(19800019).empty());
        assert(!text.Get(30017001).empty());
        for (int label : mtas::UiGameIds) {
            const auto value = mtas::ResolveUi(text, std::to_string(label));
            if (value.empty()) std::fprintf(stderr, "missing UI message %d in %s\n",
                                           label, mtas::LanguageBundle(id));
            assert(!value.empty());
        }
        for (const auto& label : mtas::UiOwnLabels)
            assert(!mtas::ResolveUi(text, label.key).empty());
        assert(text.CurrentLanguage() == (id == 9 ? 1 : id));
        auto font_text = text.FontText();
        std::vector<std::string> published;
        for (int label : mtas::UiGameIds)
            published.push_back(text.Get(label));
        for (const auto& row : mtas::UiOwnLabels) {
            published.push_back(mtas::ResolveUi(text, row.key));
            font_text += published.back();
        }
        std::set<std::uint32_t> old_pages;
        for (int label : mtas::UiGameIds)
            for (auto cp : mtas::FontCodepoints(text.Get(label)))
                if (cp >= baseline_first && cp - baseline_first < baseline.size()) {
                    const auto& g = baseline[cp - baseline_first];
                    if (g.w && g.h) old_pages.insert(g.y / mtas::FontPageHeight);
                }
        std::printf("old label layout %s: %zu pages\n", mtas::LanguageBundle(id), old_pages.size());
        published.push_back(font_text); // Also cover late mission/event strings from the bundle.
        art.SetFontText(font_text);
        std::uint32_t first{}, line{};
        std::vector<EdenDsmodFontGlyph> glyphs;
        assert(art.FontGlyphs(first, glyphs, line));
        std::uint32_t max_page = 0;
        for (const auto& g : glyphs)
            if (g.w && g.h) {
                assert(g.y % mtas::FontPageHeight + g.h <= mtas::FontPageHeight);
                max_page = std::max(max_page, g.y / mtas::FontPageHeight);
            }
        assert(max_page < 4096);
        std::set<std::uint32_t> pages;
        for (auto cp : mtas::FontCodepoints(font_text)) {
            if (cp < first || cp - first >= glyphs.size())
                continue;
            const auto& g = glyphs[cp - first];
            if (g.w && g.h)
                pages.insert(g.y / mtas::FontPageHeight);
        }
        std::map<std::uint32_t, ce_unity::Image> images;
        std::size_t bytes = 0;
        for (const auto page : pages) {
            assert(page < 4096);
            const auto image = art.FontAtlasImage(page);
            assert(image);
            bytes += image->rgba.size();
            images.emplace(page, *image);
        }
        assert(bytes <= (32u << 20));
        for (const auto& value : published)
            for (auto cp : mtas::FontCodepoints(value)) {
                // Spaces, formatting controls and inline-object markers have no text ink.
                if (cp == 0x20 || cp == 0xA0 || cp == 0xAD || cp == 0x3000 || cp == 0xFEFF || cp == 0xFFFC || (cp >= 0x7F && cp <= 0x9F) ||
                    (cp >= 0x2000 && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202F) ||
                    (cp >= 0x2060 && cp <= 0x206F) || (cp >= 0xFE00 && cp <= 0xFE0F))
                    continue;
                if (cp < first || cp - first >= glyphs.size() || !glyphs[cp - first].w) {
                    std::fprintf(stderr, "missing U+%04X in %s: %s\n", cp,
                                 mtas::LanguageBundle(id), value.substr(0, 160).c_str());
                    assert(false);
                }
                const auto& g = glyphs[cp - first];
                assert(pages.contains(g.y / mtas::FontPageHeight));
                assert(g.y % mtas::FontPageHeight + g.h <= mtas::FontPageHeight);
                const auto& image = images.at(g.y / mtas::FontPageHeight);
                bool ink = false;
                for (std::uint32_t y = 0; y < g.h && !ink; ++y)
                    for (std::uint32_t x = 0; x < g.w && !ink; ++x)
                        ink = image.rgba[((g.y % mtas::FontPageHeight + y) * image.width + g.x + x) * 4 + 3] != 0;
                assert(ink);
            }
        std::printf("font %s: cap=%u, %u virtual pages, %zu resident pages, %zu bytes\n",
                    mtas::LanguageBundle(id), line, max_page + 1, pages.size(), bytes);

        assert(mtas::GameText().Load(host, id));
        mtas::Module module;
        module.samples = 1;
        module.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        Published output;
        mtas::SampleCallback(&module, &output.host);
        for (int label : mtas::UiGameIds)
            assert(output.texts.at("ui." + std::to_string(label)) == text.Get(label));
        for (const auto& label : mtas::UiOwnLabels)
            assert(output.texts.at(std::string("ui.") + label.key) ==
                   mtas::ResolveUi(text, label.key));
        assert(mtas::WeekdayText(51) == text.Get(1271)); // fixture day is Wednesday
        for (std::uint64_t day = 0; day < 7; ++day)
            assert(mtas::WeekdayText(day) == text.Get(1269 + day));
        mtas::TimeFields calendar;
        calendar.year = 1; calendar.month = 4; calendar.day = 18; calendar.day_of_world = 52;
        assert(mtas::DaySeasonText(calendar) == mtas::UiFormat(text.Get(1121), {"1", text.Get(1287), "18"}));
        std::printf("calendar %s: %s / %s\n", mtas::LanguageBundle(id),
                    mtas::DaySeasonText(calendar).c_str(), mtas::WeekdayText(52).c_str());
        mtas::TimeLeft deadline;
        deadline.has_deadline = true; deadline.days = 3; deadline.hours = 2;
        assert(mtas::LeftText(deadline, false).starts_with("3"));
        assert(mtas::LeftText(deadline).ends_with(mtas::LeftText(deadline, false)));
        deadline.overdue = true;
        assert(mtas::LeftText(deadline) == mtas::ResolveUi(text, "overdue"));

        const auto date = mtas::UiFormat(text.Get(1121), {"1", text.Get(1287), "17"});
        assert(date.find("{0}") == std::string::npos && date.find("17") != std::string::npos);
        assert(mtas::UiFormat("{0} / {1}", {"{1}", "2"}) == "{1} / 2");
        if (id == 3 || id == 7)
            assert(text.ItemName(19800019) != english);
        std::printf("language %d (%s): %s / %s\n", id, mtas::LanguageBundle(id),
                    text.ItemName(19800019).c_str(), text.Get(30017001).c_str());
    }
    assert(mtas::GameText().Load(host, 1));
    files.refuse_french = true;
    assert(text.Load(host, 3));
    assert(text.CurrentLanguage() == 1);
    assert(text.ItemName(19800019) == english); // never retain the previous language on failure
    // Bug regression, right after the fallback above (before anything else touches `text`'s
    // language state): a failed bundle load must not permanently stick the language at English.
    // With the bundle reachable again, the SAME originally-requested language must load for real
    // on the very next Load() call, not short-circuit on the previously-failed request.
    files.refuse_french = false;
    assert(text.Load(host, 3));
    assert(text.CurrentLanguage() == 3);
    assert(text.ItemName(19800019) != english);
    assert(text.Load(host, 1) && text.ItemName(19800019) == english);
    assert(mtas::GameText().Load(host, 1));
    mtas::RecipeFields crowded;
    for (const char* name : {"Very long rice", "Very long potato", "Very long egg", "Very long flour"}) {
        mtas::MaterialNeed mat;
        mat.item_name = name;
        crowded.materials.push_back(mat);
    }
    const auto needs = mtas::FitRecipeNeeds(crowded, art);
    const auto plus = needs.find_last_of('+');
    assert(plus != std::string::npos);
    const auto hidden = std::stoi(needs.substr(plus + 1));
    assert(hidden + 1 + std::count(needs.begin(), needs.begin() + plus, ',') == 4);
    assert(art.FitLabel(needs, 424, 25) == needs);
    std::puts("TestRomfsLanguages ok (all UI labels in 12 bundles, switches, no-op, missing-bundle fallback, retry after failure)");
}

void TestMissionObjectives() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.missions.ok = true;
    auto& rows = m.model.missions.running;
    rows.resize(4);
    rows[0].title = "Untimed";
    rows[0].objective = "Synthetic story objective";
    rows[1].title = "Timed item";
    rows[1].objective = "Synthetic item objective";
    rows[1].left.has_deadline = true;
    rows[1].left.days = 2;
    rows[1].item_ok = true;
    rows[1].item_name = "Synthetic item";
    rows[1].item_need = 6;
    rows[2].title = "Unreadable objective";
    rows[3].title = "Another story";
    rows[3].objective = "Synthetic second objective";
    Published output;
    const auto sample = [&] {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        mtas::SampleCallback(&m, &output.host);
    };
    sample();
    // Objective text follows the sorted mission, with item and countdown fields retained.
    assert(output.texts.at("mission0.title") == "Timed item");
    assert(output.ints.at("mission0.objective_ok") == 1);
    assert(output.texts.at("mission0.objective") == rows[1].objective);
    assert(output.ints.at("mission0.item_ok") == 1);
    assert(output.texts.at("mission0.item_text") == "Synthetic item  (?/6)");
    assert(output.texts.at("mission0.left_text") == mtas::UiFormat(mtas::Ui("2044"), {"2", "0"}));
    assert(output.ints.at("mission1.objective_ok") == 1);
    assert(output.texts.at("mission1.objective") == rows[0].objective);
    assert(output.ints.at("mission1.has_deadline") == 0);
    assert(output.ints.at("mission2.objective_ok") == 0);
    assert(output.texts.count("mission2.objective") == 0);
    assert(output.texts.at("mission3.objective") == rows[3].objective);
    // A missing objective must hide a previously published value on the next sample.
    rows[0].objective.clear();
    sample();
    assert(output.ints.at("mission1.objective_ok") == 0);
    rows[0].objective = "Synthetic replacement objective";
    sample();
    assert(output.ints.at("mission1.objective_ok") == 1);
    assert(output.texts.at("mission1.objective") == rows[0].objective);
    // Quality text fails closed: a gated order without the game's grade-name ids keeps the plain
    // two-line item row; base grade, monster missions and no-gate orders never get a quality row.
    rows[1].item_grade = 2;
    sample();
    assert(output.ints.at("mission0.has_quality") == 0 && output.ints.at("mission0.objective_nq") == 1);
    assert(output.ints.at("mission0.objective_q") == 0);
    assert(output.texts.at("mission0.quality_text").empty());
    m.model.grade_names_ok = true;
    m.model.grade_name_ids = {1187, 1188, 1189, 1190};
    for (const auto [grade, group] : {std::pair{-1, 0}, std::pair{0, 0}, std::pair{2, 9}}) {
        assert(mtas::QualityText(m.model, grade, group).empty());
    }
    assert(output.ints.at("mission1.has_quality") == 0 && output.ints.at("mission1.objective_q") == 0);
    std::puts("TestMissionObjectives ok (sorted text, positive gate, missing/replaced, item/countdown retained)");
}

void TestInventoryCategories() {
    // Every tag bit independently, from each mask. No mutually exclusive category assumption.
    const std::map<int, int> tag_filters{{0, 6}, {1, 5}, {2, 2}, {3, 3}, {4, 4}};
    for (const auto& [tag, filter] : tag_filters) {
        for (bool instance : {false, true}) {
            mtas::InvItem item;
            item.category_ok = true;
            (instance ? item.instance_tags : item.prototype_tags) = std::uint32_t{1} << tag;
            for (int candidate : {0, 1, 2, 3, 4, 5, 6, 9, 17, 19})
                assert(mtas::MatchesInventoryCategory(item, candidate) ==
                       (candidate == 0 || candidate == filter));
        }
    }
    mtas::InvItem item;
    item.category_ok = true;
    item.prototype_tags = (1 << 2) | (1 << 4);
    item.instance_tags = 1 << 3;
    assert(mtas::MatchesInventoryCategory(item, 2));
    assert(mtas::MatchesInventoryCategory(item, 3));
    assert(mtas::MatchesInventoryCategory(item, 4));
    for (int collection : {0, 1, 2, 3}) {
        item.collection = collection;
        assert(mtas::MatchesInventoryCategory(item, 1) == (collection == 1));
        assert(mtas::MatchesInventoryCategory(item, 9) == (collection == 2));
    }
    item.category_ok = false;
    assert(mtas::MatchesInventoryCategory(item, 0));
    assert(!mtas::MatchesInventoryCategory(item, 2));

    mtas::Module m;
    m.model.game_loaded = m.model.inventory.ok = m.model.inventory.storage_ok = true;
    for (int i = 0; i < 4; ++i) {
        item = {};
        item.name = "Stack " + std::to_string(i);
        item.category_ok = true;
        item.prototype_tags = 1 << (i % 2 ? 3 : 2);
        item.in_storage = i >= 2;
        m.model.inventory.items.push_back(item);
    }
    auto sample = [&] {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        Published output;
        mtas::SampleCallback(&m, &output.host);
        return output;
    };
    for (int filter : {0, 1, 2, 3, 4, 5, 6, 9})
        assert(mtas::ActionCallback(&m, "set_category", filter));
    for (int filter : {-1, 7, 8, 10, 17, 19, 256})
        assert(!mtas::ActionCallback(&m, "set_category", filter));
    m.item_sel = 2;
    assert(mtas::ActionCallback(&m, "set_category", 2));
    auto output = sample();
    assert(output.ints.at("inv.item_count") == 2);
    assert(output.texts.at("inv.filtered_text") == "2");
    assert(output.ints.at("item_detail.ok") == 0);
    assert(output.ints.at("inv.category2.selected") == 1);
    assert(output.ints.at("inv.category3.idle") == 1);
    for (int source : {1, 2}) {
        assert(mtas::ActionCallback(&m, "set_source", source));
        output = sample();
        assert(output.ints.at("inv.item_count") == 1);
        assert(output.texts.at("item0.name") == (source == 1 ? "Stack 0" : "Stack 2"));
    }
    assert(mtas::ActionCallback(&m, "set_category", 1));
    assert(sample().ints.at("inv.item_count") == 0);
    m.model.inventory.items.back().category_ok = false;
    output = sample();
    assert(output.ints.at("inv.categories_ok") == 0);
    assert(output.ints.at("inv.item_count") == 2); // unreadable metadata falls back to All
    std::puts("TestInventoryCategories ok (all filters, both masks, collection, source intersection, actions)");
}

void TestPortraitImageSources() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.social.ok = true;
    mtas::NpcFields npc;
    npc.npc_id = 1;
    npc.portrait = "TestPortrait";
    m.model.social.npcs.push_back(npc);
    Published out;
    auto sample = [&] { m.samples = 1; mtas::SampleCallback(&m, &out.host); };
    sample();
    const auto landscape_plan = out.texts.at("plan0.portrait_src");
    const auto landscape_dialogue = out.texts.at("npc_detail.dialogue_portrait_src");
    assert(landscape_plan.ends_with(":148:142"));
    out.orientation = "portrait";
    sample();
    assert(out.texts.at("plan0.portrait_src").ends_with(":184:176"));
    assert(out.texts.at("npc_detail.dialogue_portrait_src").ends_with(":984:670"));
    out.orientation = "landscape";
    sample();
    assert(out.texts.at("plan0.portrait_src") == landscape_plan);
    assert(out.texts.at("npc_detail.dialogue_portrait_src") == landscape_dialogue);
    std::puts("TestPortraitImageSources ok (live orientation switch, model and selection retained)");
}

void TestDayEventNames() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.time.ok = true;
    m.model.time.calendar_ok = true;
    m.model.time.day_of_world = 100;
    m.model.week.ok = true;
    using Kind = mtas::DayEvent::Kind;
    m.model.week.events = {
        {Kind::Birthday, 1, "Test NPC", "portrait"},
        {Kind::Festival, 1, "Second event", ""},
        {Kind::Festival, 2, "Localized festival title", ""},
        {Kind::Reminder, 3, "Player reminder", ""},
        {Kind::Reminder, 4, "", ""},
        {Kind::Birthday, 4, "Later named event", ""},
        {Kind::Festival, 6, "Long localized festival title that needs ellipsis", ""},
        {Kind::Event, 7, "Outside strip", ""},
    };
    Published output;
    auto sample = [&] {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        mtas::SampleCallback(&m, &output.host);
    };
    sample();
    assert(output.texts.at("day1.event_name") == "Test NPC");
    assert(output.ints.at("calendar8.all") == 1 && output.ints.at("calendar7.all") == 0);
    assert(output.ints.at("calendar8.day1.birthday") == 1);
    assert(output.ints.at("calendar7.day1.birthday") == 0);
    assert(output.texts.at("festival.event_name") == "Second event");
    assert(output.texts.at("day1.event_icon_src") == "module:mtas:fit:portrait:66:80");
    assert(output.texts.at("day2.event_name") == "Localized festival title");
    assert(output.texts.at("day3.event_name") == "Player reminder");
    assert(output.texts.at("day6.event_name") == m.model.week.events[6].label);
    for (int day = 0; day < 7; ++day) {
        const auto p = "day" + std::to_string(day) + ".";
        assert(output.ints.at(p + "has_event") == (day != 0 && day != 5));
        assert(output.ints.at(p + "has_event_name") ==
               (day == 1 || day == 2 || day == 3 || day == 6));
    }
    assert(output.texts.at("day4.event_name").empty());
    // "+N more": day 1 and day 4 carry two revealed events each, the rest one or none.
    for (int day = 0; day < 7; ++day) {
        const auto p = "day" + std::to_string(day) + ".";
        assert(output.ints.at(p + "has_more") == (day == 1 || day == 4));
        assert(output.texts.at(p + "more_text") == (day == 1 || day == 4 ? "+1" : ""));
    }
    m.model.week.events.push_back({Kind::Reminder, 1, "Third", ""});
    sample();
    assert(output.texts.at("day1.more_text") == "+2" && output.texts.at("day1.event_name") == "Test NPC");
    m.model.week.events.pop_back();
    // Reuse the publisher to prove missing data clears flags and stale names.
    m.model.week.ok = false;
    sample();
    assert(output.ints.at("calendar7.all") == 1 && output.ints.at("calendar8.all") == 0);
    assert(output.ints.at("festival.has_event") == 0);
    for (int day = 0; day < 7; ++day) {
        const auto p = "day" + std::to_string(day) + ".";
        assert(output.ints.at(p + "has_event") == 0);
        assert(output.ints.at(p + "has_event_name") == 0);
        assert(output.texts.at(p + "event_name").empty());
        assert(output.ints.at(p + "has_more") == 0 && output.texts.at(p + "more_text").empty());
    }
    m.model.week.ok = true;
    m.model.week.events.clear();
    sample();
    assert(output.ints.at("day1.has_event_name") == 0);
    std::puts("TestDayEventNames ok (first event wins, all kinds, empty names, stale data cleared)");
}

void TestWorkshopRankBadges() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.workshop.ok = true;
    auto& rows = m.model.workshop.ranking;
    rows.resize(6);
    // Move the player through every position: badges depend only on row index.
    for (std::size_t player = 0; player < rows.size(); ++player) {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i].is_player = i == player;
        Published output;
        mtas::SampleCallback(&m, &output.host);
        assert(output.ints.at("ws.rank_count") == 6);
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const auto p = "rank" + std::to_string(i) + ".";
            assert(output.texts.at(p + "pos_text") == std::to_string(i + 1));
            assert(output.ints.at(p + "badge") == (i < 3));
            assert(output.ints.at(p + "plain") == (i >= 3));
            assert(output.texts.at(p + "badge_src") ==
                   (i < 3 ? "module:mtas:ui:WsRankMargin" + std::to_string(i) + ":40:56" : ""));
        }
    }
    // Workshop level stars are independent of the player's ranking-list position.
    for (int level : {0, 1, 3, 5, 9}) {
        m.samples = 1;
        m.model.workshop.rank_display = level;
        m.rendered_ready = false;
        Published output;
        mtas::SampleCallback(&m, &output.host);
        assert(output.ints.at("ws.rank_ok") == (level >= 1 && level <= 5));
        assert(output.texts.at("ws.rank_text") == std::to_string(level));
        assert(output.texts.at("ws.rank_icon_src") == "module:mtas:ui:I_Playerinfo_icon_Star_0" + std::to_string(std::clamp(level, 1, 5)) + ":80:80");
        for (int i = 0; i < 5; ++i)
            assert(output.texts.at("ws.star" + std::to_string(i) + "_src") ==
                   (i < std::clamp(level, 0, 5) ? "module:mtas:rank_star:filled" : "module:mtas:rank_star:empty"));
    }
    std::puts("TestWorkshopRankBadges ok (three position badges, later rows plain, player independent)");
}

void TestHandbookFresh() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.handbook.ok = true;
    m.model.handbook.slots.resize(13);
    Published output;
    for (const bool fresh : {true, false, true}) {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        m.model.handbook.slots[0].fresh = fresh;
        m.model.handbook.slots[12].fresh = fresh;
        mtas::SampleCallback(&m, &output.host);
        assert(output.ints.at("bp0.fresh") == fresh);
        assert(output.ints.at("bp1.fresh") == 0);
        assert(output.ints.at("bp12.fresh") == fresh);
    }
    std::puts("TestHandbookFresh ok (fresh/seen/fresh, including scrolled cards)");
}

void TestMaterialVisibility() {
    mtas::Module m;
    m.samples = 1;
    m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    m.model.game_loaded = true;
    m.model.ingredients.ok = m.model.handbook.ok = true;
    auto& recipes = m.model.ingredients.known;
    recipes.resize(1);
    // CookingFormula has up to five materials, including active recipes in this build.
    recipes[0].materials.resize(5);
    for (auto& mat : recipes[0].materials) {
        mat.need = mat.have = 1;
        mat.can_afford = true;
    }
    recipes[0].materials.back().have = 0;
    recipes[0].materials.back().can_afford = false;
    recipes[0].can_cook_now = false;

    auto& blueprints = m.model.handbook.slots;
    blueprints.resize(1);
    // The source MachineAtlas table reaches nine materials. Exercise every shortage
    // position, not just the first hidden row, as well as no-overflow and empty cards.
    for (std::size_t count = 0; count <= 9; ++count) {
        blueprints[0].materials.resize(count);
        for (std::size_t missing = 0; missing <= count; ++missing) {
            for (std::size_t j = 0; j < count; ++j) {
                auto& mat = blueprints[0].materials[j];
                mat.need = 1;
                mat.have = j == missing ? 0 : 1;
                mat.can_afford = mat.have >= mat.need;
            }
            Published output;
            mtas::SampleCallback(&m, &output.host);
            assert(output.ints.at("recipe0.mat4.ok") == 1);
            assert(output.ints.at("recipe0.mat4.need_ok_red") == 1);
            assert(output.ints.at("recipe0.mat4.need_ok_green") == 0);
            assert(output.texts.at("recipe0.mat4.have_text") == "0");
            assert(output.ints.at("recipe0.can_cook") == 0);
            assert(output.texts.at("ing.can_cook_text") == mtas::Ui("ready") + " 0 / 1");
            const bool hidden_missing = missing >= 2 && missing < count;
            assert(output.ints.at("bp0.overflow_red") == hidden_missing);
            assert(output.ints.at("bp0.overflow_green") == (count > 2 && !hidden_missing));
            if (count > 2)
                assert(output.texts.at("bp0.overflow_text") == "+" + std::to_string(count - 2) + " " + mtas::Ui("more"));
            else
                assert(output.texts.count("bp0.overflow_text") == 0);
            for (std::size_t j = 0; j < std::min(count, std::size_t{2}); ++j)
                assert(output.ints.at("bp0.mat" + std::to_string(j) + ".need_ok_red") ==
                       (j == missing));
            m.samples = 1;
            m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        }
    }
    recipes[0].materials.back().have = 1;
    recipes[0].materials.back().can_afford = true;
    recipes[0].can_cook_now = true;
    Published affordable;
    mtas::SampleCallback(&m, &affordable.host);
    assert(affordable.ints.at("recipe0.mat4.need_ok_green") == 1);
    assert(affordable.ints.at("recipe0.mat4.need_ok_red") == 0);
    assert(affordable.ints.at("recipe0.can_cook") == 1);
    assert(affordable.texts.at("ing.can_cook_text") == mtas::Ui("ready") + " 1 / 1");
    std::puts("TestMaterialVisibility ok (5 cooking materials; 0-9 blueprint materials, every shortage position)");
}

void TestFullListPublishing() {
    mtas::Module m;
    // Keep the synthetic model through the next sample instead of invoking the live reader.
    m.samples = 1;
    m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    auto& model = m.model;
    model.game_loaded = true;
    model.missions.ok = model.social.ok = model.inventory.ok = true;
    model.ingredients.ok = model.handbook.ok = true;
    // Exercise each reader's current bound, all above the former mockup-sized limits.
    model.missions.running.resize(64);
    model.social.npcs.resize(256);
    model.inventory.items.resize(256);
    model.ingredients.known.resize(512);
    model.handbook.slots.resize(512);
    for (std::size_t i = 0; i < model.social.npcs.size(); ++i) {
        model.social.npcs[i].npc_id = static_cast<std::int32_t>(1000 + i);
        model.social.npcs[i].name = "Person " + std::to_string(i);
    }
    for (std::size_t i = 0; i < model.missions.running.size(); ++i)
        model.missions.running[i].title = "Mission " + std::to_string(i);
    for (std::size_t i = 0; i < model.inventory.items.size(); ++i) {
        model.inventory.items[i].name = "Item " + std::to_string(i);
        model.inventory.items[i].icon = "test-icon";
    }
    for (std::size_t i = 0; i < model.ingredients.known.size(); ++i)
        model.ingredients.known[i].dish_name = "Recipe " + std::to_string(i);
    for (std::size_t i = 0; i < model.handbook.slots.size(); ++i)
        model.handbook.slots[i].item_name = "Blueprint " + std::to_string(i);
    m.item_sel = 255;
    model.dialogue.ok = true;
    model.dialogue.npc_id = model.social.npcs.back().npc_id;
    Published out;
    mtas::SampleCallback(&m, &out.host);
    out.CheckList("missions.count", "mission", 64);
    out.CheckList("social.npc_count", "npc", 256);
    out.CheckList("inv.item_count", "item", 256);
    out.CheckList("ing.count", "recipe", 512);
    out.CheckList("hb.count", "bp", 512);
    assert(out.texts.at("missions.count_text") == "64");
    assert(out.ints.at("item_detail.ok") == 1);
    assert(out.texts.at("item_detail.name") == "Item 255");
    assert(out.texts.at("mission63.title") == "Mission 63");
    assert(out.texts.at("recipe511.name") == "Recipe 511");
    assert(out.texts.at("bp511.name") == "Blueprint 511");
    assert(out.texts.at("item255.icon_src") == "module:mtas:ui:test-icon:64:64");
    assert(out.ints.at("dlg.active") == 1);
    assert(out.texts.at("npc_detail.name") == "Person 255");

    // Filter counts, selections, empty lists and unknown dialogue speakers stay consistent.
    m.source = 1;
    model.inventory.items.back().in_storage = true;
    model.social.npcs.clear();
    model.missions.running.clear();
    model.ingredients.known.clear();
    model.handbook.slots.clear();
    Published filtered;
    mtas::SampleCallback(&m, &filtered.host);
    filtered.CheckList("inv.item_count", "item", 255);
    filtered.CheckList("social.npc_count", "npc", 0);
    filtered.CheckList("missions.count", "mission", 0);
    filtered.CheckList("ing.count", "recipe", 0);
    filtered.CheckList("hb.count", "bp", 0);
    assert(filtered.ints.at("item_detail.ok") == 0);
    assert(filtered.ints.at("dlg.active") == 0);
    std::puts("TestFullListPublishing ok (64 missions, 256 NPCs/items, 512 recipes/blueprints)");
}

/// PERF measurement (review-L2 finding 2): same synthetic late-size model as
/// TestFullListPublishing (64 missions, 256 NPCs/items, 512 recipes/blueprints, every
/// ingredient/blueprint material slot filled so PublishMaterial's own cost is exercised too),
/// counting calls into the per-entry string-formatting primitives (mtas::FormatCallCount(), see
/// the module's own top-of-file counter) across three samples: the first ("cold") actually
/// rebuilds Module::rendered; the second ("warm", same model, same selection) must cost
/// (near-)zero formats since the model never refreshed; a third ("selected") shows the two
/// cheap, deliberately-uncached per-tick exceptions (the one selected npc/item's differently-
/// sized detail portrait/icon, see RenderSocialNpc/RenderInventoryItem's own comments).
void TestFormatCallsCached() {
    mtas::Module m;
    auto& model = m.model;
    model.game_loaded = true;
    model.missions.ok = model.social.ok = model.inventory.ok = true;
    model.ingredients.ok = model.handbook.ok = true;
    model.missions.running.resize(64);
    model.social.npcs.resize(256);
    model.inventory.items.resize(256);
    model.ingredients.known.resize(512);
    model.handbook.slots.resize(512);
    for (std::size_t i = 0; i < model.social.npcs.size(); ++i) {
        model.social.npcs[i].npc_id = static_cast<std::int32_t>(1000 + i);
        model.social.npcs[i].name = "Person " + std::to_string(i);
        model.social.npcs[i].portrait = "p";
    }
    for (auto& it : model.inventory.items) {
        it.name = "Item";
        it.icon = "i";
    }
    for (auto& r : model.ingredients.known) {
        r.dish_name = "Recipe";
        r.dish_icon = "d";
        r.materials.resize(5);
        for (auto& mat : r.materials) {
            mat.need = mat.have = 1;
            mat.can_afford = true;
            mat.item_icon = "m";
        }
        r.can_cook_now = true;
    }
    for (auto& b : model.handbook.slots) {
        b.unlocked = true;
        b.item_name = "Blueprint";
        b.item_icon = "b";
        b.materials.resize(2);
        for (auto& mat : b.materials) {
            mat.need = mat.have = 1;
            mat.can_afford = true;
            mat.item_icon = "m";
        }
    }
    m.npc_sel = -1;
    m.item_sel = -1;
    Published out;

    mtas::ResetFormatCallCount();
    m.samples = 1; // bypass the real reader; rendered_ready starts false (cold start)
    mtas::SampleCallback(&m, &out.host);
    const auto cold = mtas::FormatCallCount();

    mtas::ResetFormatCallCount();
    m.samples = 1; // unchanged model AND rendered_ready now true -- no rebuild should happen
    mtas::SampleCallback(&m, &out.host);
    const auto warm = mtas::FormatCallCount();

    mtas::ResetFormatCallCount();
    m.npc_sel = 255;
    m.item_sel = 255;
    m.samples = 1;
    mtas::SampleCallback(&m, &out.host);
    const auto selected = mtas::FormatCallCount();

    std::printf("format calls -- cold=%llu warm=%llu selected=%llu (64 missions, 256 NPCs/"
               "items, 512 recipes x5 mats/blueprints x2 mats)\n",
               static_cast<unsigned long long>(cold), static_cast<unsigned long long>(warm),
               static_cast<unsigned long long>(selected));
    // Cold must actually do the work (thousands of entries/materials formatted once); warm must
    // be (near-)zero -- the whole point of the fix -- its small constant is the two things that
    // legitimately can't be cached: npc_detail's own always-fresh, differently-sized portrait
    // render (selected or not, same as the pre-fix code's unconditional PublishPortrait call) and
    // inv.filtered_text (Grouped(filtered.size()), tick-level since the filter/selection can
    // change any tick). "selected" adds exactly one more: item_detail's own always-fresh icon,
    // only rendered once a selection exists.
    assert(cold > 5000);
    assert(warm == 2);
    assert(selected == warm + 1);
    if (std::getenv("MTAS_BENCH")) {
        constexpr int ticks = 600;
        out.calls = 0;
        const auto cpu_start = std::clock();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ticks; ++i) {
            // Replay the late-size model, including the normal 10 Hz formatting invalidation.
            m.samples = 1;
            if (i % 6 == 0) m.rendered_ready = false;
            mtas::SampleCallback(&m, &out.host);
        }
        const auto us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        const double cpu_us = (std::clock() - cpu_start) * 1e6 / CLOCKS_PER_SEC;
        std::printf("BENCH late-size replay: ticks=%d publish/s@60Hz=%.0f cpu_us/tick=%.2f elapsed_us/tick=%.2f\n",
                    ticks, out.calls * 60.0 / ticks, cpu_us / ticks, us / ticks);
    }
    std::puts("TestFormatCallsCached ok (cache rebuilds once per refresh, not once per tick)");
}

void TestDialogueReturnPage() {
    for (std::size_t i = 0; i < std::size(mtas::DialoguePages); ++i) {
        mtas::Module m;
        m.model.game_loaded = true;
        Published out;
        m.model.social.npcs.resize(2);
        m.model.social.npcs[0].npc_id = 1000;
        m.model.social.npcs[1].npc_id = 1001;
        m.npc_sel = 1;
        m.item_sel = 7;
        m.source = 2;
        const auto sample = [&] {
            m.samples = 1; // Keep the synthetic model, even across repeated dialogues.
            m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
            mtas::SampleCallback(&m, &out.host);
            assert(m.npc_sel == 1 && m.item_sel == 7 && m.source == 2);
        };
        sample();
        assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == -1);
        for (int repeat = 0; repeat < 2; ++repeat) {
            out.page = mtas::DialoguePages[i];
            m.model.dialogue.ok = true;
            m.model.dialogue.npc_id = 1000;
            sample();
            assert(out.ints.at("dlg.active") == 1 && out.ints.at("dlg.return_page") == -1);
            // The automatic switch and a speaker change must not overwrite the saved page.
            out.page = "npc";
            m.model.dialogue.npc_id = 1001;
            sample();
            assert(m.dialogue_return_page == static_cast<std::int64_t>(i));
            m.model.dialogue.ok = false;
            sample();
            assert(out.ints.at("dlg.active") == 0);
            assert(out.ints.at("dlg.return_page") == static_cast<std::int64_t>(i));
            sample();
            assert(out.ints.at("dlg.return_page") == static_cast<std::int64_t>(i));
        }
    }
    // Missing/unknown page queries and unknown speakers must never navigate to a guessed page.
    for (const char* page : {static_cast<const char*>(nullptr), "unknown"}) {
        mtas::Module m;
        m.model.game_loaded = true;
        Published out;
        out.page = page;
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        m.model.social.npcs.resize(1);
        m.model.social.npcs[0].npc_id = 1000;
        m.model.dialogue.ok = true;
        m.model.dialogue.npc_id = 1000;
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == -1);
        out.host.get_text = nullptr;
        m.dialogue_active = false;
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == -1);
    }
    std::puts("TestDialogueReturnPage ok (9 pages, repeated dialogues, selections/filter preserved)");
}

void TestMissionContext() {
    for (std::size_t i = 0; i < std::size(mtas::DialoguePages); ++i) {
        mtas::Module m;
        Published out;
        m.model.game_loaded = true;
        m.model.ui.ok = true;
        m.item_sel = 7;
        m.source = 2;
        const auto sample = [&] {
            m.samples = 1;
            m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
            mtas::SampleCallback(&m, &out.host);
            assert(m.item_sel == 7 && m.source == 2);
        };
        sample();
        assert(out.ints.at("context.mission") == 0);
        for (const int state : {10, 10, 28, 28}) {
            out.page = mtas::DialoguePages[i];
            m.model.ui.state_type = state;
            sample();
            assert(out.ints.at("context.mission") == 1);
            assert(out.ints.at("dlg.return_page") == -1);
            out.page = "mission";
            m.model.ui.ok = false;
            sample();
            assert(out.ints.at("context.mission") == 1);
            assert(m.dialogue_return_page == static_cast<std::int64_t>(i));
            m.model.ui.ok = true;
            m.model.ui.state_type = 0;
            sample();
            assert(out.ints.at("context.mission") == 0);
            assert(out.ints.at("dlg.return_page") == static_cast<std::int64_t>(i));
        }
        // A direct board/dialogue transition keeps the original page in either direction.
        out.page = mtas::DialoguePages[i];
        m.model.social.npcs.resize(1);
        m.model.social.npcs[0].npc_id = 1000;
        m.model.dialogue.ok = true;
        m.model.dialogue.npc_id = 1000;
        m.model.ui.state_type = 16;
        sample();
        out.page = "npc";
        m.model.dialogue.ok = false;
        m.model.ui.state_type = 10;
        sample();
        assert(out.ints.at("dlg.active") == 0 && out.ints.at("context.mission") == 1);
        assert(out.ints.at("dlg.return_page") == -1);
        assert(m.dialogue_return_page == static_cast<std::int64_t>(i));
        out.page = "mission";
        m.model.dialogue.ok = true;
        m.model.ui.state_type = 16;
        sample();
        assert(out.ints.at("dlg.active") == 1 && out.ints.at("context.mission") == 0);
        assert(out.ints.at("dlg.return_page") == -1);
        m.model.dialogue.ok = false;
        m.model.ui.state_type = 0;
        sample();
        assert(out.ints.at("dlg.return_page") == static_cast<std::int64_t>(i));
    }
    for (const char* page : {static_cast<const char*>(nullptr), "unknown"}) {
        mtas::Module m;
        Published out;
        out.page = page;
        m.model.game_loaded = true;
        m.model.ui = {true, 10};
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("context.mission") == 0);
        assert(out.ints.at("dlg.return_page") == -1);
        out.host.get_text = nullptr;
        m.mission_context_active = false;
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("context.mission") == 0);
    }
    mtas::Module m;
    Published out;
    m.model.ui = {true, 10};
    m.samples = 1;
    m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    mtas::SampleCallback(&m, &out.host);
    assert(out.ints.at("context.mission") == 0); // No navigation before a save is loaded.
    m.model.game_loaded = true;
    for (const int state : {-1, 0, 1, 2, 6, 16, 17, 37}) {
        m.model.ui.state_type = state;
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("context.mission") == 0);
    }
    std::puts("TestMissionContext ok (9 pages, repeated board/shop visits, unreadable UI, dialogue transitions)");
}

void TestSleepContext() {
    for (std::size_t i = 0; i < std::size(mtas::DialoguePages); ++i) {
        mtas::Module m;
        Published out;
        m.model.game_loaded = true;
        m.model.ui = {true, 0};
        const auto sample = [&] {
            m.samples = 1;
            m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
            mtas::SampleCallback(&m, &out.host);
        };
        sample();
        assert(out.ints.at("context.today") == 0);
        for (int repeat = 0; repeat < 2; ++repeat) {
            out.page = mtas::DialoguePages[i];
            m.model.ui.state_type = 37;
            m.model.time.ok = true;
            m.model.time.calendar_ok = true;
            m.model.time.day = 2 + repeat;
            m.model.time.hour = 7;
            sample();
            assert(out.ints.at("context.today") == 1);
            assert(out.ints.at("context.mission") == 0);
            assert(out.ints.at("dlg.return_page") == -1);
            assert(out.ints.at("time.day") == 2 + repeat);
            assert(out.texts.at("time.clock_text") == "07:00");
            out.page = "today";
            m.model.ui.ok = false;
            sample();
            assert(out.ints.at("context.today") == 1);
            m.model.ui = {true, 0};
            sample();
            assert(out.ints.at("context.today") == 0);
            assert(out.ints.at("dlg.return_page") == static_cast<std::int64_t>(i));
        }
    }
    std::puts("TestSleepContext ok (9 pages, repeated sleeps, refreshed day/clock, prior-page return)");
}

void TestSettings() {
    Published out;
    auto settings = mtas::ReadSettings(out.host);
    assert(!settings.clock_top && !settings.tracker_top && settings.dialogue_card);
    out.flags = {{"@flag:mtas.clock_top", 1}, {"@flag:mtas.tracker_top", 1},
                 {"@flag:mtas.dialogue_card", 0}};
    settings = mtas::ReadSettings(out.host);
    assert(settings.clock_top && settings.tracker_top && !settings.dialogue_card);
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.social.npcs.resize(1);
    m.model.social.npcs[0].npc_id = 1000;
    m.model.dialogue.ok = true;
    m.model.dialogue.npc_id = 1000;
    const auto sample = [&] {
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        mtas::TickCallback(&m, &out.host);
        mtas::SampleCallback(&m, &out.host);
    };
    out.page = "settings";
    sample();
    assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == -1);
    out.flags["@flag:mtas.dialogue_card"] = 1;
    sample();
    assert(out.ints.at("dlg.active") == 1 && m.dialogue_return_page == 8);
    out.page = "npc";
    out.flags["@flag:mtas.dialogue_card"] = 0;
    sample();
    assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == 8);
    sample();
    assert(out.ints.at("dlg.active") == 0);
    // A fresh module reads the restored preference from the host, without session state.
    mtas::Module restarted;
    restarted.model = m.model;
    restarted.samples = 1;
    restarted.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    mtas::TickCallback(&restarted, &out.host);
    mtas::SampleCallback(&restarted, &out.host);
    assert(out.ints.at("dlg.active") == 0 && out.ints.at("dlg.return_page") == -1);
    out.host.get_i64 = nullptr;
    assert(mtas::ReadSettings(out.host).dialogue_card);
    std::puts("TestSettings ok (defaults, restored flags, dialogue off/on/off, restart)");
}

void TestHudSettingsLease() {
    struct Mailbox {
        std::map<std::uint32_t, std::uint32_t> words;
    } mailbox;
    Published out;
    auto* m = new mtas::Module;
    m->helper.userdata = &mailbox;
    m->helper.store_u32 = [](void* p, std::uint32_t offset, std::uint32_t value) -> EdenDsmodBool {
        static_cast<Mailbox*>(p)->words[offset] = value;
        return EDEN_DSMOD_TRUE;
    };
    out.flags["__aux_present"] = 1;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 0 && mailbox.words[8] == 0);
    m->model.game_loaded = true;
    m->model.ui.ok = true;
    m->model.ui.state_type = 0;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 3 && mailbox.words[8] == 120);
    out.flags["@flag:mtas.clock_top"] = 1;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 2);
    out.flags["@flag:mtas.tracker_top"] = 1;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 0 && mailbox.words[8] == 0);
    out.flags["@flag:mtas.clock_top"] = 0;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 1);
    out.flags["__aux_present"] = 0;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 0 && mailbox.words[8] == 0);
    out.flags["__aux_present"] = 1;
    m->model.ui.state_type = 16;
    mtas::TickCallback(m, &out.host);
    assert(mailbox.words[12] == 0);
    m->model.ui.state_type = 0;
    mtas::TickCallback(m, &out.host);
    mtas::Destroy(m);
    assert(mailbox.words[12] == 0 && mailbox.words[8] == 0);
    std::puts("TestHudSettingsLease ok (independent masks, menus, hidden, destroy restore)");
}

void TestCalendar() {
    // LIVE-READS.md: day_of_world=100 decoded to Day 17/Month 4 (Winter)/Year 1/Wednesday,
    // matching the native HUD "4.17 Wednesday" exactly.
    {
        const auto c = mtas::CalendarFromDateData(100ULL * TicksPerDay);
        assert(c.year == 1);
        assert(c.month == 4);
        assert(std::strcmp(c.season, "Winter") == 0);
        assert(std::strcmp(c.weekday, "Wednesday") == 0);
    }
    // day_of_world=101, the next in-game day: Thursday (the +1 DayOfWeek offset was found and
    // confirmed from exactly this pair of live reads a day apart).
    {
        const auto c = mtas::CalendarFromDateData(101ULL * TicksPerDay);
        assert(std::strcmp(c.weekday, "Thursday") == 0);
    }
    // Year rollover: day_of_world=112 is the first day of Year 2, Month 1 (Spring), day 1.
    {
        const auto c = mtas::CalendarFromDateData(112ULL * TicksPerDay);
        assert(c.year == 2);
        assert(c.month == 1);
        assert(std::strcmp(c.season, "Spring") == 0);
    }
    // The top 2 bits are DateTime.Kind and must not leak into the tick math.
    {
        const auto plain = mtas::CalendarFromDateData(100ULL * TicksPerDay);
        const auto with_kind = mtas::CalendarFromDateData((100ULL * TicksPerDay) | (1ULL << 62));
        assert(plain.year == with_kind.year && plain.month == with_kind.month);
        assert(std::strcmp(plain.season, with_kind.season) == 0);
        assert(std::strcmp(plain.weekday, with_kind.weekday) == 0);
    }
    std::puts("TestCalendar ok");
}

ce_unity::Image Solid(std::uint32_t w, std::uint32_t h, std::uint8_t v) {
    ce_unity::Image img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<std::size_t>(w) * h * 4, v);
    return img;
}

void TestNineSlice() {
    const auto& pins = mtas::UiPins();
    const auto& paper = pins.at("Paper");
    const auto& coin = pins.at("Coin");

    // The real Paper pin's border (35/35/36/35 on a 72x72 source) stretched up to a wide note.
    const auto src = Solid(72, 72, 200);
    const auto out = mtas::NineSlice(src, paper, 1180, 300);
    assert(out.has_value());
    assert(out->width == 1180 && out->height == 300);
    assert(out->rgba.size() == std::size_t{1180} * 300 * 4);

    // A border too large for the output is refused, never silently clamped into garbage pixels.
    assert(!mtas::NineSlice(src, paper, 60, 60).has_value());

    // The coin pin has no border: a plain scale, any reasonable target size.
    const auto coin_src = Solid(48, 48, 90);
    const auto coin_out = mtas::NineSlice(coin_src, coin, 64, 64);
    assert(coin_out.has_value());
    assert(coin_out->width == 64 && coin_out->height == 64);
    // Uniform source -> uniform output (no stray zeroed pixels from an off-by-one row/column).
    for (std::size_t i = 0; i < coin_out->rgba.size(); i += 4)
        assert(coin_out->rgba[i] == 90);

    // A degenerate (0x0) request is refused, not a crash or an empty-but-"ok" image.
    assert(!mtas::NineSlice(src, paper, 0, 0).has_value());
    mtas::SpritePin capsule{};
    capsule.border_l = 23; capsule.border_r = 20.5f;
    capsule.border_b = 12.5f; capsule.border_t = 13;
    ce_unity::Image capsule_src{52, 26, std::vector<std::uint8_t>(52 * 26 * 4, 255)};
    const auto pill = mtas::NineSlice(capsule_src, capsule, 112, 112);
    assert(pill && pill->rgba[(56 * 112 + 56) * 4 + 3] == 255);
    std::puts("TestNineSlice ok (including native half-pixel capsule borders)");
}

void TestUiPins() {
    // Every pin the generated table claims to have must resolve to a sane sprite rect, a
    // non-empty bundle/node, a plausible ASTC block size and an atlas big enough to hold the
    // sprite's own rect -- a cheap sanity pass over the whole generated table, not a live decode.
    const auto& pins = mtas::UiPins();
    assert(pins.size() >= 40); // Stage-9: the real B layout's sprite set
    for (const auto& [name, p] : pins) {
        assert(p.bundle != nullptr && p.bundle[0] != '\0');
        assert(p.data_node != nullptr && p.data_node[0] != '\0');
        assert(p.block == 6 || p.block == 8);
        assert(p.gobs_per_block > 0);
        assert(p.atlas_w > 0 && p.atlas_h > 0);
        assert(p.rect_w > 0 && p.rect_h > 0);
        assert(p.rect_x >= 0 && p.rect_y >= 0);
        assert(p.rect_x + p.rect_w <= static_cast<float>(p.atlas_w) + 1.0f);
        assert(p.rect_y + p.rect_h <= static_cast<float>(p.atlas_h) + 1.0f);
    }
    // A handful of specific names the Today/Mission pages bind to must be present.
    for (const char* n : {"Paper", "Coin", "PageMain", "KuangNote01", "NoteMain", "ClockIcon",
                          "SeasonWinter", "TabCalendarNor", "TabCalendarSel", "TabBtnBGNor",
                          "TabBtnBGSel", "WorkshopIcon", "ScrollTrack", "ScrollThumb", "ScrollCap",
                          "MiniInfoBG", "OrderBGOther00", "OrderBGSide01", "OrderBGMain04",
                          "OrderBGSel", "OrderNeedItemBG", "OrderItemLimit", "MissionAim",
                          "MissionIconMain", "MissionIconSide", "MissionIconOther", "MissionGou",
                          "MissionCricle", "HbNewBadge"})
        assert(pins.count(n) == 1);
    std::puts("TestUiPins ok");
}

void TestSpriteIndex() {
    // Round-2: the dynamic item-icon/NPC-portrait table, same sanity shape as TestUiPins above
    // (a cheap bounds pass over the whole generated table, no live decode).
    const auto& idx = mtas::SpriteIndex();
    assert(idx.size() >= 3000); // 3,037 item icons + 58 NPC portraits resolved this pass
    for (const auto& [name, p] : idx) {
        assert(p.bundle != nullptr && p.bundle[0] != '\0');
        assert(p.data_node != nullptr && p.data_node[0] != '\0');
        assert(p.block == 6 || p.block == 8);
        assert(p.gobs_per_block > 0);
        assert(p.atlas_w > 0 && p.atlas_h > 0);
        assert(p.rect_w > 0 && p.rect_h > 0);
        assert(p.rect_x >= 0 && p.rect_y >= 0);
        assert(p.rect_x + p.rect_w <= static_cast<float>(p.atlas_w) + 1.0f);
        assert(p.rect_y + p.rect_h <= static_cast<float>(p.atlas_h) + 1.0f);
    }
    // The task's own worked examples: the tracked mission's items (DATA-CONTRACT.md "Tracked
    // mission") and three NPCs this round's live boot cross-checked against the native Social
    // tab's heart/paw pip strip (DATA-CONTRACT.md "Social hearts").
    for (const char* n : {"Item_Drop_Bronze_rods", "Item_Drop_Bar_Bronze", "Item_Crop_Potato",
                          "npc_8009", "npc_8027", "npc_8071"})
        assert(idx.count(n) == 1);
    // UiPins() and SpriteIndex() are consulted in that order (Art::FindPin) but are disjoint
    // tables -- a name in one should not also silently live in the other this pass.
    for (const auto& [name, p] : mtas::UiPins()) {
        (void)p;
        assert(idx.count(name) == 0);
    }
    std::puts("TestSpriteIndex ok");
}

void TestSpriteLru() {
    // Pure cache bookkeeping: no bundle/ASTC access, synthetic images only (Solid(), already used
    // by TestNineSlice above).
    mtas::SpriteLru lru(/*max_entries=*/3, /*max_bytes=*/1'000'000);
    assert(lru.Get("a") == nullptr);
    lru.Put("a", Solid(10, 10, 1)); // 400 bytes
    lru.Put("b", Solid(10, 10, 2));
    lru.Put("c", Solid(10, 10, 3));
    assert(lru.EntryCount() == 3);
    assert(lru.Get("a") != nullptr && lru.Get("a")->rgba[0] == 1);
    // Insert a 4th: over the entry cap, must evict the least-recently-used -- "b" (never touched
    // since insertion, unlike "a" which Get() just refreshed).
    lru.Put("d", Solid(10, 10, 4));
    assert(lru.EntryCount() == 3);
    assert(lru.Get("b") == nullptr);
    assert(lru.Get("a") != nullptr && lru.Get("c") != nullptr && lru.Get("d") != nullptr);
    // Byte budget: a single huge image alone must not be kept once it exceeds max_bytes (evicted
    // immediately after insertion, not silently allowed to blow the budget).
    mtas::SpriteLru tiny_budget(/*max_entries=*/64, /*max_bytes=*/100);
    tiny_budget.Put("big", Solid(10, 10, 9)); // 400 bytes > 100-byte budget
    assert(tiny_budget.EntryCount() == 0 && tiny_budget.ByteCount() == 0);
    // Re-putting an existing key updates it in place rather than double-counting its bytes.
    mtas::SpriteLru replace(4, 1'000'000);
    replace.Put("x", Solid(10, 10, 5));
    const std::size_t after_first = replace.ByteCount();
    replace.Put("x", Solid(10, 10, 6));
    assert(replace.EntryCount() == 1 && replace.ByteCount() == after_first);
    assert(replace.Get("x")->rgba[0] == 6);
    std::puts("TestSpriteLru ok");
}

void TestFontGuards() {
    // Too short / not a real font: BuildLatinFontAtlas must refuse, never fabricate glyphs.
    assert(!mtas::BuildLatinFontAtlas({}, 36).has_value());
    assert(!mtas::BuildLatinFontAtlas(std::vector<std::uint8_t>{1, 2, 3}, 36).has_value());
    // Codepoint set covers at least the ASCII digits and letters this slice's Today page needs.
    const auto cps = mtas::DefaultCodepoints();
    bool has_colon = false, has_zero = false, has_W = false;
    for (const auto cp : cps) {
        has_colon |= cp == ':';
        has_zero |= cp == '0';
        has_W |= cp == 'W';
    }
    assert(has_colon && has_zero && has_W);
    std::puts("TestFontGuards ok");
}

void TestImageRequestGuards() {
    for (const auto key : {"portrait:2048:2048", "portrait:4096:1024", "portrait:1:1"})
        assert(mtas::ParseNameWH(key).has_value());
    for (const auto key : {"portrait:4096:1025", "portrait:4096:4096",
                           "portrait:4294967297:1", "portrait:1:4294967297",
                           "portrait:999999999999999999999999999999:1",
                           "portrait::1", "portrait:1:", "portrait:0:1", "portrait:1:0",
                           "portrait:a:1", "portrait:1:2x", "portrait:4097:1"})
        assert(!mtas::ParseNameWH(key).has_value());
    std::puts("TestImageRequestGuards ok (dimensions, overflow, output byte budget)");
}

void TestMissionTimeLeft() {
    // "now" = the same verified live timestamp as TestCalendar (day_of_world=100, Day 17 Winter
    // Y1 Wednesday, matching the native HUD "4.17 Wednesday").
    const std::uint64_t now = 100ULL * TicksPerDay;

    // Non-positive submission deadlines mean no deadline.
    {
        const auto t = mtas::MissionTimeLeft(now, 0);
        assert(!t.has_deadline);
        const auto t2 = mtas::MissionTimeLeft(now, -1);
        assert(!t2.has_deadline);
    }
    // 4 days 3 hours left: deadline = day 104 + 3 hours.
    {
        const std::uint64_t deadline = 104ULL * TicksPerDay + 3ULL * (TicksPerDay / 24);
        const auto t = mtas::MissionTimeLeft(now, static_cast<std::int64_t>(deadline));
        assert(t.has_deadline && !t.overdue);
        assert(t.days == 4);
        assert(t.hours == 3);
    }
    // Exactly at the deadline and past it: overdue, not a negative/garbage "time left".
    {
        const auto t = mtas::MissionTimeLeft(now, static_cast<std::int64_t>(now));
        assert(t.has_deadline && t.overdue);
        const auto t2 = mtas::MissionTimeLeft(now, static_cast<std::int64_t>(now - TicksPerDay));
        assert(t2.has_deadline && t2.overdue);
    }
    // The DateTime.Kind top bits on "now" must not perturb the diff (same invariant TestCalendar
    // checks for the calendar decode).
    {
        const std::uint64_t deadline = 104ULL * TicksPerDay;
        const auto plain = mtas::MissionTimeLeft(now, static_cast<std::int64_t>(deadline));
        const auto kinded = mtas::MissionTimeLeft(now | (1ULL << 62), static_cast<std::int64_t>(deadline));
        assert(plain.days == kinded.days && plain.hours == kinded.hours);
    }
    // fixture-pc1, Day 18 Winter Y1 07:10: tracked commission -9999789, Bronze Stick 5/6.
    // The expired acceptance deadline caused "Overdue"; native uses the submission deadline.
    {
        constexpr std::uint64_t live_now = 87522157110000ULL;
        assert(mtas::MissionTimeLeft(live_now, 87371999999999LL).overdue);
        const auto t = mtas::MissionTimeLeft(live_now, 90129446150000LL);
        assert(t.has_deadline && !t.overdue && t.days == 3 && t.hours == 0);
        assert(mtas::LeftText(t) == mtas::UiFormat(mtas::Ui("2044"), {"3", "0"}));
    }
    std::puts("TestMissionTimeLeft ok");
}

void TestFilterSelectionIsolation() {
    mtas::Module m;
    m.mission_sel = 7;
    assert(mtas::ActionCallback(&m, "set_social_filter", 2));
    assert(m.social_filter == 2 && m.mission_sel == 7);
    assert(!mtas::ActionCallback(&m, "set_mission_filter", 4));
    assert(m.mission_sel == 7);
    assert(mtas::ActionCallback(&m, "set_mission_filter", 1));
    assert(m.mission_filter == 1 && m.mission_sel == 0);
    std::puts("TestFilterSelectionIsolation ok");
}

void TestMidGameAttach() {
    auto live = [] {
        mtas::Model model;
        model.game_loaded = model.ui.ok = model.time.ok = model.time.calendar_ok = true;
        model.ui.state_type = 0;
        model.time.day = 18;
        return model;
    };
    mtas::Module m;
    auto refresh = [&](std::optional<bool> loading) {
        auto fresh = live();
        mtas::AcceptFreshModel(m, fresh, loading);
        return fresh.game_loaded;
    };
    assert(!refresh(false));
    assert(!refresh(false));
    assert(!refresh(std::nullopt)); // unknown must break the consecutive run
    assert(!refresh(false));
    assert(!refresh(false));
    assert(refresh(false));
    assert(m.held_ok && !m.saw_loading);
    auto missing = mtas::Model{};
    mtas::AcceptFreshModel(m, missing, false);
    assert(!m.held_ok);
    // Neither title nor native pause can establish a new mid-game attach.
    for (int state : {2, 6}) {
        for (int i = 0; i < 5; ++i) {
            auto title = live(); title.ui.state_type = state;
            mtas::AcceptFreshModel(m, title, false);
            assert(!title.game_loaded && !m.held_ok);
        }
    }
    for (int i = 0; i < 5; ++i) {
        auto boot = live(); boot.time.day = 0;
        mtas::AcceptFreshModel(m, boot, false);
        assert(!boot.game_loaded && !m.held_ok);
    }
    assert(!refresh(true));
    assert(!refresh(false));
    assert(!refresh(false));
    assert(refresh(false));
    // The original witnessed-loading path still accepts the first completed refresh.
    mtas::Module loaded;
    loaded.saw_loading = true;
    auto fresh = live();
    mtas::AcceptFreshModel(loaded, fresh, false);
    assert(fresh.game_loaded && loaded.held_ok);
    std::puts("TestMidGameAttach ok (consecutive closed, unknown reset, title, boot defaults, load)");
}

void TestLoadingHold() {
    Published out;
    out.host.main_base = 0x10000000;
    std::map<std::uint64_t, std::uint8_t> memory;
    auto put = [&](std::uint64_t at, auto value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i)
            memory[at + i] = bytes[i];
    };
    const std::uint64_t klass = 0x20000000, parent = klass + 0x400,
                        base = klass + 0x800, statics = klass + 0xC00,
                        self = klass + 0x1000, name = klass + 0x1400;
    put(out.host.main_base + mtas::SlotScenarioModule, klass);
    put(klass + mtas::OffName, name);
    for (std::size_t i = 0; i < 96; ++i)
        memory[name + i] = i < 14 ? "ScenarioModule"[i] : 0;
    put(klass + mtas::OffParent, parent);
    put(parent + mtas::OffParent, base);
    put(base + mtas::OffStaticFields, statics);
    put(statics, self);
    const auto ui_class = klass + 0x1800, ui_name = klass + 0x1C00,
               ui_statics = klass + 0x2000, ui = klass + 0x2400,
               shared = klass + 0x2800, dict = klass + 0x2C00,
               entries = klass + 0x3000, asset = klass + 0x3400,
               view = klass + 0x3800, view_class = klass + 0x3C00,
               view_name = klass + 0x4000;
    auto class_name = [&](std::uint64_t type, std::uint64_t text, const char* value) {
        put(type + mtas::OffName, text);
        for (std::size_t i = 0; i < 96; ++i)
            memory[text + i] = i < std::strlen(value) ? value[i] : 0;
    };
    class_name(ui_class, ui_name, "UISystemMgr");
    class_name(view_class, view_name, "LoadingMaskUI");
    put(out.host.main_base + mtas::SlotUiSystemMgr, ui_class);
    put(ui_class + mtas::OffStaticFields, ui_statics);
    put(ui_statics + 0x28, ui);
    put(ui + 0x18, shared);
    put(shared + 0x28, dict);
    put(dict + 0x18, entries);
    put(dict + 0x20, std::int32_t{1});
    put(entries + 0x18, std::int32_t{1});
    for (std::size_t i = 0; i < 24; ++i)
        memory[entries + 0x20 + i] = 0;
    put(entries + 0x28, std::int32_t{8});
    put(entries + 0x30, asset);
    put(asset + 0x50, view);
    put(view, view_class);
    put(view + 0x120, std::uint8_t{0});
    // Publisher userdata remains the output; memory is borrowed by this test callback.
    static decltype(memory)* active_memory;
    active_memory = &memory;
    out.host.read_memory = [](void*, std::uint64_t at, void* dst, std::size_t n) -> EdenDsmodBool {
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = active_memory->find(at + i);
            if (it == active_memory->end())
                return EDEN_DSMOD_FALSE;
            static_cast<std::uint8_t*>(dst)[i] = it->second;
        }
        return EDEN_DSMOD_TRUE;
    };
    mtas::Reader reader;
    memory[name] = 'X';
    assert(!reader.ReadLoading(out.host).has_value());
    memory[name] = 'S';
    assert(!reader.ReadLoading(out.host).has_value());
    put(self + 0xB1, std::uint8_t{2});
    assert(!reader.ReadLoading(out.host).has_value());
    put(self + 0xB1, std::uint8_t{1});
    assert(reader.ReadLoading(out.host).value());
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.time.ok = true;
    m.model.time.hour = 8;
    m.model.time.minute = 30;
    m.model.ui.ok = true;
    m.model.ui.state_type = 0;
    for (int i = 0; i < 18; ++i)
        mtas::SampleCallback(&m, &out.host);
    assert(m.loading && out.ints.at("mtas.loading") == 1);
    assert(out.texts.at("time.clock_text") == "08:30");
    assert(m.model.game_loaded && m.model.time.hour == 8);
    assert(!mtas::ActionCallback(&m, "select_item", 3));
    assert(m.item_sel == -1);
    // A transient unreadable root must not remove the cue or replace the held data.
    memory.erase(out.host.main_base + mtas::SlotScenarioModule);
    mtas::SampleCallback(&m, &out.host);
    assert(m.loading && m.model.game_loaded);
    put(out.host.main_base + mtas::SlotScenarioModule, klass);
    put(self + 0xB1, std::uint8_t{0});
    put(view + 0x120, std::uint8_t{1});
    assert(reader.ReadLoading(out.host).value());
    m.samples = 0;
    mtas::SampleCallback(&m, &out.host);
    assert(m.loading && m.model.game_loaded); // closing tween still holds the page
    put(view + 0x120, std::uint8_t{0});
    assert(!reader.ReadLoading(out.host).value());
    // Adjacent bytes remain readable: reject invalid capacities before interpreting entries.
    for (const int capacity : {-1, 0, 65537}) {
        put(entries + 0x18, static_cast<std::int32_t>(capacity));
        assert(!reader.ReadLoading(out.host).has_value());
    }
    put(entries + 0x18, std::int32_t{1});
    assert(!reader.ReadLoading(out.host).value());
    m.samples = 0;
    mtas::SampleCallback(&m, &out.host);
    assert(!m.loading && out.ints.at("mtas.loading") == 0);
    // With no Player root, refresh replaces the held model immediately on completion.
    assert(!m.model.game_loaded);
    assert(mtas::ActionCallback(&m, "select_item", 3));
    assert(m.item_sel == 3);
    std::puts("TestLoadingHold ok");
}

void TestDailyReaders() {
    std::map<std::uint64_t, std::uint8_t> memory;
    auto put = [&](std::uint64_t at, auto value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i)
            memory[at + i] = bytes[i];
    };
    std::uint64_t next = 0x20000000;
    auto object = [&]() {
        const auto at = next;
        next += 0x400;
        for (std::size_t i = 0; i < 0x400; ++i)
            memory[at + i] = 0;
        return at;
    };
    constexpr std::uint64_t main = 0x10000000;
    auto root = [&](std::uint64_t slot, const char* name, int hops = 3) {
        auto klass = object();
        put(main + slot, klass);
        const auto str = object();
        put(klass + mtas::OffName, str);
        for (std::size_t i = 0; i < std::strlen(name); ++i)
            memory[str + i] = name[i];
        for (int i = 0; i < hops; ++i) {
            const auto parent = object();
            put(klass + mtas::OffParent, parent);
            klass = parent;
        }
        const auto statics = object();
        const auto self = object();
        put(klass + mtas::OffStaticFields, statics);
        put(statics, self);
        return self;
    };
    auto list = [&](std::initializer_list<std::uint64_t> items) {
        const auto at = object();
        const auto array = object();
        put(at + 0x10, array);
        put(at + 0x18, static_cast<std::int32_t>(items.size()));
        put(array + 0x18, static_cast<std::int32_t>(items.size()));
        std::size_t i = 0;
        for (const auto item : items)
            put(array + 0x20 + 8 * i++, item);
        return at;
    };

    const auto clock = root(mtas::SlotTimeModule, "TimeModule");
    put(clock + 0x50, std::uint64_t{76 * TicksPerDay});
    const auto social = root(mtas::SlotSocialModule, "SocialModule");
    put(social + 0xC0, list({42}));
    const auto favor = object();
    put(favor + 0x10, std::int32_t{42});
    const auto dictionary = object();
    const auto entries = object();
    put(social + 0xA0, dictionary);
    put(dictionary + 0x18, entries);
    put(dictionary + 0x20, std::int32_t{1});
    put(entries + 0x18, std::int32_t{1});
    put(entries + 0x20 + 8, std::int32_t{42});
    put(entries + 0x20 + 0x10, favor);
    const auto conversation = root(mtas::SlotConversationManager, "ConversationManager");
    const auto talked = list({42});
    put(conversation + 0x38, talked);
    const auto gifts = root(mtas::SlotSendGiftModule, "SendGiftModule", 2);
    const auto sent = list({42});
    put(gifts + 0x20, sent);
    const auto calendar = root(mtas::SlotCalendarRemindModule, "CalendarRemindModule", 2);
    const auto day = object();
    put(calendar + 0x20, list({day}));
    put(day + 0x18, std::int32_t{1});
    put(day + 0x1C, std::int32_t{3});
    put(day + 0x20, std::int32_t{21});
    const auto reminder = object();
    put(day + 0x10, list({reminder}));
    const auto custom = object();
    const std::u16string stale = u"Earlier custom label";
    put(custom + 0x10, static_cast<std::int32_t>(stale.size()));
    for (std::size_t i = 0; i < stale.size(); ++i)
        put(custom + 0x14 + i * 2, stale[i]);
    put(reminder + 0x18, custom);
    put(reminder + 0x20, std::int32_t{1});
    put(reminder + 0x24, std::int32_t{1299});
    // The native holder owns message IDs; no reminder text is embedded in the reader.
    const auto holder = root(mtas::SlotCalendarRemindPresetHolder, "CalendarRemindPresetHolder", 0);
    // A static class has no Module.Self indirection.
    const auto preset_list = list({});
    const auto preset_array = object();
    put(preset_list + 0x10, preset_array);
    put(preset_list + 0x18, std::int32_t{2});
    put(preset_array + 0x18, std::int32_t{2});
    put(preset_array + 0x20, std::int32_t{1300});
    put(preset_array + 0x24, std::int32_t{1301});
    // root() returns the first static pointer, which is the list for this static class.
    for (std::size_t i = 0; i < 0x400; ++i)
        memory[holder + i] = memory[preset_list + i];
    EdenDsmodHostApi host{};
    host.main_base = main;
    host.userdata = &memory;
    host.read_memory = [](void* p, std::uint64_t at, void* out, std::size_t size) -> EdenDsmodBool {
        const auto& mem = *static_cast<decltype(memory)*>(p);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = mem.find(at + i);
            if (it == mem.end())
                return EDEN_DSMOD_FALSE;
            static_cast<std::uint8_t*>(out)[i] = it->second;
        }
        return EDEN_DSMOD_TRUE;
    };
    mtas::Reader reader;
    auto chat = [&]() {
        const auto model = reader.Refresh(host);
        assert(model.social.npcs.size() == 1);
        return model.social.npcs[0];
    };
    const auto preference = object();
    put(preference + 0x10, std::int32_t{42});
    auto ints = [&](std::initializer_list<std::int32_t> values) {
        const auto at = list({});
        const auto array = object();
        put(at + 0x10, array);
        put(at + 0x18, static_cast<std::int32_t>(values.size()));
        put(array + 0x18, static_cast<std::int32_t>(values.size()));
        std::size_t i = 0;
        for (const auto value : values)
            put(array + 0x20 + 4 * i++, value);
        return at;
    };
    put(gifts + 0x28, list({preference}));
    put(gifts + 0x38, list({}));
    put(preference + 0x18, ints({999})); // Gift history alone must never reveal a like.
    assert(chat().known_likes.empty());
    put(preference + 0x28, ints({101, 102}));
    const auto liked = ints({102, 103});
    put(preference + 0x30, liked);
    put(preference + 0x38, ints({104})); // Disliked gifts are excluded.
    assert((chat().known_likes == std::vector<mtas::s32>{101, 102, 103}));
    put(liked + 0x18, std::int32_t{3}); // Corrupt capacity fails closed.
    assert(chat().known_likes.empty());
    put(liked + 0x18, std::int32_t{2});
    const auto shared = object();
    put(shared + 0x10, std::int32_t{40});
    put(shared + 0x14, std::int32_t{45});
    put(shared + 0x20, ints({201}));
    put(shared + 0x28, ints({202}));
    put(gifts + 0x38, list({shared}));
    assert((chat().known_likes == std::vector<mtas::s32>{201, 202}));
    put(shared + 0x10, std::int32_t{43}); // Another NPC's range must not leak.
    assert((chat().known_likes == std::vector<mtas::s32>{101, 102, 103}));
    put(preference + 0x10, std::int32_t{43});
    assert(chat().known_likes.empty());
    std::puts("TestKnownLikes ok (discovery-only, category order, ranges, corrupt data)");
    assert(chat().chatted_ok && chat().chatted_today);
    put(talked + 0x18, std::int32_t{0}); // native wake-up reset
    assert(chat().chatted_ok && !chat().chatted_today);
    put(talked + 0x18, std::int32_t{1});
    assert(chat().chatted_ok && chat().chatted_today);
    for (const int invalid_size : {-1, 257}) {
        put(talked + 0x18, static_cast<std::int32_t>(invalid_size));
        assert(!chat().chatted_ok && !chat().chatted_today);
    }
    put(talked + 0x18, std::int32_t{2}); // capacity is only one
    assert(!chat().chatted_ok);
    put(talked + 0x18, std::int32_t{1});
    put(conversation + 0x38, std::uint64_t{0});
    assert(!chat().chatted_ok);
    put(conversation + 0x38, talked);
    // Bug #2: ReadGiftedToday must mirror ReadChattedToday exactly -- any inner validation
    // failure is "unresolved" (gifted_ok=false), never a false "resolved, nobody gifted today".
    assert(chat().gifted_ok && chat().gifted_today);
    put(sent + 0x18, std::int32_t{0}); // native OnDayBegin reset
    assert(chat().gifted_ok && !chat().gifted_today);
    put(sent + 0x18, std::int32_t{1});
    assert(chat().gifted_ok && chat().gifted_today);
    for (const int invalid_size : {-1, 257}) {
        put(sent + 0x18, static_cast<std::int32_t>(invalid_size));
        assert(!chat().gifted_ok && !chat().gifted_today);
    }
    put(sent + 0x18, std::int32_t{2}); // capacity is only one
    assert(!chat().gifted_ok);
    put(sent + 0x18, std::int32_t{1});
    put(gifts + 0x20, std::uint64_t{0});
    assert(!chat().gifted_ok);
    put(gifts + 0x20, sent);
    assert(chat().gifted_ok && chat().gifted_today);
    auto events = [&]() { return reader.Refresh(host).week.events; };
    auto rows = events();
    assert(rows.size() == 1 && rows[0].label == mtas::GameText().Get(1301));
    put(reminder + 0x20, std::int32_t{99}); // native GetPreset clamps to the final entry
    assert(events()[0].label == mtas::GameText().Get(1301));
    put(reminder + 0x20, std::int32_t{-1});
    assert(events()[0].label == mtas::GameText().Get(1299));
    put(reminder + 0x24, std::int32_t{0});
    assert(events()[0].label == "Earlier custom label");
    put(day + 0x10, list({}));
    assert(events().empty());
    mtas::Module module;
    module.model.game_loaded = true;
    module.model.social.ok = true;
    module.model.social.npcs.resize(1);
    module.npc_sel = 0;
    for (const bool known : {false, true}) {
        for (const bool done : {false, true}) {
            auto& npc = module.model.social.npcs[0];
            npc.chatted_ok = known;
            npc.chatted_today = done;
            Published published;
            module.samples = 1;
            module.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
            mtas::SampleCallback(&module, &published.host);
            for (const auto* prefix : {"npc0.", "npc_detail."}) {
                const std::string key = prefix;
                assert(published.ints.at(key + "chatted_ok") == known);
                assert(published.ints.at(key + "chatted_today") == (known && done));
                assert(published.ints.at(key + "chat_pending") == (known && !done));
            }
        }
    }
    std::puts("TestDailyReaders ok (chat/gift known/done/reset/invalid; native reminder precedence)");
}

void TestMissionReaderDeadlines() {
    // Exercise the actual reader with distinct available/running lists and acceptance/submission
    // deadlines: using either old offset must fail, even when the arithmetic itself is correct.
    std::map<std::uint64_t, std::uint8_t> memory;
    auto put = [&](std::uint64_t at, auto value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i)
            memory[at + i] = bytes[i];
    };
    std::uint64_t next = 0x20000000;
    auto object = [&]() {
        const auto at = next;
        next += 0x400;
        for (std::size_t i = 0; i < 0x400; ++i)
            memory[at + i] = 0;
        return at;
    };
    constexpr std::uint64_t main = 0x10000000;
    auto root = [&](std::uint64_t slot, const char* name, int hops = 3) {
        auto klass = object();
        put(main + slot, klass);
        const auto str = object();
        put(klass + mtas::OffName, str);
        for (std::size_t i = 0; i < std::strlen(name); ++i)
            memory[str + i] = name[i];
        for (int i = 0; i < hops; ++i) {
            const auto parent = object();
            put(klass + mtas::OffParent, parent);
            klass = parent;
        }
        const auto statics = object();
        const auto self = object();
        put(klass + mtas::OffStaticFields, statics);
        put(statics, self);
        return self;
    };
    auto list = [&](std::initializer_list<std::uint64_t> items) {
        const auto at = object();
        const auto array = object();
        put(at + 0x10, array);
        put(at + 0x18, static_cast<std::int32_t>(items.size()));
        put(array + 0x18, static_cast<std::int32_t>(items.size()));
        std::size_t i = 0;
        for (const auto item : items)
            put(array + 0x20 + 8 * i++, item);
        return at;
    };
    const auto clock = root(mtas::SlotTimeModule, "TimeModule");
    constexpr std::uint64_t now = 87522157110000ULL; // same live fixture ticks as above
    put(clock + 0x50, now);
    const auto manager = root(mtas::SlotMissionManager, "MissionManager");
    const auto offer = object(), tracked = object(), other = object(), untimed = object();
    put(manager + 0xC0, list({offer}));
    put(manager + 0xC8, list({tracked, other, untimed}));
    put(manager + 0xE0, list({tracked}));
    put(tracked + 0x10, std::int32_t{-9999789});
    put(tracked + 0x38, std::int64_t{87371999999999LL});
    put(tracked + 0x48, std::int64_t{90129446150000LL});
    put(other + 0x38, now + TicksPerDay);
    put(other + 0x48, now + 2 * TicksPerDay);
    put(untimed + 0x48, std::int64_t{-1});
    const auto completed = object(), failed = object(), current = object();
    put(untimed + 0x78, list({completed, failed, current}));
    put(completed + 0x34, std::int32_t{1});
    put(failed + 0x34, std::int32_t{-1});
    put(current + 0x34, std::int32_t{0});
    put(current + 0x1C, std::int32_t{123456});
    put(tracked + 0x118, std::int32_t{42});
    put(tracked + 0x11C, std::int32_t{6});
    const auto progress_target = object(), progress_list = object(), progress_array = object();
    put(tracked + 0x78, list({progress_target}));
    put(progress_target + 0x40, progress_list);
    put(progress_list + 0x10, progress_array);
    put(progress_list + 0x18, std::int32_t{1});
    put(progress_array + 0x18, std::int32_t{1});
    put(progress_array + 0x20, std::int32_t{42});
    put(progress_array + 0x24, std::int32_t{5});
    // A late save has 1,929 unlock records; these are not the displayed catalogue count.
    const auto blueprints = root(mtas::SlotBluePrintModule, "BluePrintModule", 2);
    const auto unlock_list = object(), unlock_array = object();
    put(blueprints + 0x28, unlock_list);
    put(unlock_list + 0x10, unlock_array);
    put(unlock_list + 0x18, std::int32_t{1929});
    put(unlock_array + 0x18, std::int32_t{1929});
    for (std::size_t i = 0; i < 1929 * 0x18; ++i)
        memory[unlock_array + 0x20 + i] = 0;
    // Localization is intentionally unloaded: trace the reader's selected title field instead.
    std::vector<std::uint64_t> reads;
    std::pair<decltype(memory)*, decltype(reads)*> trace{&memory, &reads};
    EdenDsmodHostApi host{};
    host.main_base = main;
    host.userdata = &trace;
    host.read_memory = [](void* p, std::uint64_t at, void* out, std::size_t size) -> EdenDsmodBool {
        const auto& trace = *static_cast<std::pair<decltype(memory)*, decltype(reads)*>*>(p);
        trace.second->push_back(at);
        const auto& mem = *trace.first;
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = mem.find(at + i);
            if (it == mem.end())
                return EDEN_DSMOD_FALSE;
            static_cast<std::uint8_t*>(out)[i] = it->second;
        }
        return EDEN_DSMOD_TRUE;
    };
    mtas::Reader reader;
    const auto model = reader.Refresh(host);
    assert(model.missions.ok && model.missions.running.size() == 3);
    assert(model.missions.running[0].mission_id == -9999789);
    assert(model.missions.running[0].left.days == 3 && !model.missions.running[0].left.overdue);
    assert(model.missions.running[1].left.days == 2);
    assert(!model.missions.running[2].left.has_deadline);
    assert(std::count(reads.begin(), reads.end(), current + 0x1C) == 1);
    assert(std::count(reads.begin(), reads.end(), completed + 0x1C) == 0);
    assert(std::count(reads.begin(), reads.end(), failed + 0x1C) == 0);
    assert(model.tracked.ok && model.tracked.left.days == 3 && !model.tracked.left.overdue);
    assert(model.tracked.left.hours == 0 && mtas::LeftText(model.tracked.left) == mtas::UiFormat(mtas::Ui("2044"), {"3", "0"}));
    assert(model.handbook.ok);
    assert(model.missions.running[0].item_have_ok && model.missions.running[0].item_have == 5);
    for (const int capacity : {-1, 0, 65537}) {
        put(progress_array + 0x18, static_cast<std::int32_t>(capacity));
        reads.clear();
        const auto invalid = reader.Refresh(host);
        assert(!invalid.missions.running[0].item_have_ok);
        assert(!invalid.tracked.item.have_ok);
        assert(std::count(reads.begin(), reads.end(), progress_array + 0x20) == 0);
    }
    put(progress_array + 0x18, std::int32_t{1});
    assert(reader.Refresh(host).missions.running[0].item_have_ok);
    reads.clear();
    reader.Refresh(host);
    assert(std::count(reads.begin(), reads.end(), unlock_array + 0x20) == 1);
    for (const auto [size, capacity] : {std::pair{1929, 1928}, std::pair{65537, 65537},
                                      std::pair{-1, 1929}}) {
        put(unlock_list + 0x18, std::int32_t{size});
        put(unlock_array + 0x18, std::int32_t{capacity});
        reads.clear();
        reader.Refresh(host);
        assert(std::count(reads.begin(), reads.end(), unlock_array + 0x20) == 0);
    }
    std::puts("TestHandbookReaderLargeList ok (1929 records; invalid size/capacity refused)");
    std::puts("TestMissionReaderDeadlines ok (running roster, submission deadlines, tracked, untimed, current target)");
}

void TestRootCaching() {
    // PERF fix: MissionManager's root (ResolveRoot) used to be re-derived from scratch on every
    // call, and allMissionBaseInfoById's ~1,069-entry index used to be rebuilt from scratch on
    // EVERY call to ReadMissionBaseInfoIndex -- twice per Refresh() (once from ReadMissions, once
    // from ReadTracked), forever, every tick. Proves both are now cached: the base-info
    // dictionary's own backing array is read AT MOST ONCE, total, across two full Refresh() calls
    // on an unchanged fixture (not 4 times, which is what the old code did: twice per call x 2
    // calls), and each root's own class-name/hop-walk reads collapse to near-nothing on the
    // second, warm call.
    std::map<std::uint64_t, std::uint8_t> memory;
    auto put = [&](std::uint64_t at, auto value) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t i = 0; i < sizeof(value); ++i)
            memory[at + i] = bytes[i];
    };
    std::uint64_t next = 0x20000000;
    auto object = [&]() {
        const auto at = next;
        next += 0x400;
        for (std::size_t i = 0; i < 0x400; ++i)
            memory[at + i] = 0;
        return at;
    };
    constexpr std::uint64_t main = 0x10000000;
    auto root = [&](std::uint64_t slot, const char* name, int hops = 3) {
        auto klass = object();
        put(main + slot, klass);
        const auto str = object();
        put(klass + mtas::OffName, str);
        for (std::size_t i = 0; i < std::strlen(name); ++i)
            memory[str + i] = name[i];
        for (int i = 0; i < hops; ++i) {
            const auto parent = object();
            put(klass + mtas::OffParent, parent);
            klass = parent;
        }
        const auto statics = object();
        const auto self = object();
        put(klass + mtas::OffStaticFields, statics);
        put(statics, self);
        return self;
    };
    auto list = [&](std::initializer_list<std::uint64_t> items) {
        const auto at = object();
        const auto array = object();
        put(at + 0x10, array);
        put(at + 0x18, static_cast<std::int32_t>(items.size()));
        put(array + 0x18, static_cast<std::int32_t>(items.size()));
        std::size_t i = 0;
        for (const auto item : items)
            put(array + 0x20 + 8 * i++, item);
        return at;
    };

    const auto manager = root(mtas::SlotMissionManager, "MissionManager", 3);
    const auto tracked = object();
    put(tracked + 0x10, std::int32_t{7000}); // missionId
    put(manager + 0xC8, list({tracked}));    // m_missions_Running
    put(manager + 0xE0, list({tracked}));    // traceList -- non-empty, so ReadTracked proceeds
                                             // far enough to call ReadMissionBaseInfoIndex too
    // allMissionBaseInfoById: a small Dictionary<int,MissionBaseInfo>, 3 live entries.
    const auto dict = object();
    const auto entries = object();
    put(dict + 0x18, entries);
    put(dict + 0x20, std::int32_t{3});
    put(entries + 0x18, std::int32_t{3});
    for (int i = 0; i < 3; ++i) {
        put(entries + 0x20 + i * 0x18 + 0x00, std::int32_t{1});        // hashCode >= 0 (live slot)
        put(entries + 0x20 + i * 0x18 + 0x08, std::int32_t{7000 + i}); // key (missionId)
        put(entries + 0x20 + i * 0x18 + 0x10, std::uint64_t{0});       // value (unused here)
    }
    put(manager + 0x20, dict); // allMissionBaseInfoById
    const auto entries_data_addr = entries + 0x20; // DictEntries' own bulk-read address

    EdenDsmodHostApi host{};
    host.main_base = main;
    std::vector<std::uint64_t> reads;
    std::pair<decltype(memory)*, decltype(reads)*> trace{&memory, &reads};
    host.userdata = &trace;
    host.read_memory = [](void* p, std::uint64_t at, void* out, std::size_t size) -> EdenDsmodBool {
        const auto& trace = *static_cast<std::pair<decltype(memory)*, decltype(reads)*>*>(p);
        trace.second->push_back(at);
        const auto& mem = *trace.first;
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = mem.find(at + i);
            if (it == mem.end())
                return EDEN_DSMOD_FALSE;
            static_cast<std::uint8_t*>(out)[i] = it->second;
        }
        return EDEN_DSMOD_TRUE;
    };
    mtas::Reader reader;

    reads.clear();
    const auto model1 = reader.Refresh(host);
    const auto reads1 = reads.size();
    const auto base_info_reads_in_call1 =
        std::count(reads.begin(), reads.end(), entries_data_addr);

    reads.clear();
    const auto model2 = reader.Refresh(host);
    const auto reads2 = reads.size();
    const auto base_info_reads_in_call2 =
        std::count(reads.begin(), reads.end(), entries_data_addr);

    // Behaviour is unchanged by caching: both calls read the same live data the same way.
    assert(model1.missions.ok && model2.missions.ok);
    assert(model1.missions.running.size() == 1 && model2.missions.running.size() == 1);
    assert(model1.tracked.ok && model2.tracked.ok);
    assert(model1.missions.running[0].mission_class == model2.missions.running[0].mission_class);

    // The dictionary's own backing array is read at most once across these two calls combined --
    // the OLD code read it 4 times (ReadMissions + ReadTracked, each calling
    // ReadMissionBaseInfoIndex, x2 Refresh() calls). This is the direct proof item 6's own "build
    // the index once per boot, not twice per tick" fix actually works, not just that it compiles.
    assert(base_info_reads_in_call1 + base_info_reads_in_call2 <= 1);

    // The second, warm Refresh() call does meaningfully fewer total guest reads than the first,
    // cold one -- the broader ResolveRoot cache's own effect (MissionManager's root, hit 4x per
    // tick by this fixture alone, no longer re-walks its name/hops chain after the first time).
    assert(reads2 < reads1);
    std::printf("TestRootCaching: guest reads per Refresh -- cold=%zu warm=%zu (base-info array read "
               "%lld time(s) total across both calls)\n",
               reads1, reads2, static_cast<long long>(base_info_reads_in_call1 + base_info_reads_in_call2));

    std::puts("TestRootCaching ok (MissionManager root + base-info index cached across Refresh calls)");
}

void TestComputeMissionClass() {
    // Round-4 follow-up: base_info_is_main (MissionBaseInfo.IsMain, found via MissionManager.
    // allMissionBaseInfoById) is the real signal, live-verified against 3 native coloured-dot
    // badges in one boot ("Recipe for Fun"=true->Main/orange, "The Request Board"/
    // "Get Con-yak-ted"=false->Side/blue, DATA-CONTRACT.md "Mission class").
    assert(mtas::ComputeMissionClass(true, 0) == mtas::MissionClass::Main);
    assert(mtas::ComputeMissionClass(true, 9) == mtas::MissionClass::Main);
    assert(mtas::ComputeMissionClass(true, -1) == mtas::MissionClass::Main);
    assert(mtas::ComputeMissionClass(false, -1) == mtas::MissionClass::Side); // "The Request Board"
    assert(mtas::ComputeMissionClass(false, 0) == mtas::MissionClass::Side);  // Side can co-occur
                                                                               // with any group_type
                                                                               // once base info exists
    // No MissionBaseInfo entry at all (nullopt): falls to the group_type heuristic.
    // CommercialOrder(0)/TownOrder(1): the order-board systems ("Sandrock Commission", live
    // groupType==0, native GREEN/Commission badge, confirmed NOT in allMissionBaseInfoById).
    assert(mtas::ComputeMissionClass(std::nullopt, 0) == mtas::MissionClass::Commission);
    assert(mtas::ComputeMissionClass(std::nullopt, 1) == mtas::MissionClass::Commission);
    // Everything else with no base info: Other, never guessed as Main/Side.
    assert(mtas::ComputeMissionClass(std::nullopt, 9) == mtas::MissionClass::Other);  // MonsterMission
    assert(mtas::ComputeMissionClass(std::nullopt, -1) == mtas::MissionClass::Other); // Normal, no
                                                                                       // base info
    assert(mtas::ComputeMissionClass(std::nullopt, 2) == mtas::MissionClass::Other);
    std::puts("TestComputeMissionClass ok");
}

void TestMergeHaveCounts() {
    // Round-4 fix: have_by_item must sum BAG (InventoryFields) + every STORAGE box
    // (StorageFields), not bag alone -- the exact bug the UI lane found (every recipe/blueprint
    // reading have=0 in a save whose ingredients sit entirely in storage).
    mtas::InventoryFields inv;
    inv.ok = true;
    inv.items.push_back({/*item_id=*/16200003, "Potato", "", /*count=*/2, 0, 0, false});
    inv.items.push_back({/*item_id=*/19300011, "Vinegar", "", /*count=*/1, 0, 0, false});

    mtas::StorageFields storage;
    storage.ok = true;
    mtas::StorageBox box1, box2;
    box1.items.push_back({/*item_id=*/16200003, "Potato", "", /*count=*/5, 0, 0, true});
    box1.items.push_back({/*item_id=*/19300010, "Chili", "", /*count=*/3, 0, 0, true});
    box2.items.push_back({/*item_id=*/16200003, "Potato", "", /*count=*/1, 0, 0, true});
    storage.boxes.push_back(box1);
    storage.boxes.push_back(box2);

    const auto merged = mtas::MergeHaveCounts(inv, storage);
    // Potato: 2 (bag) + 5 (box1) + 1 (box2) = 8, summed across sources AND across boxes.
    auto it = merged.find(16200003);
    assert(it != merged.end() && it->second == 8);
    // Vinegar: bag-only, 1.
    it = merged.find(19300011);
    assert(it != merged.end() && it->second == 1);
    // Chili: storage-only, 3.
    it = merged.find(19300010);
    assert(it != merged.end() && it->second == 3);
    // An item in neither source is simply absent (never a fabricated 0 entry).
    assert(merged.find(99999999) == merged.end());

    // storage.ok == false (chain didn't resolve): bag-only sum, no crash on empty boxes.
    mtas::StorageFields unresolved_storage;
    const auto bag_only = mtas::MergeHaveCounts(inv, unresolved_storage);
    assert(bag_only.at(16200003) == 2);
    assert(bag_only.find(19300010) == bag_only.end()); // Chili never appears: storage not merged

    // inventory.ok == false: storage-only sum.
    mtas::InventoryFields unresolved_inv;
    const auto storage_only = mtas::MergeHaveCounts(unresolved_inv, storage);
    assert(storage_only.at(16200003) == 6); // 5 + 1, no bag contribution
    assert(storage_only.find(19300011) == storage_only.end()); // Vinegar never appears

    std::puts("TestMergeHaveCounts ok");
}

void TestMergeStorageItems() {
    // Bug fix: wires ReadStorage's live boxes into InventoryFields.items/storage_ok, which
    // ReadInventory itself never touched -- the Inventory page's "Storage" filter pill
    // (0100B63016916000.cpp) was permanently dead without this.
    auto make = [](mtas::s32 id, const char* name, mtas::s32 count, mtas::s32 grade,
                   bool in_storage) {
        mtas::InvItem it;
        it.item_id = id;
        it.name = name;
        it.count = count;
        it.grade = grade;
        it.in_storage = in_storage;
        return it;
    };

    mtas::InventoryFields inv;
    inv.ok = true;
    inv.items.push_back(make(16200003, "Potato", 2, 0, false)); // bag stack

    mtas::StorageFields storage;
    storage.ok = true;
    mtas::StorageBox box1, box2;
    box1.items.push_back(make(16200003, "Potato", 5, 0, true));  // same item_id+grade as the bag
    box1.items.push_back(make(19300010, "Chili", 3, 0, true));
    box2.items.push_back(make(16200003, "Potato", 1, 0, true));  // same item_id+grade as box1's
    box2.items.push_back(make(16200003, "Potato", 4, 1, true));  // same item_id, DIFFERENT grade
    storage.boxes.push_back(box1);
    storage.boxes.push_back(box2);

    mtas::MergeStorageItems(inv, storage);
    assert(inv.storage_ok);
    // Bag Potato (in_storage=false) is untouched -- its own row, still count 2.
    const auto bag_it = std::find_if(inv.items.begin(), inv.items.end(),
        [](const mtas::InvItem& it) { return !it.in_storage && it.item_id == 16200003; });
    assert(bag_it != inv.items.end() && bag_it->count == 2);
    // Storage Potato grade 0 merged across BOTH boxes into ONE row: 5 + 1 = 6.
    const auto storage_g0 = std::find_if(inv.items.begin(), inv.items.end(),
        [](const mtas::InvItem& it) {
            return it.in_storage && it.item_id == 16200003 && it.grade == 0;
        });
    assert(storage_g0 != inv.items.end() && storage_g0->count == 6);
    // Storage Potato grade 1 is its OWN row (a different grade never merges), count 4.
    const auto storage_g1 = std::find_if(inv.items.begin(), inv.items.end(),
        [](const mtas::InvItem& it) {
            return it.in_storage && it.item_id == 16200003 && it.grade == 1;
        });
    assert(storage_g1 != inv.items.end() && storage_g1->count == 4);
    // Chili: storage-only, one row, untouched count.
    const auto chili = std::find_if(inv.items.begin(), inv.items.end(),
        [](const mtas::InvItem& it) { return it.item_id == 19300010; });
    assert(chili != inv.items.end() && chili->count == 3 && chili->in_storage);
    // Exactly 4 rows total: bag Potato, storage Potato(g0), storage Potato(g1), Chili -- no
    // duplicate row per box for the same (item_id, grade).
    assert(inv.items.size() == 4);

    // storage.ok == false: storage_ok is set honestly to false, items untouched, no crash on
    // empty boxes.
    mtas::InventoryFields inv2;
    inv2.ok = true;
    inv2.items.push_back(make(16200003, "Potato", 2, 0, false));
    mtas::StorageFields unresolved_storage;
    mtas::MergeStorageItems(inv2, unresolved_storage);
    assert(!inv2.storage_ok);
    assert(inv2.items.size() == 1);

    std::puts("TestMergeStorageItems ok (box merge by item+grade, bag untouched, storage_ok honest)");
}

// ---- Synthetic-blob byte builders, matching the MonoBehaviour layout every table in
// mtas_text.cpp shares (ASSET-PATHS.md §1/§2/§3): 28-byte header (m_GameObject/m_Enabled/
// m_Script -- contents irrelevant, only skipped) + m_Name (UnityStr) + tag (UnityStr) +
// int32 count + rows. UnityStr = int32 length + bytes, padded to a 4-byte boundary.

void PutI32(std::vector<std::uint8_t>& b, std::int32_t v) {
    std::uint8_t raw[4];
    std::memcpy(raw, &v, 4);
    b.insert(b.end(), raw, raw + 4);
}
void PutU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    std::uint8_t raw[8];
    std::memcpy(raw, &v, 8);
    b.insert(b.end(), raw, raw + 8);
}
void PutF32(std::vector<std::uint8_t>& b, float v) {
    std::uint8_t raw[4];
    std::memcpy(raw, &v, 4);
    b.insert(b.end(), raw, raw + 4);
}
void PutStr(std::vector<std::uint8_t>& b, const char* s) {
    const std::size_t n = std::strlen(s);
    PutI32(b, static_cast<std::int32_t>(n));
    b.insert(b.end(), s, s + n);
    while (b.size() % 4 != 0)
        b.push_back(0);
}
void PutEmptyArray(std::vector<std::uint8_t>& b) {
    PutI32(b, 0);
}
void PutHeader(std::vector<std::uint8_t>& b) {
    b.resize(b.size() + 28, 0); // m_GameObject PPtr + m_Enabled + m_Script PPtr: skipped, not read
}

void TestTextParsers() {
    // AssetItem<Language> (loc table): two rows, deliberately out of id order to prove the
    // parser sorts ascending.
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetItemEnglish");
        PutStr(b, "Text");
        PutI32(b, 2);
        PutI32(b, 200);
        PutStr(b, "World");
        PutI32(b, 100);
        PutStr(b, "Hello");
        const auto rows = mtas::ParseLocTable(b);
        assert(rows.size() == 2);
        assert(rows[0].id == 100 && rows[0].text == "Hello");
        assert(rows[1].id == 200 && rows[1].text == "World");
    }
    // Corrupt/truncated table (declared count doesn't match what's actually there): refused
    // whole, not published half-decoded.
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetItemEnglish");
        PutStr(b, "Text");
        PutI32(b, 3); // declares 3, only 1 follows
        PutI32(b, 1);
        PutStr(b, "Only one");
        assert(mtas::ParseLocTable(b).empty());
    }

    // AssetItemPrototypeItem: one real row (ASSET-PATHS.md §2's own first-row example), every
    // field after male/femaleIconPath/nameId present but not kept by the parser.
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetItemPrototypeItem");
        PutStr(b, "ItemPrototype");
        PutI32(b, 1);
        PutI32(b, 10000000);                 // id
        PutStr(b, "Item_Drop_Master_cooking"); // maleIconPath
        PutStr(b, "");                         // femaleIconPath
        PutI32(b, 91000729);                   // nameId
        PutI32(b, 7654321);                     // infoId
        PutStr(b, "");                           // designer
        PutEmptyArray(b);                        // itemFromTypes
        PutStr(b, ""); PutStr(b, ""); PutStr(b, ""); // male/femaleModelPath, dropModelPath
        PutStr(b, "");                                // interactableModelPath
        PutEmptyArray(b);                              // attachModelID
        PutI32(b, 1);                                   // stackNumber
        PutEmptyArray(b);                                // skillIds
        PutI32(b, 1); PutI32(b, 0); PutI32(b, 0);         // isDefaultKnown/IsMissionItem/showroomMark
        PutEmptyArray(b); PutEmptyArray(b);                 // dlcRequires, gradeWeight
        PutI32(b, 0);                                        // orderIndex
        PutEmptyArray(b);                                     // tags
        PutI32(b, 0);                                          // buyPrice
        PutI32(b, 0); PutI32(b, 0);                             // sellPrice (a,b)
        PutEmptyArray(b);                                        // itemTag
        PutI32(b, 0);                                             // holdType
        PutI32(b, 0); PutI32(b, 0); PutI32(b, 0);                  // cantSold/cantDiscard/cantRefine
        PutI32(b, 0);                                                // dropStackNumber
        PutStr(b, "");                                                // complexModelPath
        PutEmptyArray(b);                                              // blueprintFromId
        const auto rows = mtas::ParseItemPrototypeTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 10000000);
        assert(rows[0].name_id == 91000729);
        assert(rows[0].info_id == 7654321);
        assert(rows[0].male_icon == "Item_Drop_Master_cooking");
        assert(rows[0].female_icon.empty());
    }

    // AssetNpcProtoDataNpc: Fennec Fox's own real row (npc id 8071, nameId 32000027, birthday
    // 72576000000000 -- research/.../analysis/english_sample.csv cross-ref + the live NPC table,
    // see DecodeNpcBirthday's own test below for the Day-1-Winter proof).
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetNpcProtoDataNpc");
        PutStr(b, "Npc");
        PutI32(b, 1);
        PutI32(b, 8071);               // id
        PutI32(b, 0);                   // templetID
        PutI32(b, 32000027);             // nameID
        PutI32(b, 0);                     // identityID
        PutI32(b, 0);                       // initActorLevel
        PutU64(b, 72576000000000ULL);        // birthday
        PutF32(b, 0); PutF32(b, 0); PutF32(b, 0); // defaultPos
        PutI32(b, 0);                               // defaultScene
        PutF32(b, 0); PutF32(b, 0); PutF32(b, 0);     // homePos
        PutI32(b, 0);                                   // homeScene
        PutI32(b, 0);                                     // appearSuits
        PutI32(b, 0);                                       // canLove
        PutEmptyArray(b);                                     // characteristic
        PutI32(b, 0); PutI32(b, 0); PutI32(b, 0); PutI32(b, 0); PutI32(b, 0); PutI32(b, 0);
        // initFavor/initFavorType/initFavorLevel/breakup/breakupInitiative/divorce
        PutI32(b, 0);     // divorceInitiative
        PutEmptyArray(b); // aiTags (str_array: same 0-count shape)
        PutEmptyArray(b); // tags
        PutEmptyArray(b); // defaultBuffs
        PutI32(b, 0);     // characteristics
        PutEmptyArray(b); // interactChoice
        PutF32(b, 0); PutF32(b, 0); // partyFoods
        PutF32(b, 0); PutF32(b, 0); // partyFactor
        PutStr(b, "26 cm");          // height
        PutStr(b, "1.2 kg");          // weight
        PutI32(b, 0); PutI32(b, 0); PutI32(b, 0); // sensorId/fovHearId/expFactor
        PutEmptyArray(b); // backgrounds (pair array: 0-count shape)
        PutEmptyArray(b); // influences
        PutI32(b, 0); PutI32(b, 0); // expPerDay/rescueCount
        PutEmptyArray(b); PutEmptyArray(b); PutEmptyArray(b); // rescueFavor/rescueBuffId/rescueBubbleId
        PutF32(b, 0); // exhaustionTime
        PutI32(b, 0); // preCombineModel
        const auto rows = mtas::ParseNpcProtoTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 8071);
        assert(rows[0].name_id == 32000027);
        assert(rows[0].birthday_raw == 72576000000000ULL);
    }

    // AssetSocialLevelConfigSocialLevel: Heidi's own real live row (level=4/Friend, type=0/
    // Normal, favor range [150,400] -- LIVE-READS.md's own mLevel=4 example) plus its "next
    // level" row (5), to prove ComputeNextLevelInfo below finds the real neighbour.
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetSocialLevelConfigSocialLevel");
        PutStr(b, "SocialLevel");
        PutI32(b, 2);
        for (const auto& row : {std::array<std::int32_t, 4>{4, 0, 150, 400},
                                std::array<std::int32_t, 4>{5, 0, 350, 600}}) {
            PutI32(b, row[0]); // level
            PutI32(b, row[1]); // type
            PutI32(b, 1148);   // nameId
            PutI32(b, 0);      // upgradeTipsId
            PutI32(b, 0);      // downgradeTipsId
            PutI32(b, row[2]); // range.min
            PutI32(b, row[3]); // range.max
            PutEmptyArray(b);  // actions
            PutF32(b, 0);      // partyProbability
            PutF32(b, 0); PutF32(b, 0); // confessionProbability
            PutF32(b, 0); PutF32(b, 0); // proposeProbability
            PutF32(b, 0); PutF32(b, 0); // birthdayProbability
        }
        const auto rows = mtas::ParseSocialLevelTable(b);
        assert(rows.size() == 2);
        assert(rows[0].name_id == 1148);
        assert(rows[0].social_type == 0 && rows[0].level == 4 && rows[0].range_min == 150 &&
              rows[0].range_max == 400);

        // ComputeNextLevelInfo: Heidi's real mFavor=394 at level 4 -> 400+1-394 = 7 points to go,
        // not maxed (level 5 exists for type 0). A level with no next row (level 5 here, since
        // only 4/5 are in this synthetic table) is maxed.
        const auto info = mtas::ComputeNextLevelInfo(rows, 0, 4, 394);
        assert(info.ok && !info.maxed && info.points_needed == 7);
        const auto maxed = mtas::ComputeNextLevelInfo(rows, 0, 5, 518);
        assert(maxed.ok && maxed.maxed && maxed.points_needed == 0);
        const auto unknown = mtas::ComputeNextLevelInfo(rows, 2, 4, 0); // type 2 not in this table
        assert(!unknown.ok);
    }
    std::puts("TestTextParsers ok");
}

void PutVec3Int(std::vector<std::uint8_t>& b, std::int32_t x, std::int32_t y, std::int32_t z) {
    PutI32(b, x);
    PutI32(b, y);
    PutI32(b, z);
}
void PutVec3IntArray(std::vector<std::uint8_t>& b,
                     const std::vector<std::array<std::int32_t, 3>>& items) {
    PutI32(b, static_cast<std::int32_t>(items.size()));
    for (const auto& v : items)
        PutVec3Int(b, v[0], v[1], v[2]);
}

/// DATA LANE round 3: Ingredients (Cooking/CookingFormula) + Handbook (MachineAtlas/Synthetic)
/// table decoders. Real rows (research/.../tools/parse_cooking.py / parse_blueprint_tables.py,
/// both proven 0-leftover offline against the real designer_config bundle this round):
/// Cooking id=1 -> outItemId=15000004 (Cilantro Omelet) formulaId=1; CookingFormula id=1 ->
/// materials=[16200024,19300011,19300010] (Cilantro/Egg/Sea Salt, ASSET-PATHS.md §6's own
/// verified example); MachineAtlas id=4 -> itemId=19000002, materials=[(19340010,1,1)];
/// Synthetic id=40000188 -> itemId=19010001, itemCount=1, materials=[(19000001,5,1)].
void TestCookingAndHandbookTables() {
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetCookingConfigCooking");
        PutStr(b, "Cooking");
        PutI32(b, 1);
        PutI32(b, 1);          // id
        PutI32(b, 1);          // isActive
        PutI32(b, 15000004);   // outItemId
        PutF32(b, 8.0f);       // costMinutes
        PutI32(b, 10);         // gainExp
        PutI32(b, 1);          // formulaId
        const auto rows = mtas::ParseCookingTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 1 && rows[0].is_active && rows[0].out_item_id == 15000004 &&
              rows[0].formula_id == 1);
    }
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetCookingFormulaConfigCookingFormula");
        PutStr(b, "CookingFormula");
        PutI32(b, 1);
        PutI32(b, 1);  // id
        PutI32(b, 1);  // isActive
        PutI32(b, 0);  // disableTrying
        PutI32(b, 2);  // cookingType
        PutI32(b, 3);  // materials count
        PutI32(b, 16200024); PutI32(b, 19300011); PutI32(b, 19300010);
        const auto rows = mtas::ParseCookingFormulaTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 1 && rows[0].materials.size() == 3);
        assert(rows[0].materials[0] == 16200024 && rows[0].materials[2] == 19300010);
    }
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetMachineAtlasMachineAtlas");
        PutStr(b, "MachineAtlas");
        PutI32(b, 1);
        PutI32(b, 4);           // id
        PutI32(b, 19000002);    // itemId
        PutVec3IntArray(b, {{19340010, 1, 1}}); // materials
        PutI32(b, 11);          // fromMachineType
        PutI32(b, 1);           // fromMachineLevel
        PutF32(b, 135.0f);      // makeTime
        const auto rows = mtas::ParseMachineAtlasTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 4 && rows[0].item_id == 19000002);
        assert(rows[0].materials.size() == 1 && rows[0].materials[0].item_id == 19340010 &&
              rows[0].materials[0].count == 1 && rows[0].materials[0].grade == 1);
    }
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetSyntheticConfigSynthetics");
        PutStr(b, "Synthetics");
        PutI32(b, 1);
        PutI32(b, 40000188);  // id
        PutI32(b, 19010001);  // itemId
        PutI32(b, 1);         // itemCount
        PutVec3IntArray(b, {{19000001, 5, 1}}); // materials
        PutI32(b, 5);         // fromMachineType
        PutI32(b, 1);         // fromMachineLevel
        PutF32(b, 50.0f);     // makeTime
        PutI32(b, 1);         // orderId
        PutI32(b, 13);        // exp
        PutI32(b, 1);         // proficiencyExp
        const auto rows = mtas::ParseSyntheticTable(b);
        assert(rows.size() == 1);
        assert(rows[0].id == 40000188 && rows[0].item_id == 19010001 && rows[0].item_count == 1);
        assert(rows[0].materials.size() == 1 && rows[0].materials[0].item_id == 19000001 &&
              rows[0].materials[0].count == 5);
    }
    // Corrupt/truncated MachineAtlas table: refused whole (same "never publish half a table"
    // convention every other parser in this file already proves).
    {
        std::vector<std::uint8_t> b;
        PutHeader(b);
        PutStr(b, "AssetMachineAtlasMachineAtlas");
        PutStr(b, "MachineAtlas");
        PutI32(b, 2); // declares 2, only 1 follows
        PutI32(b, 4);
        PutI32(b, 19000002);
        PutVec3IntArray(b, {{19340010, 1, 1}});
        PutI32(b, 11); PutI32(b, 1); PutF32(b, 135.0f);
        assert(mtas::ParseMachineAtlasTable(b).empty());
    }
    std::puts("TestCookingAndHandbookTables ok");
}

void TestHandbookCreationJoin() {
    std::vector<std::uint8_t> b;
    PutHeader(b);
    PutStr(b, "AssetCreationConfigCreation");
    PutStr(b, "Creation");
    PutI32(b, 2);
    for (int category = 0; category < 2; ++category) {
        PutI32(b, 10 + category); // creation id differs from output item id
        PutI32(b, 100 + category);
        PutI32(b, 1); // hideInLocking must not hide an unlocked creation
        PutI32(b, 0); PutI32(b, 0); PutF32(b, 1); PutI32(b, 1);
        PutI32(b, 3); PutI32(b, 20); PutI32(b, 21); PutI32(b, 22);
        PutStr(b, "frame");
        PutI32(b, 2); PutStr(b, "left"); PutStr(b, "right");
        PutI32(b, 1); PutI32(b, 0);
        PutI32(b, 900 - category); PutI32(b, 5); PutI32(b, 1);
        PutStr(b, "icon"); PutI32(b, category);
    }
    const auto creations = mtas::ParseCreationTable(b);
    assert(creations.size() == 2);
    assert(creations[0].item_id == 100 && creations[0].priority == 900);
    assert(creations[0].hide_in_locking && creations[0].full_page);
    assert(creations[1].category == 1 && creations[1].part_ids.size() == 3);
    b.pop_back();
    assert(mtas::ParseCreationTable(b).empty());
    b.clear();
    PutHeader(b);
    PutStr(b, "AssetCreationPartConfigCreationPart");
    PutStr(b, "CreationPart");
    PutI32(b, 3);
    for (int i = 0; i < 3; ++i) {
        PutI32(b, 20 + i); PutI32(b, 0);
        PutVec3Int(b, i == 1 ? 201 : 200, i + 1, 1);
        PutI32(b, 0);
    }
    const auto parts = mtas::ParseCreationPartTable(b);
    assert(parts.size() == 3);
    // PERF fix: JoinCreationRecipe now takes id -> row* indices (built once, same as
    // Text::Load's own creation_by_item_/creation_part_by_id_) instead of re-scanning the raw
    // tables on every call.
    std::unordered_map<std::int32_t, const mtas::CreationRow*> creation_by_item;
    for (const auto& row : creations)
        creation_by_item.emplace(row.item_id, &row);
    std::unordered_map<std::int32_t, const mtas::CreationPartRow*> part_by_id;
    for (const auto& row : parts)
        part_by_id.emplace(row.id, &row);
    const auto recipe = mtas::JoinCreationRecipe(100, creation_by_item, part_by_id);
    assert(recipe.ok && recipe.priority == 900 && recipe.category == 0);
    assert(recipe.materials.size() == 2);
    assert(recipe.materials[0].item_id == 200 && recipe.materials[0].count == 4);
    assert(recipe.materials[1].item_id == 201 && recipe.materials[1].count == 2);
    assert(mtas::JoinCreationRecipe(101, creation_by_item, part_by_id).category == 1);
    assert(!mtas::JoinCreationRecipe(10, creation_by_item, part_by_id).ok);
    assert(!mtas::JoinCreationRecipe(999, creation_by_item, part_by_id).ok);
    auto missing_part = parts;
    missing_part.pop_back();
    std::unordered_map<std::int32_t, const mtas::CreationPartRow*> missing_part_by_id;
    for (const auto& row : missing_part)
        missing_part_by_id.emplace(row.id, &row);
    assert(!mtas::JoinCreationRecipe(100, creation_by_item, missing_part_by_id).ok);
    b.pop_back();
    assert(mtas::ParseCreationPartTable(b).empty());

    mtas::Module m;
    m.samples = 1;
    m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    m.model.game_loaded = m.model.handbook.ok = true;
    m.model.handbook.slots.resize(3);
    for (int i = 0; i < 3; ++i) {
        auto& bp = m.model.handbook.slots[i];
        bp.item_name = "Creation " + std::to_string(i);
        bp.category = i == 1 ? 1 : 0;
        bp.fresh = i == 1;
    }
    for (const int category : {0, 1, 2, 0}) {
        assert(mtas::ActionCallback(&m, "set_handbook_category", category));
        Published out;
        mtas::SampleCallback(&m, &out.host);
        out.CheckList("hb.count", "bp", category == 0 ? 3 : category == 1 ? 2 : 1);
        assert(out.ints.at("hb.total") == 3);
        assert(out.ints.at("hb.category1.count") == 2);
        assert(out.ints.at("hb.category2.count") == 1);
        assert(out.ints.at("hb.category" + std::to_string(category) + ".selected") == 1);
        if (category == 2) {
            assert(out.texts.at("bp0.name") == "Creation 1");
            assert(out.ints.at("bp0.fresh") == 1);
        }
    }
    assert(!mtas::ActionCallback(&m, "set_handbook_category", -1));
    assert(!mtas::ActionCallback(&m, "set_handbook_category", 3));
    std::puts("TestHandbookCreationJoin ok (creation parts, materials, categories, full-list filtering)");
}

void TestHandbookLockedSlots() {
    // Table order breaks equal-priority ties; hidden-until-unlocked entries have no slot.
    std::vector<mtas::CreationRow> rows{
        {1, 101, {}, 800, 0, false}, {2, 102, {}, 900, 1, false},
        {3, 103, {}, 800, 1, true}, {4, 104, {}, 800, 0, false},
        {5, 105, {}, 700, 1, true}};
    const auto catalogue = mtas::HandbookCatalogue(rows, {101, 103, 999});
    assert(catalogue.size() == 4);
    assert(catalogue[0].item_id == 102 && catalogue[1].item_id == 101);
    assert(catalogue[2].item_id == 103 && catalogue[3].item_id == 104);
    assert(mtas::HandbookCatalogue(rows, {}).size() == 3);

    const std::vector<mtas::CreationRow> paired{
        {1, 201, {}, 1000, 0, false, false}, {2, 202, {}, 999, 1, false, true},
        {3, 203, {}, 998, 0, false, false}, {4, 204, {}, 997, 1, false, false},
        {5, 205, {}, 996, 1, false, true}, {6, 206, {}, 996, 0, false, true}};
    const auto packed = mtas::HandbookCatalogue(paired, {});
    const std::vector<int> expected{201, 203, 202, 205, 206, 204};
    assert(packed.size() == expected.size());
    for (std::size_t i = 0; i < packed.size(); ++i)
        assert(packed[i].item_id == expected[i]);

    mtas::Module m;
    m.samples = 1;
    m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
    m.model.game_loaded = m.model.handbook.ok = true;
    for (const auto& row : catalogue) {
        mtas::BlueprintFields bp;
        bp.category = row.category;
        bp.unlocked = row.item_id == 101 || row.item_id == 103;
        // Even an accidentally populated locked model must never publish these details.
        bp.item_name = "Hidden name";
        bp.item_icon = "HiddenIcon";
        bp.fresh = true;
        bp.materials.resize(4);
        bp.materials[0].item_name = "Hidden material";
        bp.materials[0].item_icon = "HiddenMaterialIcon";
        m.model.handbook.slots.push_back(bp);
    }
    for (const int category : {0, 1, 2, 0}) {
        assert(mtas::ActionCallback(&m, "set_handbook_category", category));
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        Published out;
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("hb.total") == 4);
        assert(out.ints.at("hb.category1.count") == 2);
        assert(out.ints.at("hb.category2.count") == 2);
        assert(out.ints.at("hb.count") == (category == 0 ? 4 : 2));
        const std::string key = category == 1 ? "bp1." : "bp0.";
        assert(out.ints.at(key + "locked") == 1);
        assert(out.ints.at(key + "unlocked") == 0);
        assert(out.texts.at(key + "name") == "???");
        for (const char* suffix : {"icon_ok", "fresh", "overflow_green", "overflow_red",
                                  "mat0.ok", "mat0.icon_ok", "mat0.need_ok_green",
                                  "mat0.need_ok_red", "mat1.icon_ok", "mat1.need_ok_green",
                                  "mat1.need_ok_red"})
            assert(out.ints.at(key + suffix) == 0);
        for (const char* suffix : {"icon_src", "overflow_text", "mat0.icon_src", "mat0.frac_text"})
            assert(!out.texts.contains(key + suffix));
    }
    // The same slot can become known and unknown again without a spoiler flag surviving.
    for (bool unlocked : {false, true, false}) {
        m.model.handbook.slots[0].unlocked = unlocked;
        m.samples = 1;
        m.rendered_ready = false; // force a fresh render; see Module::rendered's own comment
        Published out;
        mtas::SampleCallback(&m, &out.host);
        assert(out.ints.at("bp0.unlocked") == unlocked);
        assert(out.ints.at("bp0.locked") == !unlocked);
        assert(out.ints.at("bp0.icon_ok") == unlocked);
        assert(out.ints.at("bp0.fresh") == unlocked);
    }
    std::puts("TestHandbookLockedSlots ok (catalogue order, hidden entries, category slots, spoiler gates)");
}

void TestComputeSocialHearts() {
    // Heidi/Arvio/Qi's own live (type,level) pair, all three independently cross-checked this
    // round against the native pause-menu Social tab's pip strip in the same boot (DATA-CONTRACT.md
    // "Social hearts"): mType=0 Normal, mLevel=4 (native label "Friend" per the SocialLevel enum
    // name) -> 3/7 pips filled live, exactly matching the formula.
    {
        const auto h = mtas::ComputeSocialHearts(0, 4);
        assert(!h.stranger && h.hearts_max == 7 && h.hearts == 3);
    }
    // The None/None sentinel (Logan, npc_id 8011, this round's own live example -- the task's own
    // "e.g. Mason" case, a different NPC with the same mType=-1/mLevel=-1 reading): the raw "Lv.
    // -1" bug this round fixes. Must report stranger=true, never a negative pip count. Live-
    // verified native treatment: a single unfilled heart glyph, not a 7-pip strip at 0 -- see
    // SocialHearts' own header comment.
    {
        const auto h = mtas::ComputeSocialHearts(-1, -1);
        assert(h.stranger && h.hearts == 0 && h.hearts_max == 0);
    }
    // The pip-track LENGTH (7) and the +1 floor are both live-verified this round (11 NPCs, one
    // boot, DATA-CONTRACT.md "Social hearts"): NormalStranger=2 already shows one filled pip
    // (native label "Acquaintance", confirmed on 4 different NPCs at this exact level), not "0 of
    // 7"; FriendBest=8 fills every pip (reasoned -- no live NPC was at this exact top tier this
    // round, the highest observed was mLevel=4/3-of-7).
    {
        const auto lo = mtas::ComputeSocialHearts(0, 2);
        assert(!lo.stranger && lo.hearts == 1 && lo.hearts_max == 7);
        const auto hi = mtas::ComputeSocialHearts(0, 8);
        assert(!hi.stranger && hi.hearts == 7 && hi.hearts_max == 7);
    }
    // Pet family (mType=4): 4-level range (PetStranger=21..PetLoyal=24) -- NOT independently
    // live-verified this round (the one Pet-type NPC in the romfs, Fennec Fox, is not in this
    // save's mKnownList -- DATA-CONTRACT.md), kept from the prior pass's reasoning under the same
    // formula proven for the Normal family.
    {
        const auto maxed = mtas::ComputeSocialHearts(4, 24);
        assert(!maxed.stranger && maxed.hearts == 4 && maxed.hearts_max == 4);
        const auto base = mtas::ComputeSocialHearts(4, 21);
        assert(!base.stranger && base.hearts == 1 && base.hearts_max == 4);
    }
    // Lover/Mate/Child families: same 4-level shape, different floor.
    {
        assert(mtas::ComputeSocialHearts(1, 10).hearts == 2); // Lover family, LoverDeep
        assert(mtas::ComputeSocialHearts(2, 13).hearts == 1); // Mate family, MateUnhappy (floor)
        assert(mtas::ComputeSocialHearts(3, 19).hearts == 3); // Child family, ChildClose
    }
    // A genuinely negative Normal standing (Nemesis=0/Adversary=1) clamps to 0 pips, never
    // negative -- display-safe even though no live example of this was observed this pass.
    {
        const auto neg = mtas::ComputeSocialHearts(0, 0);
        assert(!neg.stranger && neg.hearts == 0 && neg.hearts_max == 7);
        const auto neg2 = mtas::ComputeSocialHearts(0, 1);
        assert(!neg2.stranger && neg2.hearts == 0 && neg2.hearts_max == 7);
    }
    // An unrecognised social_type (not one of the 5 known families) reports no pip track rather
    // than guessing.
    {
        const auto unknown = mtas::ComputeSocialHearts(9, 3);
        assert(!unknown.stranger && unknown.hearts == 0 && unknown.hearts_max == 0);
    }
    std::puts("TestComputeSocialHearts ok");
}

void TestDaysUntilBirthday() {
    // Fennec Fox's own proven birthday (DATA-CONTRACT.md "NpcBirthday"): Month 4 (Winter), Day 1
    // -> day_in_year 84. "Today" = day_of_world 100 (TestCalendar's own live timestamp, Day 17
    // Winter Y1): the birthday already passed this year (84 < 100 % 112), so the next occurrence
    // is next year, day_in_year 84 again 112 days later -> 84+112-100 = 96 days away.
    {
        const auto d = mtas::DaysUntilBirthday(100, 4, 1);
        assert(d == 96);
    }
    // A birthday later this same year-window: today=10 (day_in_year 10), birthday Month 1 Day 15
    // (day_in_year 14) -> 4 days away.
    {
        const auto d = mtas::DaysUntilBirthday(10, 1, 15);
        assert(d == 4);
    }
    // Today IS the birthday: 0 days away, not "112 days away" (the task's own "within the next 7
    // days" window must include day 0).
    {
        const auto d = mtas::DaysUntilBirthday(10, 1, 11); // day_in_year 10 == today%112
        assert(d == 0);
    }
    // Year rollover: today near the end of a 112-day year, birthday early next year.
    {
        const auto d = mtas::DaysUntilBirthday(111, 1, 1); // day_in_year 0, today's day_in_year 111
        assert(d == 1);
    }
    // Out-of-range month/day: -1 (unknown), never a garbage day count.
    {
        assert(mtas::DaysUntilBirthday(50, 0, 10) == -1);
        assert(mtas::DaysUntilBirthday(50, 5, 10) == -1);
        assert(mtas::DaysUntilBirthday(50, 2, 0) == -1);
        assert(mtas::DaysUntilBirthday(50, 2, 29) == -1);
    }
    // Always within [0,111] for a valid month/day -- the "within 7 days" UI gate
    // (0 <= days_until_birthday <= 6) can never be satisfied by a stale/wrapped value.
    {
        for (std::int32_t m = 1; m <= 4; ++m)
            for (std::int32_t d = 1; d <= 28; ++d) {
                const auto got = mtas::DaysUntilBirthday(37, m, d);
                assert(got >= 0 && got <= 111);
            }
    }
    std::puts("TestDaysUntilBirthday ok");
}

void TestNpcBirthday() {
    // Fennec Fox (npc id 8071), the task's own worked example: native UI reads "Birthday: Day 1
    // of Winter" (design/CONTENT-SPEC.md). 72576000000000 / TicksPerDay = 84 exactly (0
    // remainder) -> day_of_year 84 -> month 4 (Winter), day 1 -- see mtas_text.h's own comment.
    const auto bday = mtas::DecodeNpcBirthday(72576000000000ULL);
    assert(bday.ok);
    assert(bday.month == 4);
    assert(bday.day == 1);
    std::puts("TestNpcBirthday ok");
}

void TestFindNpcIdByName() {
    // Round 8: the dialogue speaker-name -> npc-id reverse lookup (DATA-CONTRACT.md "Round 8").
    // Synthetic 3-row roster, unsorted name ids on purpose (FindNpcIdByName must not assume loc
    // is npc-id-sorted in the same order, only name-id-sorted, which Text::Load's own invariant
    // guarantees separately).
    const std::vector<mtas::NpcProtoRow> npcs = {
        {8017, 301, 0}, // Mi-an
        {8043, 302, 0}, // Yan
        {8027, 303, 0}, // Mason
    };
    const std::vector<mtas::LocEntry> loc = {
        {301, "Mi-an"},
        {302, "Yan"},
        {303, "Mason"},
    };
    assert(mtas::FindNpcIdByName(npcs, loc, "Yan") == 8043);
    assert(mtas::FindNpcIdByName(npcs, loc, "Mi-an") == 8017);
    assert(mtas::FindNpcIdByName(npcs, loc, "Mason") == 8027);
    // The player's own self-talk name never appears in the NPC roster -- must resolve to -1, not
    // the nearest/first entry (a dialogue auto-context feature silently attaching the player's
    // own monologue to a random NPC card would be worse than showing nothing).
    assert(mtas::FindNpcIdByName(npcs, loc, "Max") == -1);
    assert(mtas::FindNpcIdByName(npcs, loc, "") == -1);
    assert(mtas::FindNpcIdByName({}, {}, "Yan") == -1);
    std::puts("TestFindNpcIdByName ok");
}

} // namespace

void TestPregameNotes() {
    for (const bool loaded : {false, true}) {
        for (const bool machines : {false, true}) {
            mtas::Module m;
            Published out;
            m.model.game_loaded = loaded;
            m.model.machines.ok = machines;
            m.model.missions.ok = m.model.bag.ok = m.model.social.ok = true;
            m.model.ingredients.ok = m.model.handbook.ok = m.model.workshop.ok = true;
            m.model.inventory.ok = m.model.player.ok = true;
            m.model.workshop.rank_display = 3;
            m.samples = 1;
            mtas::SampleCallback(&m, &out.host);
            assert(out.ints.at("today.show_fallback_note") == (loaded && !machines));
            assert(out.ints.at("machines.fuel_lack_good") == (loaded && machines));
            assert(out.ints.at("machines.water_lack_good") == (loaded && machines));
            assert(out.texts.count("machines.fuel_text") == (loaded && machines));
            for (const auto* key : {"ws.ok", "ws.rank_ok", "hb.ok", "ing.ok",
                                    "inv.ok", "player.ok", "social.ok"})
                assert(out.ints.at(key) == loaded);
            for (const auto* key : {"ws.rank_text", "ing.can_cook_text", "hb.category0.text",
                                    "inv.bag_text", "player.level_text", "bag.gold_text"})
                assert(out.texts.count(key) == loaded);
            assert(out.ints.count("missions.count") == loaded);
        }
    }
    std::puts("TestPregameNotes ok (all page availability and data gates before and after load)");
}

void TestReturnToTitle() {
    for (const int state : {0, 2}) {
        mtas::Module m;
        m.model.game_loaded = m.model.ui.ok = m.model.time.ok = m.model.time.calendar_ok = true;
        m.model.time.day = 18;
        m.model.ui.state_type = 0;
        m.model.inventory.ok = m.model.ingredients.ok = m.model.handbook.ok = true;
        m.model.workshop.ok = m.model.player.ok = m.model.social.ok = true;
        m.model.workshop.rank_display = 3;
        m.saw_loading = true;
        auto live = m.model;
        mtas::AcceptFreshModel(m, live, false);
        assert(live.game_loaded && m.held_ok);
        auto pause = live;
        pause.ui.state_type = 6;
        mtas::AcceptFreshModel(m, pause, false);
        assert(pause.game_loaded && m.held_ok); // native pause is not the title
        Published before;
        m.samples = 1;
        mtas::SampleCallback(&m, &before.host);
        assert(before.ints.at("ws.ok") == 1);
        auto title = live;
        title.ui.state_type = state;
        if (state == 0) title.game_loaded = false;
        mtas::AcceptFreshModel(m, title, false);
        assert(!m.held_ok && !m.saw_loading && m.live_refreshes == 0);
        assert(!m.model.game_loaded && !m.model.workshop.ok);
        assert(!title.game_loaded && !title.workshop.ok);
        m.model = title;
        Published after;
        m.samples = 1;
        mtas::SampleCallback(&m, &after.host);
        for (const auto* key : {"mtas.game_loaded", "mtas.held_ok", "ws.ok", "ws.rank_ok",
                                "hb.ok", "ing.ok", "inv.ok", "player.ok", "social.ok", "map.ok"})
            assert(after.ints.at(key) == 0);
        for (const auto* key : {"ws.rank_text", "ing.can_cook_text", "hb.category0.text",
                                "inv.bag_text", "player.level_text", "missions.header_text"})
            assert(after.texts.count(key) == 0);
        // Title singletons cannot reuse the preceding witnessed load; a fresh attach must qualify.
        for (int i = 0; i < 3; ++i) {
            auto again = live;
            mtas::AcceptFreshModel(m, again, false);
            assert(again.game_loaded == (i == 2));
        }
        assert(m.held_ok);
    }
    std::puts("TestReturnToTitle ok (native pause retained, title or missing bag blank, load again)");
}

void TestScrollingSelection() {
    mtas::Module m;
    m.model.game_loaded = true;
    m.model.social.ok = true;
    for (int i = 0; i < 80; ++i) {
        mtas::NpcFields npc;
        npc.name = "Person " + std::to_string(i);
        m.model.social.npcs.push_back(npc);
    }
    m.samples = 1;
    Published out;
    mtas::SampleCallback(&m, &out.host);
    assert(out.ints.at("social.npc_count") == 80);
    assert(out.texts.at("npc79.name") == "Person 79");
    assert(!out.texts.count("npc.page_text"));
    assert(mtas::ActionCallback(&m, "select_npc", 79));
    assert(m.npc_sel == 79);
    assert(!mtas::ActionCallback(&m, "npc_page", 1));
    assert(mtas::ActionCallback(&m, "select_item", 100));
    assert(m.item_sel == 100);
    assert(mtas::ActionCallback(&m, "set_source", 1));
    assert(m.item_sel == -1);
    std::puts("TestScrollingSelection ok (full list, absolute selection, filter reset)");
}

void TestNativeMarkerFogGate() {
    mtas::Module m;
    m.model.game_loaded = true; m.map.ok = true; m.map.scene = 3; m.map.radius = 1612;
    m.map.markers.push_back({"I_Map_icon_Npc_00", 0, 0, 0, 0, 0, false});
    Published unknown; mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &unknown.host);
    assert(unknown.ints.at("map.marker_count") == 0);
    assert(!unknown.floats.count("map.icon0.x"));
    m.map.markers.push_back({"I_Map_icon_Mark_00", 0, 0, 0, 0, 0, false});
    Published own_pin; mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &own_pin.host);
    assert(own_pin.ints.at("map.marker_count") == 1); // player-authored native pin is already known
    m.map.markers.pop_back();
    m.marker_fog.emplace(); m.marker_fog->width = m.marker_fog->height = 256;
    m.marker_fog->rgba.assign(256 * 256 * 4, 0);
    m.marker_fog->rgba[(128 * 256 + 128) * 4] = 250;
    Published revealed; mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &revealed.host);
    assert(revealed.ints.at("map.marker_count") == 1);
    assert(revealed.floats.at("map.icon0.x") == 2048 && revealed.floats.at("map.icon0.y") == 2048);
    // A moving actor is available before the next fog-stamp history read.
    m.map.actor_ok = m.map.facing_ok = true;
    m.map.actor_u = 0.6f; m.map.actor_v = 0.4f; m.map.facing_yaw = 90;
    m.map_history.clear();
    Published moving; mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &moving.host);
    assert(moving.ints.at("map.player_ok") == 1);
    assert(std::abs(moving.floats.at("map.px") - 0.6 * mtas::MapWorldUnits) < 0.001);
    assert(moving.floats.at("map.facing_yaw") == 90);
    m.map.camera_ok = true;
    m.map.minimap_scale = 1.5f;
    Published view;
    view.host.get_f64 = [](void* p, const char* key, double fallback) {
        const auto& values = static_cast<Published*>(p)->floats;
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    };
    view.floats["@map_view_cx:sandrock"] = m.map.actor_u * mtas::MapWorldUnits;
    view.floats["@map_view_cy:sandrock"] = m.map.actor_v * mtas::MapWorldUnits;
    view.floats["@map_view_ppw:sandrock"] = 2;
    for (const char* orientation : {"landscape", "portrait", "landscape"}) {
        view.orientation = orientation;
        const bool portrait = std::strcmp(orientation, "portrait") == 0;
        const double w = portrait ? 1080 : 1240, h = portrait ? 1496 : 956;
        mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &view.host);
        assert(view.floats.at("map.screen_x") == w / 2);
        assert(view.floats.at("map.screen_y") == h / 2);
        assert(view.ints.at("map.cone_visible") == 1);
        const double ppw = view.floats.at("map.default_ppw");
        assert(std::abs((view.floats.at("map.vx1") - view.floats.at("map.vx0")) * ppw - w) < 0.001);
        assert(std::abs((view.floats.at("map.vy1") - view.floats.at("map.vy0")) * ppw - h) < 0.001);
    }
    m.map.scene = 5;
    Published indoors; mtas::CacheMapMarkerSources(m); mtas::PublishMap(m, &indoors.host);
    assert(indoors.ints.at("map.marker_count") == 0);
    assert(indoors.ints.at("map.indoors") == 1);
    std::puts("TestNativeMarkerFogGate ok (unknown, point reveal, native projection, indoors)");
}

// Fake asynchronous texture service: discovery, pinning, unchanged work, errors and reset.
void TestLiveFog() {
    using namespace mtas;
    struct Service {
        EdenDsmodTextureRequest request{};
        int requests{}, cancels{};
        int status{EDEN_DSMOD_TEXTURE_READY};
        int refuse{}; // request status when non-zero (e.g. INVALID from a host without flags)
        EdenDsmodTextureInfo info{sizeof(info), EDEN_DSMOD_TEXTURE_READY, 0x123400,
                                 256, 256, EDEN_DSMOD_TEXTURE_RGBA8, 1, 7, 256 * 256 * 4};
        std::array<std::uint8_t, 256 * 256 * 4> pixels{};
    } f;
    EdenDsmodHostTextureApi api{EDEN_DSMOD_TEXTURE_EXT_VERSION, sizeof(api),
        EDEN_DSMOD_TEXTURE_EXT_HASH, &f,
        [](void* p, const EdenDsmodTextureRequest* r, std::int32_t* status) -> std::uint64_t {
            auto& f = *static_cast<Service*>(p); f.request = *r; ++f.requests;
            if (f.refuse) { *status = f.refuse; return 0; }
            *status = EDEN_DSMOD_TEXTURE_PENDING; return f.requests;
        },
        [](void* p, std::uint64_t, void* out, std::size_t, EdenDsmodTextureInfo* info) -> std::int32_t {
            auto& f = *static_cast<Service*>(p); *info = f.info;
            std::memcpy(out, f.pixels.data(), f.pixels.size()); return f.status;
        },
        [](void* p, std::uint64_t) { ++static_cast<Service*>(p)->cancels; }};
    // The same manifest retains the old tile path when configure is never called.
    Module fallback;
    Published published;
    fallback.model.game_loaded = true;
    fallback.map.ok = true;
    fallback.map_fog.Reveal(0);
    CacheMapMarkerSources(fallback); PublishMap(fallback, &published.host);
    assert(published.texts["map.t0"] == MapTileKey(0, MapTilePx));
    assert(published.texts["map.fog_mask"] == "module:mtas:fog:closed");
    fallback.live_fog.Configure(&api);
    published.texts.clear();
    CacheMapMarkerSources(fallback); PublishMap(fallback, &published.host);
    assert(!published.texts.contains("map.t0")); // configured but absent => fully fogged
    assert(published.texts["map.fog_mask"] == "module:mtas:fog:closed");
    assert(!published.texts.contains("map.m0"));
    LiveFog fog;
    const auto t = LiveFog::Clock::now();
    fog.Refresh(true, t);
    assert(!fog.Configured() && !fog.Ready() && f.requests == 0); // old host
    fog.Configure(&api);
    fog.Refresh(true, t);
    assert(f.request.gpu_address == 0 && f.request.width == 256);
    assert(f.request.flags == LiveFog::kSelector && f.request.flags == 0xBu && f.request.reserved == 0);
    f.status = EDEN_DSMOD_TEXTURE_PENDING;
    fog.Refresh(true, t);
    assert(!fog.Ready());
    f.status = EDEN_DSMOD_TEXTURE_READY;
    f.pixels[0] = 255; f.pixels[4] = 128;
    fog.SetExplored(std::vector<std::uint8_t>(256 * 256, 255)); // everything explored: AND = readback
    fog.Refresh(true, t);
    assert(fog.Ready());
    assert(fog.Image().rgba[(255 * 256) * 4 + 3] == 0); // rows flip
    assert(fog.Image().rgba[(255 * 256 + 1) * 4 + 3] == 200); // sRGB != 128
    assert(fog.Image().rgba[3] == 255);
    const auto key = fog.Key();
    fog.Refresh(true, t + std::chrono::seconds(1));
    assert(f.request.gpu_address == 0x123400 && f.requests == 2);
    assert(f.request.flags == LiveFog::kSelector); // the pinned address keeps the selector
    fog.Refresh(true, t + std::chrono::seconds(1));
    assert(fog.Key() == key); // unchanged generation does not rebuild
    fog.Refresh(true, t + std::chrono::seconds(2));
    f.status = EDEN_DSMOD_TEXTURE_NOT_FOUND;
    fog.Refresh(true, t + std::chrono::seconds(2));
    assert(!fog.Ready());
    fog.Refresh(true, t + std::chrono::seconds(3));
    assert(f.request.gpu_address == 0);
    f.status = EDEN_DSMOD_TEXTURE_READY; f.info.candidates = 2;
    fog.Refresh(true, t + std::chrono::seconds(3));
    assert(!fog.Ready()); // ambiguous size match fails closed
    fog.Refresh(true, t + std::chrono::seconds(4));
    f.info.candidates = 1;
    fog.Refresh(true, t + std::chrono::seconds(4));
    assert(fog.Ready() && fog.Key() != key); // reused generation after reset gets new key
    fog.Refresh(false, t + std::chrono::seconds(4));
    assert(!fog.Ready()); // title / loading cannot retain the prior save
    fog.Refresh(true, t + std::chrono::seconds(5));
    f.status = EDEN_DSMOD_TEXTURE_MISMATCH;
    fog.Refresh(true, t + std::chrono::seconds(5));
    assert(!fog.Ready());
    fog.Refresh(true, t + std::chrono::seconds(6));
    assert(f.request.gpu_address == 0);
    fog.Refresh(false, t + std::chrono::seconds(6));
    assert(f.cancels == 1 && fog.Info().candidates == 0);
    // A host without selector flags refuses the request: fully fogged, no ticket, no address.
    LiveFog old_rt24;
    old_rt24.Configure(&api);
    f.refuse = EDEN_DSMOD_TEXTURE_INVALID;
    old_rt24.Refresh(true, t + std::chrono::seconds(7));
    assert(!old_rt24.Ready() && old_rt24.Info().status == EDEN_DSMOD_TEXTURE_INVALID);
    old_rt24.Refresh(true, t + std::chrono::seconds(8));
    assert(!old_rt24.Ready() && f.request.gpu_address == 0);
    f.refuse = 0;
}

// The live reveal is the readback AND the explored record, split into per-tile masks.
void TestLiveFogGateAndTiles() {
    using namespace mtas;
    struct Service {
        std::uint64_t generation{1};
        std::array<std::uint8_t, 256 * 256 * 4> pixels{};
    } f;
    EdenDsmodHostTextureApi api{EDEN_DSMOD_TEXTURE_EXT_VERSION, sizeof(api),
        EDEN_DSMOD_TEXTURE_EXT_HASH, &f,
        [](void*, const EdenDsmodTextureRequest*, std::int32_t* status) -> std::uint64_t {
            *status = EDEN_DSMOD_TEXTURE_PENDING; return 1;
        },
        [](void* p, std::uint64_t, void* out, std::size_t, EdenDsmodTextureInfo* info) -> std::int32_t {
            auto& f = *static_cast<Service*>(p);
            *info = {sizeof(*info), EDEN_DSMOD_TEXTURE_READY, 0x5108c0000, 256, 256,
                     EDEN_DSMOD_TEXTURE_RGBA8, 1, f.generation, 256 * 256 * 4};
            std::memcpy(out, f.pixels.data(), f.pixels.size()); return EDEN_DSMOD_TEXTURE_READY;
        },
        [](void*, std::uint64_t) {}};
    // RT red (bottom-up rows): stripes in every odd column outside x 48..127 + a solid revealed
    // block at image rows 116..155 (RT rows 100..139), columns 64..111.
    const auto red = [&](int x, int image_row) -> std::uint8_t& { return f.pixels[((255 - image_row) * 256 + x) * 4]; };
    for (int y = 0; y < 256; ++y) for (int x = 1; x < 256; x += 2) if (x < 48 || x >= 128) red(x, y) = 255;
    for (int y = 116; y < 156; ++y) for (int x = 64; x < 112; ++x) red(x, y) = 255;
    constexpr int stripes = 88 * 256, block = 40 * 48;
    LiveFog fog;
    fog.Configure(&api);
    auto t = LiveFog::Clock::now();
    fog.Refresh(true, t);
    fog.Refresh(true, t + std::chrono::milliseconds(1));
    assert(fog.Ready() && !fog.HasExplored());
    // No record yet: the block passes the readback's own noise test (a clean shape on cleared
    // tiles) and shows exactly; every stripe cell is noise and stays fogged.
    assert(fog.Gated() == stripes);
    const auto decoded = LiveFog::Decode(f.pixels.data());
    const auto block_only = [&](const ce_unity::Image& image) {
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const int i = y * 256 + x;
                const bool in_block = y >= 116 && y < 156 && x >= 64 && x < 112;
                if (image.rgba[i * 4 + 3] < decoded.rgba[i * 4 + 3]) return false; // never more than native
                if (image.rgba[i * 4 + 3] != (in_block ? decoded.rgba[i * 4 + 3] : 255)) return false;
            }
        return true;
    };
    assert(block_only(fog.Image()));
    // The explored record (stamps): a 16 x 16 patch of stripes at image rows 0..15, x 160..175 is
    // explored ground; those stripe cells show, the rest stays fogged.
    std::vector<std::uint8_t> explored(256 * 256, 0);
    for (int y = 0; y < 16; ++y) for (int x = 160; x < 176; ++x) explored[y * 256 + x] = 255;
    const auto key0 = fog.Key();
    fog.SetExplored(explored);
    assert(fog.HasExplored() && fog.Key() != key0);
    assert(fog.Gated() == stripes - 16 * 8);
    for (int i = 0; i < 256 * 256; ++i) {
        const int y = i / 256, x = i % 256;
        const bool in_block = y >= 116 && y < 156 && x >= 64 && x < 112;
        assert(fog.Image().rgba[i * 4 + 3] >= decoded.rgba[i * 4 + 3]);
        assert(fog.Image().rgba[i * 4 + 3] == (in_block || explored[i] ? decoded.rgba[i * 4 + 3] : 255));
    }
    fog.SetExplored({});
    assert(block_only(fog.Image()));
    // Markers follow the shown reveal: inside the block yes, on a stripe (noise) no.
    assert(fog.RevealAt((80 + 0.5f) / 256, 1 - (130 + 0.5f) / 256) == 255);
    assert(fog.RevealAt((161 + 0.5f) / 256, 1 - (10 + 0.5f) / 256) == 0);
    assert(fog.RevealAt(1.0f, 0.5f) == 0 && fog.RevealAt(0.5f, -0.1f) == 0);
    // Tiles: interior block tile (row 8, col 5) fully revealed -> drawn unmasked; tiles touching
    // the block's edge -> masked; far tiles hidden.
    assert(fog.Tile(8 * 16 + 5) == LiveFog::TileState::Full && fog.TileMaskKey(8 * 16 + 5).empty());
    assert(fog.Tile(7 * 16 + 5) == LiveFog::TileState::Masked);
    assert(fog.Tile(0) == LiveFog::TileState::Hidden && fog.Tile(8 * 16 + 9) == LiveFog::TileState::Hidden);
    const int tile = 7 * 16 + 5;
    const auto mkey = fog.TileMaskKey(tile);
    assert(mkey.starts_with("module:mtas:fog:") && fog.Find(mkey) == &fog.TileMask(tile));
    assert(fog.Find(fog.Key()) == &fog.Image());
    assert(!fog.Find(mkey + "0") && !fog.Find("module:mtas:fog:1:x#1") && !fog.Find("module:mtas:fog:closed"));
    // Mask geometry: tile row 7 spans image rows 112..127, block starts at 116 = mask row 8
    // (2 px a cell): rows 0..6 fogged, 9.. revealed, the edge in between bilinear.
    const auto& m = fog.TileMask(tile);
    assert(m.width == LiveFog::TileMaskPx && m.height == LiveFog::TileMaskPx);
    assert(m.rgba[(5 * 32 + 10) * 4 + 3] == 0 && m.rgba[(10 * 32 + 10) * 4 + 3] == 255);
    assert(m.rgba[(7 * 32 + 10) * 4 + 3] > 0 && m.rgba[(7 * 32 + 10) * 4 + 3] < 255);
    // Seams: the last mask row of tile row 7 continues into the first of tile row 8 (both inside
    // the block here), and a horizontal ramp crop meets its neighbour within one ramp step.
    std::vector<std::uint8_t> ramp(256 * 256);
    for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) ramp[y * 256 + x] = static_cast<std::uint8_t>(x);
    const auto a = LiveFog::TileCrop(ramp, 3), b = LiveFog::TileCrop(ramp, 4);
    assert(a.size() == 32 * 32 && b[0] >= a[31] && b[0] - a[31] <= 1 && a[0] == 47 && b[31] == 79);
    assert(LiveFog::TileCrop(ramp, 256).size() == 32 * 32 && LiveFog::TileCrop({}, 0)[0] == 0);
    // An unchanged readback with a new generation keeps every key; a change inside one tile
    // changes only that tile's key (and the fog-art mask's).
    const auto fog_key = fog.Key(), other = fog.TileMaskKey(9 * 16 + 5);
    f.generation = 2;
    fog.Refresh(true, t + std::chrono::seconds(1));
    fog.Refresh(true, t + std::chrono::seconds(1));
    assert(fog.Generation() == 2 && fog.Key() == fog_key && fog.TileMaskKey(tile) == mkey);
    red(80, 120) = 0; // inside tile (7,5), explored, previously revealed
    f.generation = 3;
    fog.Refresh(true, t + std::chrono::seconds(2));
    fog.Refresh(true, t + std::chrono::seconds(2));
    assert(fog.Key() != fog_key && fog.TileMaskKey(tile) != mkey && fog.TileMaskKey(9 * 16 + 5) == other);
    assert(!fog.Find(mkey)); // a stale key no longer loads
    // Publishing: drawn tiles carry picture + detail + their mask point ("" = unmasked).
    Module module;
    module.model.game_loaded = true; module.map.ok = true; module.map.scene = MapSceneMain;
    module.live_fog = fog;
    Published shown; CacheMapMarkerSources(module); PublishMap(module, &shown.host);
    assert(shown.texts.at("map.t" + std::to_string(8 * 16 + 5)) == MapTileKey(8 * 16 + 5, MapTilePx));
    assert(shown.texts.at("map.d" + std::to_string(8 * 16 + 5)) == MapTileKey(8 * 16 + 5, MapDetailPx));
    assert(shown.texts.at("map.m" + std::to_string(8 * 16 + 5)).empty());
    assert(shown.texts.at("map.m" + std::to_string(tile)) == fog.TileMaskKey(tile));
    assert(!shown.texts.contains("map.t0") && shown.ints.at("map.fog_gated") == fog.Gated());
    // A native marker on the revealed block is shown, one on noise is not (no save, no stamps).
    module.map.radius = 2048; module.map.center_x = module.map.center_z = 2048;
    module.map.markers.push_back({"I_Map_icon_Npc_00", 80.5f * 16, 0, (256 - 130.5f) * 16, 0, 0, false});
    module.map.markers.push_back({"I_Map_icon_Npc_00", 161.5f * 16, 0, (256 - 10.5f) * 16, 0, 0, false});
    Published markers; CacheMapMarkerSources(module); PublishMap(module, &markers.host);
    assert(markers.ints.at("map.marker_count") == 1 && markers.floats.at("map.icon0.x") == 80.5 * 16);
    module.map.markers.clear();
    // Closing the readback takes every picture away again.
    module.live_fog.Reset();
    Published hidden; CacheMapMarkerSources(module); PublishMap(module, &hidden.host);
    assert(hidden.texts.at("map.t" + std::to_string(8 * 16 + 5)).empty() && !hidden.texts.contains("map.t0"));
    std::puts("TestLiveFogGateAndTiles ok (noise gate, record AND, never more than native, tile states, masks, seams, keys, publish)");
}

void TestNativeMenuSocialAndUses() {
    mtas::Module m;
    m.samples = 1;
    m.model.game_loaded = true;
    auto& social = m.model.social;
    social.ok = true;
    social.met_count = 4;
    for (int i = 0; i < 4; ++i) {
        mtas::NpcFields n;
        n.npc_id = 100 + i;
        n.name = "Met " + std::to_string(i);
        n.gifted_ok = n.chatted_ok = true;
        n.hearts = 4 - i;
        n.hearts_max = 7;
        n.social_type = i == 3 ? 4 : 0;
        n.level_title = "Native title";
        social.npcs.push_back(n);
    }
    social.npcs[0].gifted_today = true;
    social.npcs[2].days_until_birthday = 5;
    social.npcs[3].days_until_birthday = 1;
    social.npcs[0].known_likes = {101};
    social.npcs[2].known_likes = {101, 102};
    social.npcs[2].excellent_likes = {101};
    assert((mtas::SocialOrder(social, 0) == std::vector<std::size_t>{3, 2, 1, 0}));
    assert((mtas::SocialOrder(social, 1) == std::vector<std::size_t>{3, 2, 1}));
    assert((mtas::SocialOrder(social, 3) == std::vector<std::size_t>{3}));
    assert(mtas::SocialOrder(social, 2).empty());
    assert(mtas::ActionCallback(&m, "select_npc", 1));
    assert(m.npc_sel == 2);
    auto& inv = m.model.inventory;
    inv.ok = inv.storage_ok = true;
    mtas::InvItem item;
    item.item_id = 101; item.count = 3; item.name = "Material";
    inv.items.push_back(item);
    item.count = 8; item.in_storage = true; inv.items.push_back(item);
    m.item_sel = 0;
    auto& tracked = m.model.tracked;
    tracked.ok = tracked.item.ok = tracked.item.have_ok = true;
    tracked.title = "Accepted order";
    tracked.item.item_id = 101; tracked.item.need = 6; tracked.item.have = 11;
    mtas::BlueprintFields bp;
    bp.item_name = "Unlocked blueprint";
    bp.materials.push_back({101, "Material", "", 12, 11, false});
    m.model.handbook.slots.push_back(bp);
    bp.unlocked = false; bp.item_name = "Hidden"; m.model.handbook.slots.push_back(bp);
    assert(mtas::NeededFor(m.model, 101).size() == 2);
    assert(mtas::NeededFor(m.model, 999).empty());
    Published out;
    mtas::SampleCallback(&m, &out.host);
    assert(out.texts.at("social.gift_header").ends_with("3 / 4"));
    assert(out.texts.at("npc0.id") == "103");
    assert(out.ints.at("npc1.likes_count") == 2);
    assert(out.texts.at("npc_detail.level_title") == "Native title");
    assert(out.texts.at("npc_like0.owned") == "11");
    assert(out.ints.at("npc_like2.unknown") == 1);
    assert(out.texts.at("npc_like0.tier_src").find("Verylike") != std::string::npos);
    assert(out.ints.at("item_detail.liked_count") == 2);
    assert(out.ints.at("item_detail.needed_count") == 2);
    assert(out.texts.at("item_needed0.count") == "11/6");
    assert(out.texts.at("item_needed1.count") == "11/12");
    tracked.item.have = 5;
    Published quality_progress;
    mtas::SampleCallback(&m, &quality_progress.host);
    assert(quality_progress.texts.at("item_needed0.count") == "5/6");
    assert(quality_progress.ints.at("item_needed0.short") == 1);
    assert(quality_progress.ints.at("item_needed0.enough") == 0);
    tracked.item.have_ok = false;
    assert(mtas::NeededFor(m.model, 101).size() == 1);
    inv.ok = false;
    Published missing_inventory;
    mtas::SampleCallback(&m, &missing_inventory.host);
    assert(missing_inventory.ints.at("npc_like0.owned_ok") == 0);
    assert(missing_inventory.ints.at("npc_like0.none_owned") == 0);
    m.model.game_loaded = false;
    Published title;
    mtas::SampleCallback(&m, &title.host);
    assert(title.ints.at("social.ok") == 0);
    assert(!title.texts.contains("social.gift_header"));
    std::puts("TestNativeMenuSocialAndUses ok (sort, met-only denominator, filters, per-NPC discovery, title, owned counts, reverse joins, locked exclusion)");
}

int main() {
    TestNativeMenuSocialAndUses();
    TestLiveFogGateAndTiles();
    TestLiveFog();
    TestNativeMarkerFogGate();
    TestScrollingSelection();
    TestPregameNotes();
    TestReturnToTitle();
    TestLanguageMapping();
    TestRomfsLanguages();
    TestLoadingHold();
    TestMidGameAttach();
    TestFilterSelectionIsolation();
    TestSettings();
    TestMissionContext();
    TestSleepContext();
    TestHudSettingsLease();
    TestMissionObjectives();
    TestInventoryCategories();
    TestDayEventNames();
    TestPortraitImageSources();
    TestWorkshopRankBadges();
    TestHandbookFresh();
    TestMaterialVisibility();
    TestFullListPublishing();
    TestFormatCallsCached();
    TestDialogueReturnPage();
    TestCalendar();
    TestNineSlice();
    TestUiPins();
    TestSpriteIndex();
    TestSpriteLru();
    TestFontGuards();
    TestImageRequestGuards();
    TestMissionTimeLeft();
    TestDailyReaders();
    TestMissionReaderDeadlines();
    TestRootCaching();
    TestComputeMissionClass();
    TestMergeHaveCounts();
    TestMergeStorageItems();
    TestTextParsers();
    TestCookingAndHandbookTables();
    TestHandbookCreationJoin();
    TestHandbookLockedSlots();
    TestComputeSocialHearts();
    TestDaysUntilBirthday();
    TestNpcBirthday();
    TestFindNpcIdByName();
    std::puts("mtas_publish_test: all ok");
    return 0;
}
