// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

// See isaac_romfs.h. Owner: ASSETS lane.

#include "isaac_romfs.h"

namespace isaac_romfs {

std::string_view LangSuffix(Lang lang) {
    switch (lang) {
    case Lang::Jp:
        return "jp";
    case Lang::Kr:
        return "kr";
    case Lang::Zh:
        return "zh";
    case Lang::Ru:
        return "ru";
    case Lang::De:
        return "de";
    case Lang::Es:
        return "es";
    case Lang::Fr:
        return "fr";
    default:
        return "";
    }
}

bool ReadRaw(const EdenDsmodHostApi* host, std::string_view path, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!host || !host->read_romfs || path.empty() || path.size() > 1024)
        return false;
    const std::string p{path};
    const std::size_t size = host->read_romfs(host->userdata, p.c_str(), 0, nullptr, 0);
    if (size == 0 || size > (std::size_t(64) << 20))
        return false;
    out.resize(size);
    if (host->read_romfs(host->userdata, p.c_str(), 0, out.data(), size) != size) {
        out.clear();
        return false;
    }
    return true;
}

namespace {
std::vector<std::string> Order(const EdenDsmodHostApi* host, std::string_view rel, Lang lang) {
    const std::string r{rel};
    const std::string_view sfx = LangSuffix(lang);
    std::vector<std::string> order;
    const bool repentance = !host || !host->get_i64 ||
                            host->get_i64(host->userdata, "__source:aoc", -1) != 0;
    if (repentance && !sfx.empty())
        order.push_back("romfs:rp_patch/resources." + std::string{sfx} + "/" + r);
    if (repentance)
        order.push_back("romfs:rp_patch/resources/" + r);
    if (!sfx.empty())
        order.push_back("aoc:resources." + std::string{sfx} + "/" + r);
    order.push_back("aoc:resources/" + r);
    if (!sfx.empty())
        order.push_back("romfs:resources." + std::string{sfx} + "/" + r);
    order.push_back("romfs:resources/" + r);
    return order;
}
bool Exists(const EdenDsmodHostApi* host, const std::string& p) {
    if (!host || !host->read_romfs)
        return false;
    const std::size_t size = host->read_romfs(host->userdata, p.c_str(), 0, nullptr, 0);
    return size > 0 && size <= (std::size_t(64) << 20);
}
} // namespace

std::string Resolve(const EdenDsmodHostApi* host, std::string_view rel, Lang lang) {
    if (rel.empty() || rel.size() > 900)
        return {};
    for (const auto& p : Order(host, rel, lang))
        if (Exists(host, p))
            return p;
    return {};
}

bool ReadResource(const EdenDsmodHostApi* host, std::string_view rel, Lang lang,
                  std::vector<std::uint8_t>& out) {
    out.clear();
    if (rel.empty() || rel.size() > 900)
        return false;
    for (const auto& p : Order(host, rel, lang))
        if (ReadRaw(host, p, out))
            return true;
    return false;
}

FileCache::Bytes FileCache::Resource(const EdenDsmodHostApi* host, std::string_view rel, Lang lang) {
    if (rel.empty() || rel.size() > 900)
        return nullptr;
    std::string key = "R";
    key += static_cast<char>('0' + static_cast<int>(lang));
    key += rel;
    return Get(host, key, rel, lang, false);
}

FileCache::Bytes FileCache::Raw(const EdenDsmodHostApi* host, std::string_view path) {
    if (path.empty() || path.size() > 1024)
        return nullptr;
    std::string key = "P";
    key += path;
    return Get(host, key, path, Lang::En, true);
}

void FileCache::Clear() {
    std::lock_guard lk{mu_};
    files_.clear();
    lru_.clear();
    missing_.clear();
    used_ = 0;
}

FileCache::Bytes FileCache::Get(const EdenDsmodHostApi* host, const std::string& key,
                                std::string_view rel, Lang lang, bool raw) {
    {
        std::lock_guard lk{mu_};
        if (missing_.count(key))
            return nullptr;
        const auto it = files_.find(key);
        if (it != files_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.it);
            return it->second.bytes;
        }
    }
    auto bytes = std::make_shared<std::vector<std::uint8_t>>();
    const bool ok = raw ? ReadRaw(host, rel, *bytes) : ReadResource(host, rel, lang, *bytes);
    std::lock_guard lk{mu_};
    if (!ok) {
        if (missing_.size() > 4096)
            missing_.clear();
        missing_[key] = true;
        return nullptr;
    }
    const auto it = files_.find(key);
    if (it != files_.end())
        return it->second.bytes;
    if (bytes->size() > budget_)
        return bytes; // too big to keep: hand it out uncached
    lru_.push_front(key);
    files_[key] = Entry{bytes, lru_.begin()};
    used_ += bytes->size();
    while ((used_ > budget_ || files_.size() > 4096) && !lru_.empty()) {
        const auto victim = files_.find(lru_.back());
        used_ -= victim->second.bytes->size();
        files_.erase(victim);
        lru_.pop_back();
    }
    return bytes;
}

} // namespace isaac_romfs
