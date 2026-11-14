// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <new>
#include <vector>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/mman.h>
#endif
namespace re4::assets {
// Large, short-lived decode buffers must return their pages on release. Otherwise
// libc's adaptive mmap threshold can retain a whole inflate/raster burst in its
// heap after the module has freed it, exceeding the asset resident-memory budget.
// This allocator is module-local; it never changes the host allocator's settings.
template<class T> struct AssetAllocator {
    using value_type=T;
    using is_always_equal=std::true_type;
    AssetAllocator()=default;
    template<class U> AssetAllocator(const AssetAllocator<U>&){}
    T* allocate(size_t n){
        if(n>std::numeric_limits<size_t>::max()/sizeof(T))throw std::bad_alloc();
        const size_t bytes=n*sizeof(T);
#if defined(__linux__) || defined(__ANDROID__)
        if(bytes>=128u*1024){
            void* p=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(p==MAP_FAILED)throw std::bad_alloc();return static_cast<T*>(p);
        }
#endif
        return static_cast<T*>(::operator new(bytes));
    }
    void deallocate(T* p,size_t n){
#if defined(__linux__) || defined(__ANDROID__)
        if(n*sizeof(T)>=128u*1024){munmap(p,n*sizeof(T));return;}
#endif
        ::operator delete(p);
    }
    template<class U> bool operator==(const AssetAllocator<U>&)const{return true;}
};
template<class T> using AssetBuffer=std::vector<T,AssetAllocator<T>>;
using ByteBuffer=AssetBuffer<uint8_t>;
}
