#pragma once
// What a crash leaves behind: a record appended to logs/crashes.log, with its summary on stderr (launcher.log too).
// tools/symbolize_crash.py turns a record's addresses into source lines with the symbols the device build kept.
// Everything after a fatal signal is async-signal-safe: plain write(2)s of numbers formatted by hand, nothing that
// allocates or locks, since the heap may be what broke. The frame walk probes each pointer through a pipe, where a bad
// address is an EFAULT and not a second fault.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <elf.h>
#include <link.h>
#include <sys/syscall.h>
#include <ucontext.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pthread.h>
#include <sys/ucontext.h>
#endif

namespace rgds::crash {
// Where the launcher was. Published as it runs and only read by the handler.
struct Context {
    volatile int page = -1, home = 0, library_ready = 0, collection_loading = 0;
    volatile size_t games = 0, all_games = 0;
};
inline Context context;

struct State {
    int file = -1, probe_read = -1, probe_write = -1;
    char mode[32] = "launcher", release[128] = "", build_id[64] = "";
    uintptr_t base = 0;
    std::atomic_flag writing = ATOMIC_FLAG_INIT;
    alignas(16) char stack[64 * 1024];
};
inline State state;
constexpr off_t keep_bytes = 1024 * 1024;
constexpr int fatal_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

// The record goes to the file, and all but the memory map to stderr as well.
inline void out(const char* text, bool summary = true) {
    const size_t length = std::strlen(text);
    if (state.file >= 0 && write(state.file, text, length) < 0) {}
    if (summary && write(STDERR_FILENO, text, length) < 0) {}
}
inline void out_number(uintmax_t value, unsigned base = 10) {
    char digits[32];
    size_t at = sizeof(digits) - 1;
    digits[at] = 0;
    do {
        digits[--at] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    out(digits + at);
}
inline void out_field(const char* name, uintmax_t value) {
    out(name);
    out_number(value);
}
inline void out_address(const char* name, uintptr_t value) {
    out(name);
    out("0x");
    out_number(value, 16);
}
inline void out_context() {
    out_address("\nbase=", state.base);
    out(" mode=");
    out(state.mode);
    out(" release=");
    out(state.release);
    out(" build_id=");
    out(state.build_id);
    out_field("\ntime=", uintmax_t(std::time(nullptr)));
    out_field(" page=", uintmax_t(intmax_t(context.page)));
    out_field(" home=", uintmax_t(context.home));
    out_field(" games=", context.games);
    out_field(" all_games=", context.all_games);
    out_field(" library_ready=", uintmax_t(context.library_ready));
    out_field(" collection_loading=", uintmax_t(context.collection_loading));
    out("\n");
}
inline bool readable(uintptr_t address, size_t bytes) {
    if (state.probe_write < 0 || bytes > 64) return false;
    if (write(state.probe_write, reinterpret_cast<const void*>(address), bytes) != ssize_t(bytes)) return false;
    char drain[64];
    return read(state.probe_read, drain, bytes) == ssize_t(bytes);
}
inline const char* signal_name(int number) {
    switch (number) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
    }
}
inline void walk_frames(const void* raw) {
    uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
    const auto* context_data = static_cast<const ucontext_t*>(raw);
#if defined(__linux__) && defined(__aarch64__)
    pc = context_data->uc_mcontext.pc;
    lr = context_data->uc_mcontext.regs[30];
    sp = context_data->uc_mcontext.sp;
    fp = context_data->uc_mcontext.regs[29];
#elif defined(__APPLE__) && defined(__aarch64__)
    pc = context_data->uc_mcontext->__ss.__pc;
    lr = context_data->uc_mcontext->__ss.__lr;
    sp = context_data->uc_mcontext->__ss.__sp;
    fp = context_data->uc_mcontext->__ss.__fp;
#else
    (void)context_data;
#endif
    out_address("pc=", pc);
    out_address(" lr=", lr);
    out_address(" sp=", sp);
    out_address(" fp=", fp);
    out("\n");
    // Frame records are {previous frame, return address}, climbing the stack. The link register comes second: a
    // fault in a leaf function has not stored it yet.
    int number = 0;
    auto frame = [&](uintptr_t address) {
        out_field("#", uintmax_t(number++));
        out_address(" ", address);
        out("\n");
    };
    if (pc) frame(pc);
    if (lr) frame(lr);
    while (number < 64 && fp && !(fp & 15) && readable(fp, sizeof(uintptr_t) * 2)) {
        const auto* record = reinterpret_cast<const uintptr_t*>(fp);
        const uintptr_t next = record[0], back = record[1];
        if (!back) break;
        if (back != lr) frame(back);
        lr = 0;
        if (next <= fp) break;
        fp = next;
    }
}
// Which library an address fell in: the launcher's own come from the symbols, the rest name their file.
inline void out_memory_map() {
#if defined(__linux__)
    const int map = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (map < 0) return;
    out("maps:\n", false);
    char block[4096];
    ssize_t count;
    size_t total = 0;
    while (total < 128 * 1024 && (count = read(map, block, sizeof(block))) > 0) {
        if (state.file >= 0 && write(state.file, block, size_t(count)) < 0) break;
        total += size_t(count);
    }
    close(map);
#endif
}
inline void on_fatal(int number, siginfo_t* info, void* raw) {
    // A second thread's crash waits here; the first one's re-raise ends the process.
    if (state.writing.test_and_set()) {
        for (;;) pause();
    }
    out("=== crash ===\nkind=signal signal=");
    out(signal_name(number));
    out_field(" code=", uintmax_t(intmax_t(info->si_code)));
    out_address(" fault=", reinterpret_cast<uintptr_t>(info->si_addr));
#if defined(__linux__)
    out_field(" thread=", uintmax_t(syscall(SYS_gettid)));
    out(syscall(SYS_gettid) == getpid() ? " main=1" : " main=0");
#elif defined(__APPLE__)
    out(pthread_main_np() ? " main=1" : " main=0");
#endif
    out_context();
    walk_frames(raw);
    out_memory_map();
    out("=== end ===\n");
    if (state.file >= 0) fsync(state.file); // a power-off right after the crash keeps the record
    // Delivered on return, with the default action: init sees the same death as without this handler.
    signal(number, SIG_DFL);
    raise(number);
}
// Something thrown that nothing caught, and what main() catches last: both end the launcher without a fault.
inline void record_exception(const char* kind, const char* what) {
    if (state.file < 0) return;
    out("=== crash ===\nkind=");
    out(kind);
    out(" what=");
    out(what);
    out_context();
    out("=== end ===\n");
    fsync(state.file);
}
inline void on_terminate() {
    const char* what = "not a standard exception";
    try {
        if (auto current = std::current_exception()) std::rethrow_exception(current);
        what = "no active exception";
    } catch (const std::exception& error) {
        what = error.what();
    } catch (...) {}
    record_exception("terminate", what);
    std::abort(); // the signal record adds where
}
#if defined(__linux__)
inline int find_program(dl_phdr_info* info, size_t, void*) {
    state.base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_type != PT_NOTE) continue;
        const auto* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + header.p_vaddr);
        const auto* end = at + header.p_memsz;
        while (at + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(at);
            const auto* name = at + sizeof(*note);
            const auto* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && !std::memcmp(name, "GNU", 4)) {
                size_t written = 0;
                for (unsigned byte = 0; byte < note->n_descsz && written + 2 < sizeof(state.build_id); byte++) {
                    state.build_id[written++] = "0123456789abcdef"[desc[byte] >> 4];
                    state.build_id[written++] = "0123456789abcdef"[desc[byte] & 15];
                }
                state.build_id[written] = 0;
                return 1;
            }
            at = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1; // the first object is the program
}
#endif
inline void copy_text(char* target, size_t size, const std::string& text) {
    std::strncpy(target, text.c_str(), size - 1);
    target[size - 1] = 0;
}
// Everything a crash needs is opened here: at that moment the process may have no descriptors or memory to spare.
inline void install(const std::filesystem::path& logs, const std::string& mode, const std::string& release) {
    copy_text(state.mode, sizeof(state.mode), mode);
    copy_text(state.release, sizeof(state.release), release);
#if defined(__linux__)
    dl_iterate_phdr(find_program, nullptr);
#elif defined(__APPLE__)
    state.base = uintptr_t(_dyld_get_image_vmaddr_slide(0));
#endif
    std::error_code ignored;
    std::filesystem::create_directories(logs, ignored);
    const auto path = logs / "crashes.log";
    const auto size = std::filesystem::file_size(path, ignored);
    if (!ignored && size > uintmax_t(keep_bytes)) {
        std::filesystem::rename(path, logs / "crashes.log.1", ignored);
    }
    state.file = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    int probe[2];
    if (pipe(probe) == 0) {
        fcntl(probe[0], F_SETFD, FD_CLOEXEC);
        fcntl(probe[1], F_SETFD, FD_CLOEXEC);
        state.probe_read = probe[0];
        state.probe_write = probe[1];
    }
    // Its own stack: running out of stack is itself a crash to record.
    stack_t stack{};
    stack.ss_sp = state.stack;
    stack.ss_size = sizeof(state.stack);
    sigaltstack(&stack, nullptr);
    struct sigaction action{};
    action.sa_sigaction = on_fatal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    for (int number : fatal_signals) sigaction(number, &action, nullptr);
    std::set_terminate(on_terminate);
}
} // namespace rgds::crash
