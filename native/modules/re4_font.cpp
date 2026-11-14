// SPDX-License-Identifier: GPL-3.0-or-later
#include "re4_font.h"
#include "re4_assets.h"
#include <algorithm>
#include <cstring>
namespace re4::assets {
namespace {
int Index(unsigned cp){
    if(cp>='0'&&cp<='9')return cp-'0'+3;
    if(cp>='A'&&cp<='Z')return cp-'A'+39;
    if(cp>='a'&&cp<='z')return cp-'a'+65;
    switch(cp){
    case ':':return 13;case '%':return 14;case '&':return 15;case '+':return 16;
    case '-':return 17;case '/':return 18;case '=':return 19;case ',':return 20;
    case '.':return 21;case '(':return 24;case ')':return 25;case '!':return 26;
    case '?':return 27;case '<':return 33;case '>':return 34;case '[':return 35;
    case ']':return 36;case '\'':return 163;case 215:return 181;default:return -1;
    }
}
}
void Font::Warm(const EdenDsmodHostApi* host,const Image* atlas){
    if(attempted)return;
    // Retry transient reads/allocations, but bound repeated work for broken RomFS.
    attempted=++attempts>=30;
    Image decoded;
    if(!atlas){if(!Load(host,"module:re4/font/atlas",decoded))return;atlas=&decoded;}
    if(!*atlas||atlas->w!=1536||atlas->h!=576){attempted=true;return;}
    std::vector<uint8_t> result(0x20000,255);
    std::array<EdenDsmodFontGlyph,95> metrics{};
    std::array<unsigned,95> top{};
    unsigned count=0,height=0;
    auto put=[&](size_t at,unsigned v){result[at]=v;result[at+1]=v>>8;};
    for(unsigned cp=32;cp<=215;++cp){
        if(cp>126&&cp!=215)continue;
        unsigned x=0,y=0,w=0,h=0,pad=0,advance=12;
        if(cp!=32){
            const int index=Index(cp);if(index<0)continue;
            const unsigned ox=(index%32)*48,oy=(index/32)*48;
            unsigned x0=48,y0=48,x1=0,y1=0;
            for(unsigned j=0;j<48;++j)for(unsigned i=0;i<48;++i)
                if(atlas->pixels[(size_t(oy+j)*atlas->w+ox+i)*4+3]){
                    x0=std::min(x0,i);y0=std::min(y0,j);x1=std::max(x1,i+1);y1=std::max(y1,j+1);
                }
            if(!x1)continue;
            x=ox+x0;y=oy+y0;w=x1-x0;h=y1-y0;pad=y0;advance=w+2;
        }
        put(cp*2,count++);
        const size_t at=result.size();result.resize(at+12);
        put(at,cp);put(at+2,x);put(at+4,y);result[at+6]=w;result[at+7]=h;
        result[at+8]=0;result[at+9]=pad;put(at+10,advance*16);
        // Match the existing SoS adapter's ASCII range and baseline conversion exactly.
        if(cp<=126){metrics[cp-32]={uint16_t(x),uint16_t(y),uint16_t(w),uint16_t(h),0,0,uint16_t(advance)};top[cp-32]=pad;}
        if((cp>='A'&&cp<='Z')||(cp>='0'&&cp<='9'))height=std::max(height,pad+h);
    }
    if(!height){attempted=true;return;}
    for(unsigned i=0;i<metrics.size();++i)
        if(metrics[i].advance)metrics[i].bearing_y=height-top[i];
    packed=std::move(result);glyphs=metrics;ascent=height;attempted=true;
    ready.store(true,std::memory_order_release);
}
EdenDsmodBool Font::Decode(const uint8_t* bytes,size_t size,void* receiver,EdenDsmodFontSink sink){
    // common_p.fnt is a source sentinel only. No reads, image work or allocation here.
    constexpr uint8_t magic[]={0x78,0x56,0x34,0x12};
    if(!bytes||size<40||std::memcmp(bytes+32,magic,4)||!sink||!ascent)return EDEN_DSMOD_FALSE;
    sink(receiver,ascent,32,glyphs.data(),glyphs.size());
    installed.store(true,std::memory_order_release);
    return EDEN_DSMOD_TRUE;
}
}
