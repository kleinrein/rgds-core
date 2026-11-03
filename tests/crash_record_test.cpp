// Dies the way it is told to, for tests/test_crash_record.py.
#include "platform/crash_record.hpp"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace rgds;

[[gnu::noinline]] static void fault_inner() {
    *static_cast<volatile int*>(nullptr) = 1;
    asm("");
}
[[gnu::noinline]] static void fault_outer() {
    fault_inner();
    asm("");
}
[[gnu::noinline]] static int overflow(int depth) {
    volatile char filler[4096];
    filler[0] = char(depth);
    return overflow(depth + 1) + filler[0];
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::string kind = argv[2];
    crash::install(argv[1], "test", "test-release");
    crash::context.page = 7;
    crash::context.games = 6000;
    crash::context.collection_loading = 1;
    if (kind == "segv") fault_outer();
    else if (kind == "abort") std::abort();
    else if (kind == "stack") return overflow(0);
    else if (kind == "thread") std::thread(fault_outer).join();
    else if (kind == "throw") throw std::runtime_error("nobody caught this");
    else if (kind == "caught") {
        try {
            throw std::runtime_error("card is read-only");
        } catch (const std::exception& error) {
            crash::record_exception("exception", error.what());
            return 1;
        }
    } else if (kind == "fine") return 0;
    return 3;
}
