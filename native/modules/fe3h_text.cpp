// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "fe3h_text.h"

#include <cstring>

namespace fe3h_text {

bool XlTable::Parse(std::vector<uint8_t> bytes) {
    b_ = std::move(bytes);
    count_ = header_ = 0;
    if (b_.size() < 0x14 || b_[0] != 'X' || b_[1] != 'L')
        return false;
    uint16_t count;
    uint32_t header;
    std::memcpy(&count, b_.data() + 8, 2);
    std::memcpy(&header, b_.data() + 12, 4);
    if (header < 0x14 || uint64_t(header) + uint64_t(count) * 4 > b_.size())
        return false;
    count_ = count;
    header_ = header;
    return true;
}

bool XlTable::Get(uint32_t index, std::string& out) const {
    if (index >= count_)
        return false;
    uint32_t off;
    std::memcpy(&off, b_.data() + header_ + size_t(index) * 4, 4);
    const uint64_t start = uint64_t(header_) + off;
    if (start >= b_.size())
        return false;
    const auto* s = reinterpret_cast<const char*>(b_.data() + start);
    const size_t len = strnlen(s, b_.size() - start);
    if (start + len >= b_.size())
        return false; // unterminated
    out.assign(s, len);
    return true;
}

const XlTable* MessageText::Part(uint32_t lang, uint32_t part) {
    const auto key = std::make_pair(lang, part);
    if (const auto it = parts_.find(key); it != parts_.end())
        return it->second.get();
    auto& slot = parts_[key];
    const auto msg = link_.Entry(MsgDataId);
    const auto l = msg ? fe3h_romfs::PtblChild(msg, lang) : nullptr;
    const auto p = l ? fe3h_romfs::PtblChild(l, part) : nullptr;
    std::vector<uint8_t> bytes;
    if (!p || !p->ReadAll(bytes, uint64_t{16} << 20))
        return nullptr;
    auto table = std::make_unique<XlTable>();
    if (!table->Parse(std::move(bytes)))
        return nullptr;
    slot = std::move(table);
    return slot.get();
}

bool MessageText::Get(uint32_t part, uint32_t index, std::string& out, uint32_t lang) {
    const XlTable* t = Part(lang, part);
    return t && t->Get(index, out);
}

uint32_t MessageText::Count(uint32_t part, uint32_t lang) {
    const XlTable* t = Part(lang, part);
    return t ? t->Count() : 0;
}

} // namespace fe3h_text
