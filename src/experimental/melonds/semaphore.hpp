#pragma once
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <system_error>
#ifdef __linux__
#include <semaphore.h>
#include <time.h>
#endif

namespace rgds_melonds {
// The Linux experiment keeps the existing backend as the default. Every
// semaphore chooses its backend at creation and retains it for its lifetime.
class Semaphore {
    std::mutex lock;
    std::condition_variable changed;
    unsigned count = 0;
    bool posix = false;
#ifdef __linux__
    sem_t native{};
    static void failed(const char* operation) { throw std::system_error(errno, std::generic_category(), operation); }
#endif
public:
    explicit Semaphore(bool requested = false) {
#ifdef __linux__
        if (requested) {
            if (sem_init(&native, 0, 0) != 0) failed("sem_init");
            posix = true;
        }
#else
        (void)requested;
#endif
    }
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    ~Semaphore() {
#ifdef __linux__
        if (posix) sem_destroy(&native);
#endif
    }
    bool uses_posix() const { return posix; }
    void reset() {
#ifdef __linux__
        if (posix) {
            int available = 0;
            if (sem_getvalue(&native, &available) != 0) failed("sem_getvalue");
            // Bound the drain to the initial snapshot, like upstream's
            // acquire(available()). Concurrent producers cannot extend it.
            for (int i = 0; i < available && try_wait(0); ++i) {}
            return;
        }
#endif
        std::lock_guard<std::mutex> guard(lock);
        count = 0;
    }
    void wait() {
#ifdef __linux__
        if (posix) {
            while (sem_wait(&native) != 0) {
                if (errno != EINTR) failed("sem_wait");
            }
            return;
        }
#endif
        std::unique_lock<std::mutex> guard(lock);
        changed.wait(guard, [&] { return count > 0; });
        --count;
    }
    bool try_wait(int timeout_ms) {
#ifdef __linux__
        if (posix) {
            timespec deadline{};
            if (timeout_ms > 0) {
                if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) failed("clock_gettime");
                deadline.tv_sec += timeout_ms / 1000;
                deadline.tv_nsec += long(timeout_ms % 1000) * 1000000;
                if (deadline.tv_nsec >= 1000000000) {
                    deadline.tv_nsec -= 1000000000;
                    ++deadline.tv_sec;
                }
            }
            for (;;) {
                int result = timeout_ms > 0 ? sem_timedwait(&native, &deadline) : sem_trywait(&native);
                if (result == 0) return true;
                if (errno == EINTR) continue; // Reuse the deadline after signals.
                if (errno == EAGAIN || errno == ETIMEDOUT) return false;
                failed("sem_timedwait/sem_trywait");
            }
        }
#endif
        std::unique_lock<std::mutex> guard(lock);
        if (!changed.wait_for(guard, std::chrono::milliseconds(timeout_ms), [&] { return count > 0; })) return false;
        --count;
        return true;
    }
    void post(int permits = 1) {
        if (permits <= 0) return;
#ifdef __linux__
        if (posix) {
            for (int i = 0; i < permits; ++i)
                if (sem_post(&native) != 0) failed("sem_post");
            return;
        }
#endif
        {
            std::lock_guard<std::mutex> guard(lock);
            count += unsigned(permits);
        }
        changed.notify_all();
    }
};
}
