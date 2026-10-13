#pragma once
// Adapted from JorreFog/ROCKNIXDS patch 5e14c95a (SuperDrastic helper door).
// Copyright (c) 2026 JorreFog and the ROCKNIXDS contributors. MIT; full notice
// in THIRD_PARTY.md. RGDS adds exact executable identity gating, owned gate
// lifetimes, successful-init counting and a monotonic timeout.
#ifdef __linux__
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <memory>
#include <new>
#include <pthread.h>
#include <time.h>

namespace rgds::drastic_threads {
inline uint64_t now_ns() {
    timespec value{};clock_gettime(CLOCK_MONOTONIC,&value);
    return uint64_t(value.tv_sec)*1000000000+value.tv_nsec;
}
inline int identify(dl_phdr_info* info,size_t,void* result) {
    // Only the main executable, never a core library with similar offsets.
    if(info->dlpi_name && *info->dlpi_name)return 0;
    constexpr unsigned char expected[]={0x7a,0x5e,0x0e,0x5f,0xc6,0xe5,0x2e,0x6e,0x8f,0x54,0x99,0xc3,0xd4,0xd6,0x67,0xef,0x51,0xdb,0x07,0x48};
    for(unsigned i=0;i<info->dlpi_phnum;i++){
        const auto& ph=info->dlpi_phdr[i];if(ph.p_type!=PT_NOTE)continue;
        const auto* at=reinterpret_cast<const unsigned char*>(info->dlpi_addr+ph.p_vaddr);
        size_t left=ph.p_memsz;
        while(left>=sizeof(ElfW(Nhdr))){
            ElfW(Nhdr) note{};std::memcpy(&note,at,sizeof(note));at+=sizeof(note);left-=sizeof(note);
            size_t names=(uint64_t(note.n_namesz)+3)&~uint64_t(3),desc=(uint64_t(note.n_descsz)+3)&~uint64_t(3);
            if(names>left || desc>left-names)break;
            if(note.n_type==NT_GNU_BUILD_ID && note.n_namesz==4 && note.n_descsz==sizeof(expected) && !std::memcmp(at,"GNU",4) && !std::memcmp(at+names,expected,sizeof(expected))){
                *static_cast<uintptr_t*>(result)=info->dlpi_addr;return 1;
            }
            at+=names+desc;left-=names+desc;
        }
    }
    return 1;
}
inline uintptr_t executable_base() {
    const char* core=std::getenv("RGDS_CORE");const char* disabled=std::getenv("RGDS_DRASTIC_HELPER_GATE");
    if(!core || std::strcmp(core,"drastic") || (disabled && !std::strcmp(disabled,"0")))return 0;
    uintptr_t base=0;dl_iterate_phdr(identify,&base);return base;
}
struct Gate {
    void* (*run)(void*);void* argument;
    std::atomic<bool> ready{false};unsigned initialized=0;
    Gate(void* (*fn)(void*),void* arg):run(fn),argument(arg){}
};
inline thread_local std::shared_ptr<Gate> pending;
inline void* start(void* pointer) {
    auto gate=std::move(*static_cast<std::shared_ptr<Gate>*>(pointer));
    delete static_cast<std::shared_ptr<Gate>*>(pointer);
    const uint64_t deadline=now_ns()+2000000000;
    while(!gate->ready.load(std::memory_order_acquire) && now_ns()<deadline){timespec delay{0,100000};nanosleep(&delay,nullptr);}
    if(!gate->ready.load(std::memory_order_acquire))
        std::fprintf(stderr,"RGDS: DraStic helper initialization timed out; using original startup behavior\n");
    return gate->run(gate->argument);
}
}

extern "C" int pthread_create(pthread_t* thread,const pthread_attr_t* attributes,void* (*fn)(void*),void* argument) noexcept {
    using namespace rgds::drastic_threads;
    static auto real=reinterpret_cast<decltype(&pthread_create)>(dlsym(RTLD_NEXT,"pthread_create"));
    if(!real)return EAGAIN;
    static const uintptr_t base=executable_base();
    const uintptr_t offset=reinterpret_cast<uintptr_t>(fn)-base;
    if(!base || (offset!=0x2fa50 && offset!=0x58e50))return real(thread,attributes,fn,argument);
    try {
        auto gate=std::make_shared<Gate>(fn,argument);
        auto* transfer=new std::shared_ptr<Gate>(gate);
        pending=gate;
        int result=real(thread,attributes,start,transfer);
        if(result){delete transfer;pending.reset();}
        return result;
    }catch(const std::bad_alloc&){return ENOMEM;}
}

extern "C" int pthread_cond_init(pthread_cond_t* condition,const pthread_condattr_t* attributes) noexcept {
    using namespace rgds::drastic_threads;
    static auto real=reinterpret_cast<decltype(&pthread_cond_init)>(dlsym(RTLD_NEXT,"pthread_cond_init"));
    if(!real)return EAGAIN;
    const int result=real(condition,attributes);
    if(result==0 && pending && ++pending->initialized==2){
        pending->ready.store(true,std::memory_order_release);pending.reset();
    }
    return result;
}
#endif
