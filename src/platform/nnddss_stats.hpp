#pragma once
#include <cstdint>
#include <cstring>
#ifdef __linux__
#include <link.h>
#endif
namespace rgds {
// The pinned loader's private frame counter is not a dynamic ELF symbol.
// Verify its accessor instructions AND readable PT_LOAD bounds before reading.
// Unknown loaders simply retain presentation statistics.
inline const volatile uint32_t* nnddss_frame_counter(){
#if defined(__linux__) && defined(__aarch64__)
    const volatile uint32_t* result=nullptr;
    dl_iterate_phdr([](dl_phdr_info* info,size_t,void* output){
        if(info->dlpi_name && *info->dlpi_name)return 0;
        constexpr uintptr_t accessor=0x11c50,counter=0x60e90;
        bool code=false,data=false;
        for(int i=0;i<info->dlpi_phnum;i++){
            const auto& p=info->dlpi_phdr[i];if(p.p_type!=PT_LOAD || !(p.p_flags&PF_R))continue;
            code|=(p.p_flags&PF_X) && accessor>=p.p_vaddr && accessor+16<=p.p_vaddr+p.p_memsz;
            data|=(p.p_flags&PF_W) && counter>=p.p_vaddr && counter+4<=p.p_vaddr+p.p_memsz;
        }
        const uint32_t expected[]{0xf0000260,0x913a0000,0xb9401000,0xd65f03c0};
        if(code && data && !std::memcmp(reinterpret_cast<void*>(info->dlpi_addr+accessor),expected,sizeof(expected)))
            *static_cast<const volatile uint32_t**>(output)=reinterpret_cast<const volatile uint32_t*>(info->dlpi_addr+counter);
        return 1;
    },&result);
    return result;
#else
    return nullptr;
#endif
}
class FrameCounterRate {
    uint64_t since=0;
    uint32_t previous=0;
public:
    double fps=0;
    void reset(){since=0;previous=0;fps=0;}
    bool sample(uint64_t us,uint32_t frames,bool blocked){
        if(blocked){since=0;return false;}
        if(!since){since=us;previous=frames;return false;}
        if(us-since<1000000)return false;
        const uint32_t delta=frames-previous;
        fps=delta*1000000.0/(us-since);since=us;previous=frames;return true;
    }
};
}
