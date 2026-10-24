// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_prologue_fonts.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
namespace {
size_t checks{};
void Check(bool ok,const char* label) {++checks;if(!ok)throw std::runtime_error(label);}
std::vector<uint8_t> File(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);if(!f)return {};
    return {(std::istreambuf_iterator<char>(f)),{}};
}
}
int main() {
    try {
        std::string display="stale";
        const std::string raw="Bomb \xee\x83\xa0; Bow \xee\x83\xa2; Sword \xee\x83\xa3; Shield \xee\x83\xa6";
        const std::string expected="Bomb \xe2\x90\x80; Bow \xe2\x90\x81; Sword \xe2\x90\x82; Shield \xe2\x90\x83";
        Check(echoes::PrologueDisplayText(raw,display) && display==expected,"four display aliases match original message glyphs");
        Check(raw.find("\xee\x83\xa3")!=std::string::npos,"native input metadata remains unchanged");
        Check(echoes::PrologueDisplayText("Native words\nremain unchanged.",display) && display=="Native words\nremain unchanged.","ordinary text and line breaks unchanged");
        for(const auto text:{"\xc0\xaf","\xed\xa0\x80","\xf4\x90\x80\x80","\xee\x83\xa1","\xe2\x90\x80","\x01","\xe2\x80"}) {
            display="stale";Check(!echoes::PrologueDisplayText(text,display) && display.empty(),"unsupported or malformed display text clears output");
        }
        Check(!echoes::PrologueDisplayText(std::string(8193,'A'),display),"display text bounded");
        echoes::FontAtlas atlas;
        auto required=echoes::EnglishBodyCodepoints();for(uint32_t cp=0x2400;cp<0x2404;++cp)required.push_back(cp);
        Check(!echoes::PrologueBodyAtlas({}, {},required,atlas) && atlas.image.Empty(),"missing optional resources fail closed");
        const char* root=std::getenv("ECHOES_ROMFS");const char* ttf=std::getenv("ECHOES_FONT_TTF");
        if(!root || !ttf){std::cout<<checks<<" checks passed; native fixtures skipped (ECHOES_ROMFS/ECHOES_FONT_TTF)\n";return 0;}
        echoes::NativeFonts native;native.compatible=native.locale_valid=native.body_font_supported=native.standard_font_valid=true;
        native.standard_font=File(ttf);
        auto pictogram=File(std::filesystem::path(root)/"region_common/ui/Pictogram.bffnt");Check(!pictogram.empty(),"original Pictogram fixture exists");
        size_t reads=0,queries=0;
        const auto reader=[&](std::string_view path,uint64_t at,void* out,size_t length){
            Check(path=="region_common/ui/Pictogram.bffnt","exact original Pictogram source");
            if(!out && !length){++queries;return pictogram.size();}
            if(at>pictogram.size() || length>pictogram.size()-at)return size_t(0);
            ++reads;std::memcpy(out,pictogram.data()+at,length);return length;
        };
        Check(echoes::PrologueBodyAtlas(native,reader,required,atlas),"original native composite atlas builds");
        Check(queries==1 && reads==1,"native Pictogram read once per worker atlas");
        Check(atlas.line_height==29 && atlas.image.w==1024 && atlas.image.h<=4096,"bounded native body and prologue atlas");
        std::set<std::vector<uint8_t>> masks;
        for(uint32_t cp=0x2400;cp<0x2404;++cp) {
            const auto& g=atlas.glyphs.at(cp-atlas.first_codepoint);
            Check(g.h==29 && g.w>0 && g.advance>=g.w && g.bearing_y==29,"native inline icon keeps29px cap and baseline");
            std::vector<uint8_t> mask;unsigned ink=0;
            for(unsigned y=0;y<g.h;++y)for(unsigned x=0;x<g.w;++x){const auto a=atlas.image.At(g.x+x,g.y+y)[3];mask.push_back(a);ink+=a;}
            Check(ink>0,"CMAP-selected native pictogram has ink");masks.insert(std::move(mask));
        }
        Check(masks.size()==4,"four original button glyphs remain distinct");
        Check(echoes::PrologueDisplayText(raw,display),"fixture description normalized");acnh::Image rendered;
        Check(echoes::RenderBody(atlas,display,{255,243,215,255},2048,rendered) && !rendered.Empty(),"mixed native text and exact button glyphs render");
        Check(!echoes::RenderBody(atlas,raw,{255,243,215,255},2048,rendered),"raw high-PUA metadata is never silently substituted");
        if(const char* output=std::getenv("ECHOES_PROLOGUE_FONT_OUTPUT")) {
            Check(echoes::RenderBody(atlas,display,{255,243,215,255},2048,rendered),"render glyph review strip");
            std::ofstream ppm(output,std::ios::binary);ppm<<"P6\n"<<rendered.w<<" "<<rendered.h<<"\n255\n";
            for(int y=0;y<rendered.h;++y)for(int x=0;x<rendered.w;++x){const auto* p=rendered.At(x,y);for(int c=0;c<3;++c)ppm.put(char((unsigned(p[c])*p[3]+unsigned(24)*(255-p[3]))/255));}
            Check(bool(ppm),"review strip written");
        }
        auto missing=[](std::string_view,uint64_t,void*,size_t){return size_t(0);};
        Check(!echoes::PrologueBodyAtlas(native,missing,required,atlas) && atlas.image.Empty(),"missing Pictogram clears optional atlas");
        const auto original=pictogram;
        for(size_t n:{size_t(20),size_t(60),pictogram.size()-1}) {
            pictogram.assign(original.begin(),original.begin()+n);
            Check(!echoes::PrologueBodyAtlas(native,reader,required,atlas) && atlas.image.Empty(),"truncated original font fails closed");
        }
        pictogram=original;pictogram[0]='X';
        Check(!echoes::PrologueBodyAtlas(native,reader,required,atlas) && atlas.image.Empty(),"incorrect Pictogram container fails closed");
        pictogram=original;
        Check(!echoes::PrologueBodyAtlas(native,reader,required,atlas,21),"minimum cap enforced");
        Check(!echoes::PrologueBodyAtlas(native,reader,required,atlas,37),"maximum cap enforced");
        std::cout<<checks<<" checks passed with original Pictogram and native Standard\n";
    } catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
