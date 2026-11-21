// SPDX-License-Identifier: GPL-3.0-or-later
#include "za_guest_mailbox.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <cstdio>

struct Memory {
    std::array<za::u8,4096> bytes{};
    template<class T> T Read(unsigned at){T value{};std::memcpy(&value,bytes.data()+at,sizeof(value));return value;}
    template<class T> void Put(unsigned at,T value){std::memcpy(bytes.data()+at,&value,sizeof(value));}
};
int main(){
    Memory memory;
    EdenDsmodHostExtensions ext{};
    ext.mailbox_address=0x10000;ext.mailbox_size=4096;ext.userdata=&memory;
    ext.load_u32=[](void* p,uint32_t at,uint32_t* value)->EdenDsmodBool{if(at>4092)return false;*value=static_cast<Memory*>(p)->Read<uint32_t>(at);return true;};
    ext.load_u64=[](void* p,uint32_t at,uint64_t* value)->EdenDsmodBool{if(at>4088)return false;*value=static_cast<Memory*>(p)->Read<uint64_t>(at);return true;};
    ext.store_u32=[](void* p,uint32_t at,uint32_t value)->EdenDsmodBool{if(at>4092)return false;static_cast<Memory*>(p)->Put(at,value);return true;};
    ext.store_u64=[](void* p,uint32_t at,uint64_t value)->EdenDsmodBool{if(at>4088)return false;static_cast<Memory*>(p)->Put(at,value);return true;};
    za::GuestMailbox box;box.Configure(&ext);
    memory.Put(4092,uint32_t{7});memory.Put(8,uint32_t{100});
    assert(!box.RequestMarker(std::numeric_limits<float>::quiet_NaN(),0,false,true));
    assert(!box.RequestMarker(2048,2048,false,true));
    assert(box.RequestMarker(250,-500,false,true));
    assert(memory.Read<float>(0x138)==250&&memory.Read<float>(0x13c)==-500);
    assert(memory.Read<uint32_t>(0x104)==7&&memory.Read<uint32_t>(0x108)==280);
    assert(memory.Read<uint32_t>(0x10c)==2&&memory.Read<uint32_t>(0x100)==1);
    memory.Put(0x140,uint64_t{0x345000});
    assert(!box.MarkerMapModel());
    assert(!box.RequestMarker(0,0,true,false));
    memory.Put(4092,uint32_t{8});box.Poll(1);
    assert(!memory.Read<uint32_t>(0x100)&&!memory.Read<uint32_t>(0x10c));
    assert(box.TravelStatus()==za::GuestMailbox::TravelResult::Expired);
    assert(!box.MarkerMapModel());
    assert(box.RequestMarker(0,0,true,false));
    assert(memory.Read<uint32_t>(0x10c)==3&&!memory.Read<uint32_t>(0x100));
    memory.Put(8,uint32_t{281});box.Poll(2);
    assert(!memory.Read<uint32_t>(0x10c));
    assert(box.RequestMapOpen());
    assert(memory.Read<uint32_t>(0x100)==1 && !memory.Read<uint32_t>(0x10c) && !box.Idle());
    memory.Put(4092,uint32_t{9});box.Poll(3);
    assert(!memory.Read<uint32_t>(0x100) && box.Idle());
    assert(box.RequestMarker(250,-500,false,false));
    assert(!memory.Read<uint64_t>(0x140));
    memory.Put(0x140,uint64_t{0x345000});
    memory.Put(0x120,uint32_t{4});
    memory.Put(0x10c,uint32_t{0});
    assert(box.MarkerMapModel()==0x345000);

    za::Entity member{};
    member.checksum_ok=true;
    member.encryption_constant=0x12345678;
    member.checksum=0x4321;
    za::u32 sequence{};
    assert(box.Request(za::GuestMailbox::Operation::HeldItem,2,150,149,member,nullptr,&sequence));
    assert(sequence==1&&memory.Read<za::u32>(20)==3&&memory.Read<za::u32>(24)==2);
    assert(memory.Read<za::u32>(28)==150&&memory.Read<za::u32>(32)==149);
    assert(memory.Read<za::u64>(40)==0x4321000012345678ULL);
    assert(box.BagResult(sequence)==za::GuestMailbox::OperationResult::Pending);
    memory.Put(4,sequence);
    memory.Put(76,za::u32{1});
    assert(box.BagResult(sequence)==za::GuestMailbox::OperationResult::Pending);
    memory.Put(72,sequence);
    assert(box.BagResult(sequence)==za::GuestMailbox::OperationResult::Accepted);
    assert(box.Request(za::GuestMailbox::Operation::HeldItem,2,0,150,member,nullptr,&sequence));
    assert(sequence==2&&box.BagResult(sequence)==za::GuestMailbox::OperationResult::Pending);
    memory.Put(76,za::u32{0});
    memory.Put(72,sequence);
    assert(box.BagResult(sequence)==za::GuestMailbox::OperationResult::Rejected);
    assert(!box.Request(za::GuestMailbox::Operation::Medicine,2,22,0x100d7,member,nullptr,&sequence,0xd7));
    memory.Put(4,sequence);
    assert(box.Request(za::GuestMailbox::Operation::Medicine,2,22,0x100d7,member,nullptr,&sequence,0xd7));
    assert(memory.Read<za::u32>(20)==4&&memory.Read<za::u32>(28)==22);
    assert(memory.Read<za::u32>(32)==0x100d7&&memory.Read<za::u64>(48)==0xd7);
    std::puts("za-mailbox: ok");
}
