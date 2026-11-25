// SPDX-FileCopyrightText: Copyright 2026 Daniel Vergara
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ffr_assets.h"

#include <algorithm>
#include <cstring>

#include "bc_decoder.h"
#include "third_party/ooz/ooz.h"

namespace ffr {
namespace {

template <class T> bool Get(const uint8_t* b, size_t size, size_t o, T& v) {
    if (o > size || size - o < sizeof(T)) return false;
    std::memcpy(&v, b + o, sizeof(T));
    return true;
}
uint64_t Be40(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 5; ++i) v = (v << 8) | p[i];
    return v;
}
uint64_t Le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
void AppendUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) s += char(c);
    else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
    else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 0x3F)); s += char(0x80 | ((c >> 6) & 0x3F)); s += char(0x80 | (c & 0x3F)); }
}
bool ReadAll(const RomRead& read, const std::string& path, std::vector<uint8_t>& out) {
    const size_t n = read(path.c_str(), 0, nullptr, 0);
    if (n == 0 || n > (64u << 20)) return false;
    out.resize(n);
    return read(path.c_str(), 0, out.data(), n) == n;
}
std::string NormPath(std::string s) {
    // mount points are "../../../" + path; collapse "x/../"
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string p = s.substr(i, j - i);
        if (p == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back(); else parts.push_back(p); }
        else if (!p.empty() && p != ".") parts.push_back(p);
        i = j + 1;
    }
    std::string out;
    for (auto& p : parts) {
        if (p == "..") continue;
        if (!out.empty()) out += '/';
        out += p;
    }
    return out;
}

} // namespace

bool OodleDecode(const uint8_t* src, size_t n, uint8_t* dst, size_t raw) {
    if (!n || !raw || raw > (64u << 20)) return false;
    std::vector<uint8_t> in(n + OOZ_SAFE_SPACE, 0), out(raw + OOZ_SAFE_SPACE);
    std::memcpy(in.data(), src, n);
    if (ooz_decompress(in.data(), n, out.data(), raw) != int(raw)) return false;
    std::memcpy(dst, out.data(), raw);
    return true;
}

bool ReadFString(const uint8_t* b, size_t size, size_t& o, std::string& out) {
    int32_t n;
    if (!Get(b, size, o, n)) return false;
    o += 4;
    out.clear();
    if (n == 0) return true;
    if (n < 0) {
        const size_t bytes = size_t(-int64_t(n)) * 2;
        if (n < -(1 << 20) || o + bytes > size) return false;
        for (size_t i = 0; i + 1 < size_t(-int64_t(n)); ++i) {
            uint32_t c = uint32_t(b[o + 2 * i]) | uint32_t(b[o + 2 * i + 1]) << 8;
            if (c >= 0xD800 && c < 0xDC00 && i + 2 < size_t(-int64_t(n))) {
                uint32_t lo = uint32_t(b[o + 2 * i + 2]) | uint32_t(b[o + 2 * i + 3]) << 8;
                if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); ++i; }
            }
            AppendUtf8(out, c);
        }
        o += bytes;
        return true;
    }
    if (n > (1 << 20) || o + size_t(n) > size) return false;
    out.assign(reinterpret_cast<const char*>(b + o), size_t(n) - 1);
    o += size_t(n);
    return true;
}

