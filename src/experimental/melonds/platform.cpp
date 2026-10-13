// Shared NDS platform. The frontend owns ROM/save IO; networking, camera and
// filesystem add-ons are unavailable. The headless probe forbids persistence.
#ifdef RGDS_MELONDS_FRONTEND
#include "frontend_platform.hpp"
#endif
#include "Platform.h"
#include "semaphore.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace melonDS::Platform {
struct Thread { std::thread value; };
struct Mutex { std::mutex value; };
struct Semaphore : rgds_melonds::Semaphore { using rgds_melonds::Semaphore::Semaphore; };
void SignalStop(StopReason reason, void*) {
    #ifdef RGDS_MELONDS_FRONTEND
    rgds_melonds::stop(int(reason));
#else
    if (reason != External) { std::fprintf(stderr, "Unexpected emulation stop: %d\n", reason); std::abort(); }
#endif
}
void Log(LogLevel, const char* fmt, ...) {
    va_list args; va_start(args, fmt); std::vfprintf(stderr, fmt, args); va_end(args);
}
Thread* Thread_Create(std::function<void()> fn) { return new Thread{std::thread(std::move(fn))}; }
void Thread_Wait(Thread* t) { if (t->value.joinable()) t->value.join(); }
void Thread_Free(Thread* t) { Thread_Wait(t); delete t; }
Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* m) { delete m; }
void Mutex_Lock(Mutex* m) { m->value.lock(); }
void Mutex_Unlock(Mutex* m) { m->value.unlock(); }
bool Mutex_TryLock(Mutex* m) { return m->value.try_lock(); }
Semaphore* Semaphore_Create() {
    const char* setting=std::getenv("RGDS_MELONDS_POSIX_SEMAPHORE");
    auto* semaphore=new Semaphore(setting && std::strcmp(setting,"1")==0);
    static std::once_flag reported;
    std::call_once(reported,[semaphore]{Log(LogLevel::Info,"RGDS melonDS semaphore=%s\n",semaphore->uses_posix()?"posix-experiment":"condition-variable");});
    return semaphore;
}
void Semaphore_Free(Semaphore* s) { delete s; }
void Semaphore_Reset(Semaphore* s) { s->reset(); }
void Semaphore_Wait(Semaphore* s) { s->wait(); }
bool Semaphore_TryWait(Semaphore* s, int timeout_ms) { return s->try_wait(timeout_ms); }
void Semaphore_Post(Semaphore* s, int count) { s->post(count); }
void Sleep(u64 usecs) { std::this_thread::sleep_for(std::chrono::microseconds(usecs)); }
u64 GetUSCount() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
u64 GetMSCount() { return GetUSCount() / 1000; }

// The synthetic cartridge and checkpoint stay in memory. Refuse all file
// access so the probe cannot silently read BIOS, ROMs or another OS's files.
std::string GetLocalFilePath(const std::string& name) { return name; }
FileHandle* OpenFile(const std::string&, FileMode) { return nullptr; }
FileHandle* OpenLocalFile(const std::string&, FileMode) { return nullptr; }
bool FileExists(const std::string&) { return false; }
bool LocalFileExists(const std::string&) { return false; }
bool CheckFileWritable(const std::string&) { return false; }
bool CheckLocalFileWritable(const std::string&) { return false; }
bool CloseFile(FileHandle*) { return false; }
bool IsEndOfFile(FileHandle*) { return true; }
bool FileReadLine(char*, int, FileHandle*) { return false; }
u64 FilePosition(FileHandle*) { return 0; }
bool FileSeek(FileHandle*, s64, FileSeekOrigin) { return false; }
void FileRewind(FileHandle*) {}
u64 FileRead(void*, u64, u64, FileHandle*) { return 0; }
bool FileFlush(FileHandle*) { return false; }
u64 FileWrite(const void*, u64, u64, FileHandle*) { return 0; }
u64 FileWriteFormatted(FileHandle*, const char*, ...) { return 0; }
u64 FileLength(FileHandle*) { return 0; }

static void unsupported_write() { std::fputs("Unexpected persistent write in memory-only probe\n", stderr); std::abort(); }
void WriteNDSSave(const u8* data, u32 size, u32 offset, u32 length, void*) {
#ifdef RGDS_MELONDS_FRONTEND
    rgds_melonds::battery_write(data,size,offset,length);
#else
    (void)data;(void)size;(void)offset;(void)length;unsupported_write();
#endif
}
void WriteGBASave(const u8*, u32, u32, u32, void*) { unsupported_write(); }
void WriteFirmware(const Firmware&, u32, u32, void*) {
#ifndef RGDS_MELONDS_FRONTEND
    unsupported_write();
#endif
}
void WriteDateTime(int, int, int, int, int, int, void*) {}
void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }
int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }
void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*) { std::memset(frame, 0, size_t(width) * height * sizeof(u32)); }
void Mic_Start(void*) {
#ifdef RGDS_MELONDS_FRONTEND
    rgds_melonds::mic_start();
#endif
}
void Mic_Stop(void*) {
#ifdef RGDS_MELONDS_FRONTEND
    rgds_melonds::mic_stop();
#endif
}
int Mic_ReadInput(s16* data, int maximum, void*) {
#ifdef RGDS_MELONDS_FRONTEND
    return rgds_melonds::mic_read(data,maximum);
#else
    (void)data;(void)maximum;return 0;
#endif
}
AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }
bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0; }
DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }
}
