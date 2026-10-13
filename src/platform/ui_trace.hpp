#pragma once
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace rgds {
// Optional single-writer trace in /run (tmpfs). Frame checkpoints only update
// shared memory: no allocations, logging, filesystem writes or fsync per frame.
class UiTrace {
    struct Record {
        uint32_t sequence,version,pid,stage;
        uint64_t frames,monotonic_ms;
        uint32_t page,reserved;
    };
    static_assert(sizeof(Record)==40,"Diagnostic trace ABI");
    Record* record=nullptr;
public:
    enum Stage {Loop=1,TopDraw,TopFlush,TopPresent,BottomDraw,BottomFlush,
                BottomPresent,FrameDone,Playing,Assets,Events};
    void open(const char* path) {
        if(!path || !*path || record)return;
        int fd=::open(path,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return;
        struct stat info{};
        if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() || ftruncate(fd,sizeof(Record))){::close(fd);return;}
        auto* mapping=mmap(nullptr,sizeof(Record),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        ::close(fd);if(mapping==MAP_FAILED)return;
        record=static_cast<Record*>(mapping);
        __atomic_store_n(&record->sequence,1,__ATOMIC_SEQ_CST);
        record->version=1;record->pid=getpid();record->stage=Loop;
        record->frames=record->monotonic_ms=0;record->page=record->reserved=0;
        __atomic_store_n(&record->sequence,2,__ATOMIC_RELEASE);
    }
    void mark(Stage stage,uint64_t frames,uint32_t page) {
        if(!record)return;
        auto sequence=__atomic_load_n(&record->sequence,__ATOMIC_RELAXED);
        __atomic_store_n(&record->sequence,sequence+1,__ATOMIC_SEQ_CST);
        timespec now{};clock_gettime(CLOCK_MONOTONIC,&now);
        record->stage=stage;record->frames=frames;record->page=page;
        record->monotonic_ms=uint64_t(now.tv_sec)*1000+now.tv_nsec/1000000;
        __atomic_store_n(&record->sequence,sequence+2,__ATOMIC_RELEASE);
    }
    ~UiTrace(){if(record)munmap(record,sizeof(Record));}
};
}