// ---- IoStore (TOC version 8) ----
bool IoStore::Open(const RomRead& read, const std::string& utoc, const std::string& ucas, std::string& err) {
    read_ = read;
    ucas_ = ucas;
    std::vector<uint8_t> b;
    if (!ReadAll(read, utoc, b) || b.size() < 144) { err = "utoc unreadable"; return false; }
    static const uint8_t kMagic[16] = {'-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-', '-', '=', '=', '-'};
    if (std::memcmp(b.data(), kMagic, 16) != 0) { err = "not an IoStore TOC"; return false; }
    const uint8_t ver = b[16];
    uint32_t h[9];
    std::memcpy(h, b.data() + 20, sizeof h);
    const uint32_t hsz = h[0], n = h[1], nblk = h[2], bsz = h[3], nmeth = h[4], mlen = h[5], dsz = h[7];
    block_size_ = h[6];
    if (bsz != 12 || ver < 5 || block_size_ == 0) { err = "unexpected TOC layout"; return false; }
    if (b[80] & 2) { err = "encrypted container"; return false; }
    uint32_t nseeds = 0, nover = 0;
    std::memcpy(&nseeds, b.data() + 84, 4);
    std::memcpy(&nover, b.data() + 96, 4);
    size_t o = hsz + size_t(12) * n;  // chunk ids (unused: paths come from the directory index)
    if (o + size_t(10) * n > b.size()) { err = "TOC truncated"; return false; }
    ranges_.resize(n);
    for (uint32_t i = 0; i < n; ++i) ranges_[i] = {Be40(&b[o + 10 * i]), Be40(&b[o + 10 * i + 5])};
    o += size_t(10) * n + size_t(4) * nseeds + size_t(4) * nover;
    if (o + size_t(12) * nblk > b.size()) { err = "TOC truncated (blocks)"; return false; }
    blocks_.resize(nblk);
    for (uint32_t i = 0; i < nblk; ++i) {
        const uint8_t* e = &b[o + 12 * i];
        blocks_[i] = {Le(e, 5), uint32_t(Le(e + 5, 3)), uint32_t(Le(e + 8, 3)), e[11]};
    }
    o += size_t(12) * nblk;
    for (uint32_t i = 0; i < nmeth; ++i) {
        std::string m(reinterpret_cast<const char*>(&b[o + size_t(mlen) * i]), mlen);
        m.resize(std::strlen(m.c_str()));
        methods_.push_back(m);
    }
    o += size_t(mlen) * nmeth;
    if (!(b[80] & 8) || (b[80] & 4) || o + dsz > b.size()) { err = "no directory index"; return false; }
    // FIoDirectoryIndexResource
    const uint8_t* d = &b[o];
    size_t p = 0;
    std::string mount;
    if (!ReadFString(d, dsz, p, mount)) { err = "bad directory index"; return false; }
    int32_t nd, nf, ns;
    if (!Get(d, dsz, p, nd) || nd < 0 || p + 4 + size_t(nd) * 16 > dsz) { err = "bad dirs"; return false; }
    const uint8_t* dirs = d + p + 4;
    p += 4 + size_t(nd) * 16;
    if (!Get(d, dsz, p, nf) || nf < 0 || p + 4 + size_t(nf) * 12 > dsz) { err = "bad files"; return false; }
    const uint8_t* fil = d + p + 4;
    p += 4 + size_t(nf) * 12;
    if (!Get(d, dsz, p, ns) || ns < 0) { err = "bad strings"; return false; }
    p += 4;
    std::vector<std::string> strs(static_cast<size_t>(ns));
    for (auto& s : strs)
        if (!ReadFString(d, dsz, p, s)) { err = "bad string"; return false; }
    constexpr uint32_t kNone = 0xffffffffu;
    struct Todo { uint32_t dir; std::string at; };
    std::vector<Todo> todo;
    if (nd > 0) todo.push_back({0, mount});
    while (!todo.empty()) {
        Todo t = std::move(todo.back());
        todo.pop_back();
        if (t.dir >= uint32_t(nd)) continue;
        uint32_t e[4];
        std::memcpy(e, dirs + size_t(16) * t.dir, 16);
        const std::string here = e[0] != kNone && e[0] < strs.size() ? t.at + strs[e[0]] + "/" : t.at;
        if (e[2] != kNone) todo.push_back({e[2], t.at});
        if (e[1] != kNone) todo.push_back({e[1], here});
        uint32_t f = e[3];
        for (int guard = 0; f != kNone && f < uint32_t(nf) && guard < nf; ++guard) {
            uint32_t fe[3];
            std::memcpy(fe, fil + size_t(12) * f, 12);
            if (fe[0] < strs.size() && fe[2] < n) files_[NormPath(here + strs[fe[0]])] = fe[2];
            f = fe[1];
        }
    }
    if (files_.empty()) { err = "empty container"; return false; }
    return true;
}

