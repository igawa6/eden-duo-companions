// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fee_assets.h"

#include <algorithm>
#include <cstdio>

namespace fee {
namespace {

constexpr std::string_view kFaceBundle =
    "fe_assets_ui/common/unitlist/facethumb/facethumb.bundle";
constexpr std::size_t kFaceCache = 24;
constexpr std::int32_t kClassTextAsset = 49;

bool ReadTextAsset(lp_unity::Reader& unity, const std::string& bundle, const char* name,
                   fee_text::Table& out) {
    const auto value = unity.ReadObjectByName(bundle, name, kClassTextAsset);
    if (!value)
        return false;
    const lp_unity::Value* script = value->Get("m_Script");
    if (!script || script->s.empty())
        return false;
    return fee_text::ParseMsbt(reinterpret_cast<const std::uint8_t*>(script->s.data()),
                               script->s.size(), out);
}

} // namespace

lp_unity::RangeReader MakeRomfsReader(const EdenDsmodHostApi& api) {
    const auto fn = api.read_romfs;
    void* ud = api.userdata;
    const auto full = [](std::string_view p) {
        if (p.starts_with("base:") || p.starts_with("romfs:"))
            return std::string{p};
        return "romfs:" + std::string{p};
    };
    lp_unity::RangeReader r;
    r.size = [fn, ud, full](std::string_view p) -> std::optional<std::uint64_t> {
        if (!fn)
            return std::nullopt;
        const auto path = full(p);
        const std::size_t n = fn(ud, path.c_str(), 0, nullptr, 0);
        return n ? std::optional<std::uint64_t>{n} : std::nullopt;
    };
    r.read = [fn, ud, full](std::string_view p, std::uint64_t off, std::uint8_t* out,
                            std::size_t n) {
        if (!fn)
            return false;
        const auto path = full(p);
        return fn(ud, path.c_str(), off, out, n) >= n;
    };
    return r;
}

Assets::Assets(lp_unity::RangeReader read, std::string asset_root)
    : unity_{std::move(read), {}, std::move(asset_root), 4},
      sprites_{std::make_unique<Sprites>(unity_)} {}

Assets::~Assets() {
    Stop();
}

void Assets::Stop() {
    stopping_.store(true, std::memory_order_relaxed);
    std::scoped_lock lock{worker_mutex_};
    if (worker_.joinable())
        worker_.join();
}

void Assets::EnsureLanguage(int lang) {
    if (fee_text::LocaleDir(lang)[0] == '\0')
        return;
    if (stopping_.load(std::memory_order_relaxed))
        return;
    const int current = wanted_lang_.load(std::memory_order_acquire);
    if (current == lang && (loading_.load(std::memory_order_acquire) ||
                            failed_lang_.load(std::memory_order_acquire) == lang ||
                            (loaded_lang_.load(std::memory_order_acquire) == lang &&
                             text_ready_.load(std::memory_order_acquire))))
        return; // loading, loaded, or known unreadable (no reload loop over a missing bundle)
    std::scoped_lock lock{worker_mutex_};
    // Another language while a load runs: wait for the next sample instead of blocking the
    // sample thread on the join.
    if (loading_.load(std::memory_order_acquire))
        return;
    if (worker_.joinable())
        worker_.join(); // finished: loading_ is false
    wanted_lang_.store(lang, std::memory_order_release);
    // Set before the thread starts, so the next sample never sees "not loading, not loaded"
    // and spawns (and joins) a second load of the same tables.
    loading_.store(true, std::memory_order_release);
    worker_ = std::thread{[this, lang] { Work(lang); }};
}

void Assets::Work(int lang) {
    bool ok = false;
    try {
        ok = LoadText(lang);
    } catch (...) {
        ok = false;
    }
    if (ok) {
        loaded_lang_.store(lang, std::memory_order_release);
        text_ready_.store(true, std::memory_order_release);
    } else {
        failed_lang_.store(lang, std::memory_order_release);
    }
    loading_.store(false, std::memory_order_release);
}

bool Assets::LoadText(int lang) {
    const char* locale = fee_text::LocaleDir(lang);
    if (locale[0] == '\0')
        return false;
    const std::string dir = std::string{"fe_assets_message/"} + locale + "/";
    fee_text::Table person;
    if (!ReadTextAsset(unity_, dir + "person.bytes.bundle", "Person", person))
        return false;
    fee_text::Table job;
    if (ReadTextAsset(unity_, dir + "job.bytes.bundle", "Job", job)) {
        for (auto& [key, value] : job.text)
            person.text.insert_or_assign(std::move(key), std::move(value));
    }
    // Item / skill / terrain names (MIID_, MSID_, MTID_) for the unit details.
    for (const auto& [bundle, asset] : {std::pair{"item.bytes.bundle", "Item"},
                                        std::pair{"skill.bytes.bundle", "Skill"},
                                        std::pair{"gamedata.bytes.bundle", "GameData"},
                                        std::pair{"battlestyle.bytes.bundle", "BattleStyle"},
                                        // Somniel places (MID_Hub_*) and DLC chapter names
                                        // (Fell Xenologue MCID_E*) for fee_hub.
                                        std::pair{"hub.bytes.bundle", "Hub"},
                                        // DLC Emblems / classes / Phantom units (Patch0-2: Wave
                                        // 1-3 names, MPID_/MGID_/MJID_ keys absent elsewhere).
                                        std::pair{"patch0.bytes.bundle", "Patch0"},
                                        std::pair{"patch1.bytes.bundle", "Patch1"},
                                        std::pair{"patch2.bytes.bundle", "Patch2"},
                                        std::pair{"patch3.bytes.bundle", "Patch3"}}) {
        fee_text::Table extra;
        if (ReadTextAsset(unity_, dir + bundle, asset, extra))
            for (auto& [key, value] : extra.text)
                person.text.insert_or_assign(std::move(key), std::move(value));
    }
    auto table = std::make_shared<const fee_text::Table>(std::move(person));
    {
        std::scoped_lock lock{text_mutex_};
        text_ = std::move(table);
    }
    return true;
}

std::string Assets::Text(std::string_view key) const {
    std::shared_ptr<const fee_text::Table> table;
    std::string hero;
    {
        std::scoped_lock lock{text_mutex_};
        table = text_;
        hero = hero_name_;
    }
    if (!table)
        return {};
    if (key == "MPID_Lueur" && !hero.empty())
        return hero;
    std::string text = table->Get(key);
    if (text.find(fee_text::PlayerNameMark) == std::string::npos)
        return text;
    // "[tag g6t3:]" -- the player-given name, else the avatar's default name from the table.
    if (hero.empty())
        hero = table->Get("MPID_Lueur");
    std::string out;
    for (const char c : text) {
        if (c == fee_text::PlayerNameMark)
            out += hero;
        else
            out += c;
    }
    return out;
}

void Assets::InstallTable(fee_text::Table table) {
    auto shared = std::make_shared<const fee_text::Table>(std::move(table));
    std::scoped_lock lock{text_mutex_};
    text_ = std::move(shared);
}

void Assets::SetHeroName(std::string name) {
    std::scoped_lock lock{text_mutex_};
    hero_name_ = std::move(name);
}

std::string Assets::HeroName() const {
    std::scoped_lock lock{text_mutex_};
    return hero_name_;
}

bool Assets::HasFace(std::string_view sprite) {
    if (sprite.empty())
        return false;
    {
        std::scoped_lock lock{face_mutex_};
        if (names_ready_)
            return names_.contains(std::string{sprite});
    }
    std::unordered_set<std::string> names;
    try {
        for (auto& name : unity_.ListSprites(kFaceBundle))
            names.insert(std::string{name});
    } catch (...) {
        names.clear();
    }
    std::scoped_lock lock{face_mutex_};
    names_ = std::move(names);
    names_ready_ = true;
    return names_.contains(std::string{sprite});
}

std::shared_ptr<const lp_unity::Image> Assets::Face(std::string_view sprite) {
    if (sprite.empty())
        return nullptr;
    const std::string key{sprite};
    {
        std::scoped_lock lock{face_mutex_};
        const auto it = faces_.find(key);
        if (it != faces_.end()) {
            const auto lru = std::find(face_lru_.begin(), face_lru_.end(), key);
            if (lru != face_lru_.end()) {
                face_lru_.erase(lru);
                face_lru_.push_back(key);
            }
            return it->second;
        }
    }
    auto image = unity_.LoadSprite(kFaceBundle, sprite);
    if (!image)
        return nullptr;
    auto shared = std::make_shared<const lp_unity::Image>(std::move(*image));
    {
        std::scoped_lock lock{face_mutex_};
        faces_.insert_or_assign(key, shared);
        face_lru_.push_back(key);
        while (face_lru_.size() > kFaceCache) {
            faces_.erase(face_lru_.front());
            face_lru_.erase(face_lru_.begin());
        }
    }
    return shared;
}

} // namespace fee
