// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_names.h"
#include "re4_sha256.h"
#include <stdexcept>
namespace re4 {
void RuntimeNames::Load(const EdenDsmodHostApi* host,const std::string& directory){
    if(attempted)return;attempted=true;
    try {
        std::vector<uint8_t> data;
        if(!assets::Read(host,"bio4/etc/core.udas",data,directory)||
           Sha256(data)!="d3ad83a6bda219af11946431ac264baf795b74dc5ce4f50cb882224561affcba")return;
        auto word=[&](size_t p,unsigned bytes)->uint64_t{
            if(p>data.size()||bytes>data.size()-p)throw std::out_of_range("message offset");
            uint64_t v=0;for(unsigned i=0;i<bytes;++i)v|=uint64_t(data[p+i])<<(i*8);return v;
        };
        const size_t archive=word(0x2c,4),table=archive+word(archive+0x15*4,4),block=table+word(table+8,4);
        if(word(block+4,4)!=names.size())return;
        std::array<std::string,272> result;
        for(size_t i=0;i<result.size();++i){
            size_t cursor=block+word(block+8+i*8,8);auto& text=result[i];
            for(;;){
                const auto w=word(cursor,2);cursor+=2;if(w==1)break;
                if(w<0x80){if(w!=0&&w!=8&&w!=14)return;continue;}
                const auto glyph=w-0x80;
                if(glyph==0)text+=' ';
                else if(glyph>=3&&glyph<22)text+="0123456789:%&+-/=,."[glyph-3];
                else if(glyph>=39&&glyph<91)text+="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"[glyph-39];
                else if(glyph==24)text+='(';else if(glyph==25)text+=')';
                else if(glyph==163)text+='\'';else if(glyph==181)text+="\xc3\x97";else return;
                if(text.size()>4096)return;
            }
            const auto first=text.find_first_not_of(' ');
            text=first==text.npos?"":text.substr(first,text.find_last_not_of(' ')-first+1);
        }
        names=std::move(result);ready=true;
    }catch(...){names={};}
}
}