bool IoStore::Read(const std::string& path, std::vector<uint8_t>& out) const {
    auto it = files_.find(path);
    if (it == files_.end()) return false;
    const auto [off, len] = ranges_[it->second];
    if (len == 0 || len > (64u << 20)) return false;
    const uint64_t first = off / block_size_, last = (off + len - 1) / block_size_;
    if (last >= blocks_.size()) return false;
    std::vector<uint8_t> all;
    all.reserve(size_t((last - first + 1) * block_size_));
    std::vector<uint8_t> comp;
    for (uint64_t k = first; k <= last; ++k) {
        const Block& bl = blocks_[k];
        comp.resize(bl.size);
        if (read_(ucas_.c_str(), bl.off, comp.data(), bl.size) != bl.size) return false;
        const size_t at = all.size();
        all.resize(at + bl.raw);
        if (bl.method == 0) {
            std::memcpy(all.data() + at, comp.data(), std::min<size_t>(bl.size, bl.raw));
        } else if (bl.method - 1u < methods_.size() && methods_[bl.method - 1] == "Oodle") {
            if (!OodleDecode(comp.data(), bl.size, all.data() + at, bl.raw)) return false;
        } else {
            return false;
        }
    }
    const size_t s = size_t(off - first * block_size_);
    if (s + len > all.size()) return false;
    out.assign(all.begin() + s, all.begin() + s + len);
    return true;
}

// ---- pak (version 11, encoded entries, footer magic 0xcc from the end) ----
bool Pak::Open(const RomRead& read, const std::string& path, std::string& err) {
    read_ = read;
    path_ = path;
    const size_t size = read(path.c_str(), 0, nullptr, 0);
    if (size < 0x200) { err = "pak unreadable"; return false; }
    uint8_t ft[0xcc + 21];
    if (read(path.c_str(), size - sizeof ft, ft, sizeof ft) != sizeof ft) { err = "pak footer"; return false; }
    uint32_t magic, ver;
    std::memcpy(&magic, ft + 21, 4);
    std::memcpy(&ver, ft + 25, 4);
    if (magic != 0x5A6F12E1 || ver != 11) { err = "unexpected pak footer"; return false; }
    if (ft[20]) { err = "encrypted pak index"; return false; }
    uint64_t ioff, isz;
    std::memcpy(&ioff, ft + 29, 8);
    std::memcpy(&isz, ft + 37, 8);
    for (int i = 0; i < 5; ++i) {
        std::string m(reinterpret_cast<const char*>(ft + 21 + 44 + 32 * i), 32);
        m.resize(std::strlen(m.c_str()));
        methods_.push_back(m);
    }
    if (ioff + isz > size || isz > (64u << 20)) { err = "pak index out of range"; return false; }
    std::vector<uint8_t> idx(isz);
    if (read(path.c_str(), ioff, idx.data(), isz) != isz) { err = "pak index read"; return false; }
    size_t p = 0;
    std::string mount;
    int32_t count;
    uint64_t seed;
    int32_t has;
    if (!ReadFString(idx.data(), isz, p, mount) || !Get(idx.data(), isz, p, count)) { err = "pak index"; return false; }
    p += 4;
    if (!Get(idx.data(), isz, p, seed)) return false;
    p += 8;
    if (!Get(idx.data(), isz, p, has)) return false;
    p += 4;
    if (has) p += 16 + 20;
    if (!Get(idx.data(), isz, p, has) || !has) { err = "no full directory index"; return false; }
    p += 4;
    uint64_t doff, dsz;
    if (!Get(idx.data(), isz, p, doff) || !Get(idx.data(), isz, p + 8, dsz)) return false;
    p += 16 + 20;
    int32_t encn;
    if (!Get(idx.data(), isz, p, encn) || encn < 0 || p + 4 + size_t(encn) > isz) return false;
    enc_.assign(idx.begin() + p + 4, idx.begin() + p + 4 + encn);
    if (doff + dsz > size || dsz > (64u << 20)) { err = "pak directory out of range"; return false; }
    std::vector<uint8_t> d(dsz);
    if (read(path.c_str(), doff, d.data(), dsz) != dsz) return false;
    size_t q = 0;
    int32_t nd;
    if (!Get(d.data(), dsz, q, nd)) return false;
    q += 4;
    std::string dn, fn;
    for (int32_t i = 0; i < nd; ++i) {
        int32_t nfiles;
        if (!ReadFString(d.data(), dsz, q, dn) || !Get(d.data(), dsz, q, nfiles)) return false;
        q += 4;
        for (int32_t j = 0; j < nfiles; ++j) {
            int32_t at;
            if (!ReadFString(d.data(), dsz, q, fn) || !Get(d.data(), dsz, q, at)) return false;
            q += 4;
            std::string full = dn + fn;
            while (!full.empty() && full[0] == '/') full.erase(0, 1);
            files_[full] = uint32_t(at);
        }
    }
    return !files_.empty();
}

