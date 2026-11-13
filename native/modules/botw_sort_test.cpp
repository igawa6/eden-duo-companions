// SPDX-License-Identifier: GPL-3.0-or-later
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main holding_original_test_main
#include "botw_hold_test.cpp"
#undef main
#pragma GCC diagnostic pop
struct SortFixture:HoldFixture {
 static constexpr uint64_t G=0x1D000000,H=0x1D001000,B=0x1D002000,A0=0x1D003000,F0=0x1D004000,I0=0x1D005000,N=0x1D006000;
 void mode(bool on){
  std::array<std::pair<uint32_t,bool>,4> a;unsigned n=0;
  for(const char*k:{"SortTypeWeaponPouch","SortTypeBowPouch","SortTypeShieldPouch","SortTypeArmorPouch"})a[n++]={botw::warp::crc(k),on};
  std::sort(a.begin(),a.end());
  for(unsigned i=0;i<4;++i){auto at=F0+i*16;put(A0+i*8,at);put(at,M+0x2BD1078);put<uint8_t>(at+8,0);put<uint8_t>(at+9,1);put<uint8_t>(at+10,0);put<uint8_t>(at+11,a[i].second);put<uint32_t>(at+12,a[i].first);}
 }
 SortFixture(){mail<uint32_t>(0,0x494F5544);put<int32_t>(Manager+0x44800,-1);
  put(M+0x2CC5FE8,G);put(G,M+0x2BD07E0);put(G+0xBE0,H);put(H,B);
  put<int32_t>(B+8,4);put<int32_t>(B+12,4);put(B+16,A0);put<int32_t>(B+24,1);put<int32_t>(B+28,1);put(B+32,I0);put(I0,N);
  put(N,std::array<uint8_t,40>{});put(N,M+0x2BD1160);put<uint8_t>(N+8,1);
  put<uint8_t>(G+0xBE8,0);put<uint32_t>(G+0xC18,0);put<uint32_t>(G+0xC28,0);mode(false);
 }
};
#ifndef BOTW_SORT_FIXTURE_ONLY
int main(){unsigned cases{};
 for(int cat=0;cat<7;++cat){SortFixture f;botw::SelectionBridge b;b.configure(&f.extensions);b.poll(f.h,{});assert(b.sort_supported);assert(b.sort_mode(cat)==0);++cases;
  const auto held=botw::held_tray(f.h);assert(held.valid&&held.count==0);
  assert(b.request_sort(cat));assert(f.mail<uint32_t>(20)==14&&f.mail<uint32_t>(24)==unsigned(cat));
  assert(f.mail<uint64_t>(32)==f.Manager&&f.mail<uint64_t>(48)==f.U);assert(!b.request_sort(cat));++cases;
  // A successful guest answer with no native state change is never optimistic success.
  f.mail<uint32_t>(16,1);f.mail<uint32_t>(12,b.pending);b.poll(f.h,{});assert(std::string(b.status)=="Sorting"&&b.pending);b.requested-=std::chrono::seconds(4);b.poll(f.h,{});assert(std::string(b.status)=="Sort not confirmed");++cases;
  assert(b.request_sort(cat));f.put<int32_t>(f.Manager+0x44800,cat);if(cat<4)f.mode(true);
  f.mail<uint32_t>(16,1);f.mail<uint32_t>(12,b.pending);b.poll(f.h,{});assert(std::string(b.status)=="Sorted");++cases;
 }
 for(int reason=0;reason<6;++reason){SortFixture f;botw::SelectionBridge b;b.configure(&f.extensions);b.poll(f.h,{});
  if(reason==0){f.mail<uint32_t>(0,0x484F5544);b.poll(f.h,{});}
  if(reason==1)f.texts["__page"]="map";
  if(reason==2)f.values["__mailbox_epoch"]=2;
  if(reason==3)f.slot(0,f.Apple);
  if(reason==4)f.put<uint64_t>(f.U+0x64C30,0x1E09000004ULL);
  if(reason==5)b.heartbeat_at-=std::chrono::seconds(3);
  assert(!b.request_sort(4));assert(f.mail<uint32_t>(8)==0);++cases;
 }
 assert(std::string(botw::SelectionBridge::sort_label(3,1))=="Sort: Set");++cases;
 std::cout<<cases<<" native sort request, identity, flags, acknowledgement and blocked-state cases passed\n";
}

#endif
