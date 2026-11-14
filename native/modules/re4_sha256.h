// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
namespace re4 {
inline std::string Sha256(std::span<const uint8_t> data) {
    constexpr std::array<uint32_t,64> constants{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    std::array<uint32_t,8> state{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const size_t padded=(data.size()+9+63)/64*64;
    for(size_t offset=0;offset<padded;offset+=64){
        std::array<uint32_t,64> words{};
        for(unsigned i=0;i<64;++i){
            const size_t p=offset+i;uint8_t b=0;
            if(p<data.size())b=data[p];else if(p==data.size())b=0x80;
            else if(p>=padded-8)b=(uint64_t(data.size())*8)>>((padded-1-p)*8);
            words[i/4]|=uint32_t(b)<<(24-(i%4)*8);
        }
        for(unsigned i=16;i<64;++i){
            auto a=words[i-15],b=words[i-2];
            words[i]=words[i-16]+(std::rotr(a,7)^std::rotr(a,18)^(a>>3))+words[i-7]+(std::rotr(b,17)^std::rotr(b,19)^(b>>10));
        }
        auto v=state;
        for(unsigned i=0;i<64;++i){
            const auto t1=v[7]+(std::rotr(v[4],6)^std::rotr(v[4],11)^std::rotr(v[4],25))+((v[4]&v[5])^(~v[4]&v[6]))+constants[i]+words[i];
            const auto t2=(std::rotr(v[0],2)^std::rotr(v[0],13)^std::rotr(v[0],22))+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]));
            for(unsigned j=7;j>0;--j)v[j]=v[j-1];v[4]+=t1;v[0]=t1+t2;
        }
        for(unsigned i=0;i<8;++i)state[i]+=v[i];
    }
    std::string out;constexpr char hex[]="0123456789abcdef";
    for(auto word:state)for(int i=7;i>=0;--i)out+=hex[(word>>(i*4))&15];return out;
}
}