bool Pak::Read(const std::string& path, std::vector<uint8_t>& out) const {
    auto it = files_.find(path);
    if (it == files_.end() || it->second >= enc_.size()) return false;
    const uint8_t* e = enc_.data();
    const size_t es = enc_.size();
    size_t r = it->second;
    uint32_t v;
    if (!Get(e, es, r, v)) return false;
    r += 4;
    uint32_t bsz = (v & 0x3f) << 11;
    if ((v & 0x3f) == 0x3f) { if (!Get(e, es, r, bsz)) return false; r += 4; }
    const uint32_t nblk = (v >> 6) & 0xffff, enc = (v >> 22) & 1, meth = (v >> 23) & 0x3f;
    if (enc) return false;
    auto var = [&](bool small, uint64_t& x) {
        if (small) { uint32_t y; if (!Get(e, es, r, y)) return false; x = y; r += 4; }
        else { if (!Get(e, es, r, x)) return false; r += 8; }
        return true;
    };
    uint64_t off, raw, stored = 0;
    if (!var(v >> 31 & 1, off) || !var(v >> 30 & 1, raw)) return false;
    if (meth && !var(v >> 29 & 1, stored)) return false;
    if (raw > (64u << 20)) return false;
    if (!meth) {
        out.resize(raw);
        return read_(path_.c_str(), off + 53, out.data(), raw) == raw;
    }
    if (meth > methods_.size() || methods_[meth - 1] != "Oodle") return false;
    if (nblk == 1 && bsz == 0) bsz = uint32_t(raw);
    std::vector<std::pair<uint64_t, uint32_t>> blocks;
    uint64_t cur = 53 + 4 + 16ull * nblk;
    if (nblk == 1) blocks.push_back({cur, uint32_t(stored)});
    else
        for (uint32_t i = 0; i < nblk; ++i) {
            uint32_t s;
            if (!Get(e, es, r, s)) return false;
            r += 4;
            blocks.push_back({cur, s});
            cur += s;
        }
    out.resize(raw);
    uint64_t left = raw, at = 0;
    std::vector<uint8_t> comp;
    for (auto [s, n] : blocks) {
        const uint64_t k = std::min<uint64_t>(left, bsz);
        comp.resize(n);
        if (read_(path_.c_str(), off + s, comp.data(), n) != n) return false;
        if (!OodleDecode(comp.data(), n, out.data() + at, size_t(k))) return false;
        at += k;
        left -= k;
    }
    return left == 0;
}

