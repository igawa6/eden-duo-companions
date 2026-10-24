// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later
#include "echoes_asset_cache.h"
#include <cassert>
int main(){
 using namespace echoes;constexpr auto key="region_common/ui/Field.arc#timg/__Combined.bntx";
 FieldArchiveCache cache;size_t calls=0;bool short_read=true;
 ReadAssetFn read=[&](std::string_view,uint64_t,void* out,size_t n)->size_t{++calls;if(!out)return 1024;if(short_read)return n-1;std::memset(out,42,n);return n;};
 unsigned char out[4]{};
 assert(cache.Read(read,key,0,out,4)==0 && calls==2);
 short_read=false;assert(cache.Read(read,key,1022,out,4)==2 && out[0]==42 && calls==4);
 for(int i=0;i<500;++i)assert(cache.Read(read,key,i,out,4)==4);
 assert(calls==4 && cache.Read(read,key,0,nullptr,0)==1024);
 assert(cache.Read(read,key,UINT64_MAX,out,4)==0);
 assert(cache.Read(read,"other",0,nullptr,0)==1024 && calls==5);
 FieldArchiveCache huge;ReadAssetFn oversized=[](auto,auto,auto,auto){return size_t(33u<<20);};
 assert(huge.Read(oversized,key,0,nullptr,0)==0);
}
