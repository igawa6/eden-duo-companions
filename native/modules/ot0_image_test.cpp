// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ot0_image.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace ot0;
int main() {
    assert(DecodeUtf8("Celsus \xc2\xb7 \xf0\x9f\x8c\x99") == U"Celsus · 🌙");
    for (const auto* bad : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe2\x82"})
        assert(!DecodeUtf8(bad));
    Image src{2,1,{255,0,0,255, 255,0,1,134}};
    auto keyed=RedKey(src);
    assert(keyed.rgba[3]==0 && keyed.rgba[7]==134 && src.rgba[3]==255);
    assert(Resize(src,2,0,1,1,1,1).rgba.empty());
    assert(Resize(src,0,0,2,1,0,1).rgba.empty());
    Image invisible{2,1,{255,0,0,0, 0,0,255,255}};
    auto resized=Resize(invisible,0,0,2,1,1,1);
    assert(resized.rgba[0]==0 && resized.rgba[2]>250); // no red alpha halo
    Image dst{1,1,{0,0,255,128}}, overlay{1,1,{255,0,0,128}};
    Over(dst,overlay,0,0);
    assert(dst.rgba[3]==192 && dst.rgba[0]==170 && dst.rgba[2]==85);
    const auto previous=dst.rgba;
    Over(dst,overlay,-1,0); assert(dst.rgba==previous);
    Image solid{3,3,std::vector<u8>(36,200)};
    for (std::size_t i=3;i<solid.rgba.size();i+=4) solid.rgba[i]=255;
    auto blur=DarkenBlur(solid,.1,8);
    assert(blur.width==3 && blur.height==3);
    for (std::size_t i=0;i<blur.rgba.size();i+=4)
        assert(blur.rgba[i]==20 && blur.rgba[i+1]==20 && blur.rgba[i+2]==20 && blur.rgba[i+3]==255);
    auto nine=Nine(solid,8,7);
    assert(nine.width==8 && nine.height==7 && nine.rgba[3]==255 && nine.rgba.back()==255);
    assert(!FontLibrary{}.Measure("x",FontFace::Franco,32));
    std::cout << "ot0_image: Unicode, alpha, bounds and blur checks passed\n";
}