// ---- textures ----
bool DecodeTexture(const std::vector<uint8_t>& pkg, Image& out) {
    const uint8_t* b = pkg.data();
    const size_t n = pkg.size();
    for (size_t i = 20; i + 16 < n; ++i) {
        if (b[i] != 'P' || b[i + 1] != 'F' || b[i + 2] != '_') continue;
        int32_t slen;
        std::memcpy(&slen, b + i - 4, 4);
        if (slen < 4 || slen > 32 || i + size_t(slen) > n || b[i + slen - 1] != 0) continue;
        const std::string name(reinterpret_cast<const char*>(b + i), size_t(slen) - 1);
        int32_t w, h;
        std::memcpy(&w, b + i - 16, 4);
        std::memcpy(&h, b + i - 12, 4);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) continue;
        const size_t o = i + size_t(slen) + 12;
        const size_t px = size_t(w) * h;
        out.w = w;
        out.h = h;
        out.rgba.assign(px * 4, 0);
        const int nbx = (w + 3) / 4, nby = (h + 3) / 4;
        auto blocks = [&](int bpb, auto fn) {
            if (o + size_t(nbx) * nby * bpb > n) return false;
            for (int by = 0; by < nby; ++by)
                for (int bx = 0; bx < nbx; ++bx) {
                    const uint8_t* s = b + o + (size_t(by) * nbx + bx) * bpb;
                    uint8_t* d = out.rgba.data() + (size_t(by) * 4 * w + size_t(bx) * 4) * 4;
                    fn(s, d, bx * 4, by * 4);
                }
            return true;
        };
        if (name == "PF_BC7") return blocks(16, [&](auto s, auto d, int x, int y) { bcn::DecodeBc7(s, d, x, y, w, h); });
        if (name == "PF_DXT5") return blocks(16, [&](auto s, auto d, int x, int y) { bcn::DecodeBc3(s, d, x, y, w, h); });
        if (name == "PF_DXT1") return blocks(8, [&](auto s, auto d, int x, int y) { bcn::DecodeBc1(s, d, x, y, w, h); });
        if (name == "PF_B8G8R8A8") {
            if (o + px * 4 > n) return false;
            for (size_t k = 0; k < px; ++k) {
                out.rgba[4 * k] = b[o + 4 * k + 2];
                out.rgba[4 * k + 1] = b[o + 4 * k + 1];
                out.rgba[4 * k + 2] = b[o + 4 * k];
                out.rgba[4 * k + 3] = b[o + 4 * k + 3];
            }
            return true;
        }
        if (name == "PF_G8") {
            if (o + px > n) return false;
            for (size_t k = 0; k < px; ++k) out.rgba[4 * k] = out.rgba[4 * k + 1] = out.rgba[4 * k + 2] = b[o + k], out.rgba[4 * k + 3] = 255;
            return true;
        }
        if (name == "PF_BC4" || name == "PF_BC5") {
            const int ch = name == "PF_BC4" ? 1 : 2;
            std::vector<uint8_t> plane(px * ch);
            const bool ok = blocks(ch * 8, [&](auto s, auto, int x, int y) {
                uint8_t* d = plane.data() + (size_t(y) * w + x) * ch;
                if (ch == 1) bcn::DecodeBc4(s, d, x, y, w, h, false);
                else bcn::DecodeBc5(s, d, x, y, w, h, false);
            });
            for (size_t k = 0; k < px; ++k) {
                out.rgba[4 * k] = out.rgba[4 * k + 1] = out.rgba[4 * k + 2] = 255;
                out.rgba[4 * k + 3] = plane[k * ch];
            }
            return ok;
        }
        return false;
    }
    return false;
}

bool ParseLocres(const std::vector<uint8_t>& v, std::unordered_map<std::string, std::string>& out) {
    const uint8_t* b = v.data();
    const size_t n = v.size();
    if (n < 32) return false;
    const uint8_t ver = b[16];
    if (ver < 2 || ver > 3) return false;
    size_t o = 17;
    int64_t soff;
    if (!Get(b, n, o, soff) || soff < 0 || size_t(soff) >= n) return false;
    o += 8;
    if (ver >= 3) o += 4;
    size_t p = size_t(soff);
    int32_t ns;
    if (!Get(b, n, p, ns) || ns < 0) return false;
    p += 4;
    std::vector<std::string> strs(static_cast<size_t>(ns));
    for (auto& s : strs) {
        if (!ReadFString(b, n, p, s)) return false;
        p += 4;  // refcount
    }
    uint32_t nns;
    if (!Get(b, n, o, nns)) return false;
    o += 4;
    std::string nsn, key;
    for (uint32_t i = 0; i < nns; ++i) {
        o += 4;
        uint32_t nk;
        if (!ReadFString(b, n, o, nsn) || !Get(b, n, o, nk)) return false;
        o += 4;
        for (uint32_t k = 0; k < nk; ++k) {
            o += 4;
            int32_t idx;
            if (!ReadFString(b, n, o, key) || !Get(b, n, o + 4, idx)) return false;
            o += 8;
            if (idx >= 0 && size_t(idx) < strs.size()) out[nsn + '\x1f' + key] = strs[size_t(idx)];
        }
    }
    return !out.empty();
}

bool UnwrapFontFace(const std::vector<uint8_t>& b, std::vector<uint8_t>& ttf) {
    uint32_t n;
    if (!Get(b.data(), b.size(), 0, n) || n + 4 > b.size() || n < 12) return false;
    ttf.assign(b.begin() + 4, b.begin() + 4 + n);
    return true;
}

} // namespace ffr
