// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ctr_lzma.h"

#include <algorithm>
#include <array>

namespace CtrLzma {
namespace {

using Prob = std::uint16_t;
constexpr Prob ProbInit = 1024;

/// Range decoder over a bounded input; reading past the end marks the stream bad.
struct RangeDecoder {
    const std::uint8_t* in;
    std::size_t size;
    std::size_t pos{};
    std::uint32_t range{0xFFFFFFFFu};
    std::uint32_t code{};
    bool bad{};

    std::uint8_t Next() {
        if (pos >= size) {
            bad = true;
            return 0;
        }
        return in[pos++];
    }
    bool Init() {
        if (Next() != 0)
            return false;
        for (int i = 0; i < 4; ++i)
            code = (code << 8) | Next();
        return !bad && code != range;
    }
    void Normalize() {
        if (range < (1u << 24)) {
            range <<= 8;
            code = (code << 8) | Next();
        }
    }
    unsigned Bit(Prob& p) {
        const std::uint32_t bound = (range >> 11) * p;
        unsigned bit;
        if (code < bound) {
            p = static_cast<Prob>(p + ((2048 - p) >> 5));
            range = bound;
            bit = 0;
        } else {
            p = static_cast<Prob>(p - (p >> 5));
            code -= bound;
            range -= bound;
            bit = 1;
        }
        Normalize();
        return bit;
    }
    std::uint32_t Direct(unsigned bits) {
        std::uint32_t res = 0;
        do {
            range >>= 1;
            code -= range;
            const std::uint32_t t = 0u - (code >> 31);
            code += range & t;
            Normalize();
            res = (res << 1) + (t + 1);
        } while (--bits);
        return res;
    }
};

std::uint32_t Tree(RangeDecoder& rc, Prob* probs, unsigned bits) {
    std::uint32_t m = 1;
    for (unsigned i = 0; i < bits; ++i)
        m = (m << 1) + rc.Bit(probs[m]);
    return m - (1u << bits);
}
std::uint32_t ReverseTree(RangeDecoder& rc, Prob* probs, unsigned bits) {
    std::uint32_t m = 1, sym = 0;
    for (unsigned i = 0; i < bits; ++i) {
        const unsigned bit = rc.Bit(probs[m]);
        m = (m << 1) + bit;
        sym |= bit << i;
    }
    return sym;
}

struct LenDecoder {
    Prob choice{ProbInit}, choice2{ProbInit};
    std::array<std::array<Prob, 8>, 16> low{}, mid{};
    std::array<Prob, 256> high{};
    LenDecoder() {
        for (auto& a : low)
            a.fill(ProbInit);
        for (auto& a : mid)
            a.fill(ProbInit);
        high.fill(ProbInit);
    }
    std::uint32_t Decode(RangeDecoder& rc, unsigned pos_state) {
        if (rc.Bit(choice) == 0)
            return Tree(rc, low[pos_state].data(), 3);
        if (rc.Bit(choice2) == 0)
            return 8 + Tree(rc, mid[pos_state].data(), 3);
        return 16 + Tree(rc, high.data(), 8);
    }
};

} // namespace

bool Decode(const std::uint8_t* props, const std::uint8_t* in, std::size_t in_size,
            std::size_t out_size, std::vector<std::uint8_t>& out) {
    if (!props || !in || props[0] >= 9 * 5 * 5)
        return false;
    const unsigned lc = props[0] % 9, lp = (props[0] / 9) % 5, pb = props[0] / 45;
    out.clear();
    out.reserve(out_size);
    if (out_size == 0)
        return true;

    std::vector<Prob> literal(static_cast<std::size_t>(0x300) << (lc + lp), ProbInit);
    std::array<Prob, 12 << 4> is_match{}, is_rep0_long{};
    std::array<Prob, 12> is_rep{}, is_rep_g0{}, is_rep_g1{}, is_rep_g2{};
    std::array<std::array<Prob, 64>, 4> pos_slot{};
    std::array<Prob, 1 + 114> pos_decoders{};
    std::array<Prob, 16> align{};
    is_match.fill(ProbInit);
    is_rep0_long.fill(ProbInit);
    is_rep.fill(ProbInit);
    is_rep_g0.fill(ProbInit);
    is_rep_g1.fill(ProbInit);
    is_rep_g2.fill(ProbInit);
    for (auto& a : pos_slot)
        a.fill(ProbInit);
    pos_decoders.fill(ProbInit);
    align.fill(ProbInit);
    LenDecoder len_dec, rep_len_dec;

    RangeDecoder rc{in, in_size};
    if (!rc.Init())
        return false;
    unsigned state = 0;
    std::uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    const std::uint32_t pb_mask = (1u << pb) - 1, lp_mask = (1u << lp) - 1;

    while (out.size() < out_size) {
        if (rc.bad)
            return false;
        const std::size_t pos = out.size();
        const unsigned pos_state = static_cast<unsigned>(pos) & pb_mask;
        if (rc.Bit(is_match[(state << 4) + pos_state]) == 0) {
            const unsigned prev = pos ? out[pos - 1] : 0;
            const std::size_t lit_state =
                ((static_cast<unsigned>(pos) & lp_mask) << lc) + (prev >> (8 - lc));
            Prob* probs = literal.data() + 0x300 * lit_state;
            unsigned symbol = 1;
            if (state >= 7) {
                if (rep0 >= pos)
                    return false;
                unsigned match_byte = out[pos - rep0 - 1];
                do {
                    const unsigned match_bit = (match_byte >> 7) & 1;
                    match_byte <<= 1;
                    const unsigned bit = rc.Bit(probs[((1 + match_bit) << 8) + symbol]);
                    symbol = (symbol << 1) | bit;
                    if (match_bit != bit)
                        break;
                } while (symbol < 0x100);
            }
            while (symbol < 0x100)
                symbol = (symbol << 1) | rc.Bit(probs[symbol]);
            out.push_back(static_cast<std::uint8_t>(symbol));
            state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
            continue;
        }
        std::uint32_t len;
        if (rc.Bit(is_rep[state]) != 0) {
            if (pos == 0)
                return false;
            if (rc.Bit(is_rep_g0[state]) == 0) {
                if (rc.Bit(is_rep0_long[(state << 4) + pos_state]) == 0) { // short rep
                    if (rep0 >= pos)
                        return false;
                    state = state < 7 ? 9 : 11;
                    out.push_back(out[pos - rep0 - 1]);
                    continue;
                }
            } else {
                std::uint32_t dist;
                if (rc.Bit(is_rep_g1[state]) == 0) {
                    dist = rep1;
                } else {
                    if (rc.Bit(is_rep_g2[state]) == 0) {
                        dist = rep2;
                    } else {
                        dist = rep3;
                        rep3 = rep2;
                    }
                    rep2 = rep1;
                }
                rep1 = rep0;
                rep0 = dist;
            }
            len = rep_len_dec.Decode(rc, pos_state);
            state = state < 7 ? 8 : 11;
        } else {
            rep3 = rep2;
            rep2 = rep1;
            rep1 = rep0;
            len = len_dec.Decode(rc, pos_state);
            state = state < 7 ? 7 : 10;
            // distance
            const unsigned len_state = std::min<std::uint32_t>(len, 3);
            const std::uint32_t slot = Tree(rc, pos_slot[len_state].data(), 6);
            if (slot < 4) {
                rep0 = slot;
            } else {
                const unsigned direct = (slot >> 1) - 1;
                std::uint32_t dist = (2 | (slot & 1)) << direct;
                if (slot < 14) {
                    dist += ReverseTree(rc, pos_decoders.data() + dist - slot, direct);
                } else {
                    dist += rc.Direct(direct - 4) << 4;
                    dist += ReverseTree(rc, align.data(), 4);
                }
                rep0 = dist;
            }
            if (rep0 == 0xFFFFFFFFu) // end marker
                return out.size() == out_size;
        }
        len += 2;
        if (rep0 >= out.size())
            return false;
        for (std::uint32_t i = 0; i < len && out.size() < out_size; ++i)
            out.push_back(out[out.size() - rep0 - 1]);
    }
    return !rc.bad;
}

} // namespace CtrLzma
