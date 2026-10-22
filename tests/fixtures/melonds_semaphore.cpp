#include "experimental/melonds/semaphore.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#ifdef __linux__
#include <csignal>
#include <pthread.h>
static void harmless_signal(int) {}
#endif
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using rgds_melonds::Semaphore;
static double milliseconds(Clock::time_point began) {
    return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

static void exercise(bool requested) {
    Semaphore semaphore(requested);
#ifdef __linux__
    assert(semaphore.uses_posix() == requested);
#else
    assert(!semaphore.uses_posix());
#endif
    assert(!semaphore.try_wait(0));
    assert(!semaphore.try_wait(-1));
    semaphore.post(5);
    for (int i = 0; i < 5; ++i) assert(semaphore.try_wait(0));
    assert(!semaphore.try_wait(0));
    semaphore.post(192);
    semaphore.reset();
    assert(!semaphore.try_wait(0));
    auto began = Clock::now();
    assert(!semaphore.try_wait(30));
    assert(milliseconds(began) >= 20);
    std::thread delayed([&] {
        std::this_thread::sleep_for(10ms);
        semaphore.post();
    });
    assert(semaphore.try_wait(1000));
    delayed.join();
    assert(!semaphore.try_wait(0));

    // Scanline-style producer/consumer handoff must publish prior writes.
    Semaphore ready(requested), consumed(requested);
    unsigned payload = 0;
    began = Clock::now();
    std::thread producer([&] {
        for (unsigned n = 1; n <= 5000; ++n) {
            payload = n;
            ready.post();
            consumed.wait();
        }
    });
    for (unsigned n = 1; n <= 5000; ++n) {
        ready.wait();
        assert(payload == n);
        consumed.post();
    }
    producer.join();
    double handoff_ms = milliseconds(began);

    // Every permit is consumed once even with several concurrent waiters.
    std::atomic<unsigned> total{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; ++i)
        waiters.emplace_back([&] {
            for (int n = 0; n < 500; ++n) {
                semaphore.wait();
                ++total;
            }
        });
    semaphore.post(2000);
    for (auto& thread : waiters) thread.join();
    assert(total == 2000 && !semaphore.try_wait(0));

#ifdef __linux__
    if (requested) {
        struct sigaction action{}, previous{};
        action.sa_handler = harmless_signal;
        sigemptyset(&action.sa_mask);
        assert(sigaction(SIGUSR1, &action, &previous) == 0);
        std::atomic<bool> waiting{false}, finished{false};
        std::thread interrupted([&] {
            waiting = true;
            semaphore.wait();
            finished = true;
        });
        while (!waiting) std::this_thread::yield();
        std::this_thread::sleep_for(10ms);
        assert(pthread_kill(interrupted.native_handle(), SIGUSR1) == 0);
        std::this_thread::sleep_for(10ms);
        assert(!finished);
        semaphore.post();
        interrupted.join();
        assert(finished);
        // Signals must not shorten or restart a timed wait's deadline.
        waiting = false;
        double elapsed = 0;
        std::thread timed([&] {
            auto start = Clock::now();
            waiting = true;
            assert(!semaphore.try_wait(100));
            elapsed = milliseconds(start);
        });
        while (!waiting) std::this_thread::yield();
        for (int n = 0; n < 4; ++n) {
            std::this_thread::sleep_for(15ms);
            assert(pthread_kill(timed.native_handle(), SIGUSR1) == 0);
        }
        timed.join();
        assert(elapsed >= 85 && elapsed < 500);
        assert(sigaction(SIGUSR1, &previous, nullptr) == 0);
    }
#endif
    std::printf(
        "{\"backend\":\"%s\",\"requested\":%s,\"handoffs\":5000,\"handoff_ms\":%.3f,\"counts_reset_timeout_visibility\":true}\n",
        semaphore.uses_posix() ? "posix" : "condition-variable", requested ? "true" : "false", handoff_ms);
}
int main() {
    exercise(false);
    exercise(true);
}
