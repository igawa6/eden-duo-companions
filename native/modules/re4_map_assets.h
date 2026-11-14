// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "re4_assets.h"
namespace re4::assets {
bool MapSprite(const EdenDsmodHostApi*,std::string_view,Image&,const std::string&);
bool MapImage(const EdenDsmodHostApi*,std::string_view,Image&,const std::string&);
}
