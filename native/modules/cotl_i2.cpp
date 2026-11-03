// SPDX-FileCopyrightText: Copyright 2026 AlterDigga
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cotl_i2.h"

#include <cstring>

namespace cotl {

namespace {

// Unity serialization (little endian): strings and byte arrays are aligned to 4 bytes.
class Cursor {
public:
    Cursor(const std::vector<uint8_t>& b, size_t pos) : b_(b), pos_(pos) {}

    bool ok() const { return ok_; }

    int32_t I32() {
        int32_t v = 0;
        Bytes(&v, 4);
        return v;
    }
    uint8_t U8() {
        uint8_t v = 0;
        Bytes(&v, 1);
        return v;
    }
    void Align() { pos_ = (pos_ + 3) & ~size_t{3}; }
    std::string Str() {
        const int32_t n = I32();
        if (n < 0 || static_cast<size_t>(n) > b_.size() - std::min(pos_, b_.size())) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b_.data() + pos_), static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        Align();
        return s;
    }
    void Skip(size_t n) {
        if (n > b_.size() - std::min(pos_, b_.size())) {
            ok_ = false;
        } else {
            pos_ += n;
        }
    }
    // Count of an array, limited to `max`
    int32_t Count(int32_t max) {
        const int32_t n = I32();
        if (n < 0 || n > max) {
            ok_ = false;
            return 0;
        }
        return n;
    }

private:
    void Bytes(void* out, size_t n) {
        if (!ok_ || n > b_.size() - std::min(pos_, b_.size())) {
            ok_ = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, b_.data() + pos_, n);
        pos_ += n;
    }

    const std::vector<uint8_t>& b_;
    size_t pos_;
    bool ok_ = true;
};

constexpr size_t kMonoBehaviourNameAt = 28; // m_GameObject PPtr, m_Enabled (ausgerichtet), m_Script PPtr
constexpr int32_t kMaxTerms = 200000;
constexpr int32_t kMaxLanguages = 64;

struct Term {
    std::string key;
    std::vector<std::string> languages;
};

// bool/byte fields of a script: each one aligned to 4 bytes on its own (measured).
void Bools(Cursor& c, int count) {
    for (int i = 0; i < count; ++i) {
        c.U8();
        c.Align();
    }
}

// LanguageSourceAsset.mSource (I2.Loc.LanguageSourceData). Field order from dump.cs, without the
// [NonSerialized] fields mIsGlobalSource and TermData.Description: Il2CppDumper does not show the flag,
// the data in resources.assets does not contain them (hex dump).
// Ebenso fehlt LanguageData.Compressed.
std::optional<I2Table> Parse(const std::vector<uint8_t>& raw) {
    Cursor c(raw, kMonoBehaviourNameAt);
    c.Str(); // m_Name
    // UserAgreesToHaveItOnTheScene, UserAgreesToHaveItInsideThePluginsFolder,
    // GoogleLiveSyncIsUptoDate
    Bools(c, 3);

    std::vector<Term> terms(static_cast<size_t>(c.Count(kMaxTerms)));
    for (auto& t : terms) {
        t.key = c.Str();                          // Term
        c.I32();                                  // TermType
        t.languages.resize(static_cast<size_t>(c.Count(kMaxLanguages)));
        for (auto& l : t.languages) {
            l = c.Str();                          // Languages[]
        }
        c.Skip(static_cast<size_t>(c.Count(kMaxLanguages))); // Flags (byte[])
        c.Align();
        for (int32_t n = c.Count(kMaxLanguages); n > 0; --n) {
            c.Str();                              // Languages_Touch[]
        }
        if (!c.ok()) {
            return std::nullopt;
        }
    }
    Bools(c, 1); // CaseInsensitiveTerms
    c.I32();                    // OnMissingTranslation
    c.Str();                    // mTerm_AppName

    int32_t english = -1;
    const int32_t languages = c.Count(kMaxLanguages);
    for (int32_t i = 0; i < languages; ++i) {
        const std::string name = c.Str(); // name
        c.Str();                          // Code
        Bools(c, 1);                      // Flags (byte)
        if (name == "English" && english < 0) {
            english = i;
        }
    }
    if (!c.ok() || english < 0 || terms.empty()) {
        return std::nullopt;
    }

    std::map<std::string, std::string> out;
    for (auto& t : terms) {
        if (static_cast<size_t>(english) < t.languages.size() &&
            !t.languages[static_cast<size_t>(english)].empty()) {
            out.emplace(std::move(t.key), std::move(t.languages[static_cast<size_t>(english)]));
        }
    }
    return I2Table::FromMap(std::move(out));
}

} // namespace

std::optional<I2Table> I2Table::Load(const ce_unity::RangeReader& romfs, std::string_view path) {
    try {
        if (!romfs) {
            return std::nullopt;
        }
        const auto size = romfs.size(path);
        if (!size) {
            return std::nullopt;
        }
        ce_unity::ByteSource src;
        src.size = *size;
        src.read = [romfs, p = std::string(path)](uint64_t off, uint8_t* out, size_t n) {
            return romfs.read(p, off, out, n);
        };
        const auto file = ce_unity::SerializedFile::Open(std::move(src));
        if (!file) {
            return std::nullopt;
        }
        for (const auto& obj : file->Objects()) {
            if (obj.class_id != 114 || file->PeekName(obj) != "I2Languages") {
                continue;
            }
            const auto raw = file->ReadRaw(obj);
            if (!raw) {
                return std::nullopt;
            }
            return Parse(*raw);
        }
    } catch (...) {
    }
    return std::nullopt;
}

I2Table I2Table::FromMap(std::map<std::string, std::string> terms) {
    I2Table t;
    for (auto& [k, v] : terms) {
        t.terms_.emplace(k, std::move(v));
    }
    return t;
}

std::optional<std::string> I2Table::English(std::string_view term) const {
    if (const auto it = terms_.find(term); it != terms_.end()) {
        return it->second;
    }
    return std::nullopt;
}

} // namespace cotl
