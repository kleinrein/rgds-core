/* Kernel costs a game pays, for comparing two kernels or command lines on the RG DS (docs/OWN_BSP_PLAN.md, Phase D:
 * ROCKNIX's debug and hardening defaults).
 *
 *   kbench [SECONDS_PER_TEST]   (default 2)
 *
 * Each test runs on CPU 3 (the emulation core) with a second thread on CPU 2 where it needs one, repeats for the
 * given time, and prints the median of 5 rounds:
 *   syscall    getppid(): the bare entry and exit
 *   fault      first touch of fresh anonymous memory then munmap, per 4 KiB page: zeroing on allocation and on free
 *              (init_on_alloc / init_on_free), the page allocator's locks
 *   pipe       a one-byte round trip between two threads on two CPUs: wakeups, the runqueue and pipe spinlocks
 *   futex      a FUTEX_WAKE with nobody waiting: the hash bucket spinlock
 *   mmap       mmap + munmap of 64 KiB untouched: the mm locks, VMA allocation and free
 *
 * Run it with the menu idle (or the session held), nothing else busy. Built with the cross compiler and Buildroot's
 * CFLAGS: aarch64-buildroot-linux-gnu-gcc -O2 -D_FORTIFY_SOURCE=1 -pthread -o kbench kbench.c
 */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double seconds = 2.0;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pin(int cpu)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof set, &set);
}

static double bench_syscall(void)
{
	long n = 0;
	double t0 = now(), t;
	do {
		for (int i = 0; i < 10000; i++)
			syscall(SYS_getppid);
		n += 10000;
		t = now();
	} while (t - t0 < seconds);
	return (t - t0) * 1e9 / n;
}

#define FAULT_BYTES (64 << 20)
static double bench_fault(void)
{
	long pages = 0;
	long page = sysconf(_SC_PAGESIZE);
	double t0 = now(), t;
	do {
		char *p = mmap(NULL, FAULT_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) {
			perror("mmap");
			exit(1);
		}
		for (long off = 0; off < FAULT_BYTES; off += page)
			p[off] = 1;
		munmap(p, FAULT_BYTES);
		pages += FAULT_BYTES / page;
		t = now();
	} while (t - t0 < seconds);
	return (t - t0) * 1e9 / pages;
}

static int ping[2], pong[2];
static void *echo(void *arg)
{
	char c;
	(void)arg;
	pin(2);
	while (read(ping[0], &c, 1) == 1) {
		if (c == 'q')
			break;
		if (write(pong[1], &c, 1) != 1)
			break;
	}
	return NULL;
}

static double bench_pipe(void)
{
	pthread_t th;
	char c = 'x';
	long n = 0;
	if (pipe(ping) || pipe(pong)) {
		perror("pipe");
		exit(1);
	}
	pthread_create(&th, NULL, echo, NULL);
	double t0 = now(), t;
	do {
		for (int i = 0; i < 1000; i++) {
			if (write(ping[1], &c, 1) != 1 || read(pong[0], &c, 1) != 1) {
				perror("pipe round trip");
				exit(1);
			}
		}
		n += 1000;
		t = now();
	} while (t - t0 < seconds);
	c = 'q';
	if (write(ping[1], &c, 1) != 1)
		perror("pipe stop");
	pthread_join(th, NULL);
	close(ping[0]);
	close(ping[1]);
	close(pong[0]);
	close(pong[1]);
	return (t - t0) * 1e9 / n;
}

static double bench_futex(void)
{
	static int word;
	long n = 0;
	double t0 = now(), t;
	do {
		for (int i = 0; i < 10000; i++)
			syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
		n += 10000;
		t = now();
	} while (t - t0 < seconds);
	return (t - t0) * 1e9 / n;
}

static double bench_mmap(void)
{
	long n = 0;
	double t0 = now(), t;
	do {
		for (int i = 0; i < 1000; i++) {
			void *p = mmap(NULL, 64 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED) {
				perror("mmap");
				exit(1);
			}
			munmap(p, 64 << 10);
		}
		n += 1000;
		t = now();
	} while (t - t0 < seconds);
	return (t - t0) * 1e9 / n;
}

static int by_value(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name, *unit;
		double (*run)(void);
	} tests[] = {
		{"syscall", "ns/call", bench_syscall}, {"fault", "ns/page", bench_fault}, {"pipe", "ns/round trip", bench_pipe},
		{"futex", "ns/call", bench_futex},     {"mmap", "ns/pair", bench_mmap},
	};
	if (argc > 1)
		seconds = atof(argv[1]);
	pin(3);
	for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
		double r[5];
		for (int k = 0; k < 5; k++)
			r[k] = tests[i].run();
		qsort(r, 5, sizeof r[0], by_value);
		printf("%-8s %9.1f %s  (min %.1f max %.1f)\n", tests[i].name, r[2], tests[i].unit, r[0], r[4]);
		fflush(stdout);
	}
	return 0;
}
