// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
// FINAL FANTASY RESONANCE companion: the player's own game files, read at runtime.
// IoStore container (FFRS-Switch.utoc/.ucas) and pak (FFRS-Switch.pak), Oodle via ooz, textures,
// the English localization table and the UI font faces. Nothing decoded here is shipped.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ffr {

// read(path, offset, out, size) -> bytes read; out == nullptr -> file size (EdenDsmodHostApi::read_romfs)
using RomRead = std::function<size_t(const char*, uint64_t, void*, size_t)>;

bool OodleDecode(const uint8_t* src, size_t n, uint8_t* dst, size_t raw);

// FString at b[o] (int32 length, negative = UTF-16) as UTF-8; o advances. False on a bad length.
bool ReadFString(const uint8_t* b, size_t size, size_t& o, std::string& out);

class IoStore {
public:
    bool Open(const RomRead& read, const std::string& utoc, const std::string& ucas, std::string& err);
    bool Has(const std::string& path) const { return files_.count(path) != 0; }
    bool Read(const std::string& path, std::vector<uint8_t>& out) const;
    size_t FileCount() const { return files_.size(); }

private:
    struct Block { uint64_t off; uint32_t size, raw; uint8_t method; };
    RomRead read_;
    std::string ucas_;
    uint32_t block_size_ = 0;
    std::vector<std::pair<uint64_t, uint64_t>> ranges_;
    std::vector<Block> blocks_;
    std::vector<std::string> methods_;
    std::unordered_map<std::string, uint32_t> files_;
};

class Pak {
public:
    bool Open(const RomRead& read, const std::string& path, std::string& err);
    bool Read(const std::string& path, std::vector<uint8_t>& out) const;
    size_t FileCount() const { return files_.size(); }

private:
    RomRead read_;
    std::string path_;
    std::vector<std::string> methods_;
    std::vector<uint8_t> enc_;  // encoded entries
    std::unordered_map<std::string, uint32_t> files_;
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // straight alpha
    bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};

// A cooked UTexture2D package (its export bytes): the first mip of PF_BC7 / DXT1 / DXT5 / BC4 / BC5 /
// B8G8R8A8 / G8 platform data stored inline (this game keeps UI textures inline and linear).
bool DecodeTexture(const std::vector<uint8_t>& pkg, Image& out);

// Game.locres (version 2/3): "<namespace>\x1f<key>" -> UTF-8 string.
bool ParseLocres(const std::vector<uint8_t>& b, std::unordered_map<std::string, std::string>& out);

// UFontFace bulk (.ufont): a TArray<uint8> count, then the OpenType/TrueType bytes.
bool UnwrapFontFace(const std::vector<uint8_t>& b, std::vector<uint8_t>& ttf);

} // namespace ffr
