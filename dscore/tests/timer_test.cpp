// SPDX-License-Identifier: GPL-3.0-or-later
// Control writes around a pending timer overflow, without a ROM or BIOS.
#include "core/nds.h"
#include "check.h"
#include <memory>

using namespace ds;

static void rewrite_control(NDS& nds, u32 which) { nds.io.write(static_cast<Cpu>(which), 0x04000106, 16, 0xC1); }

static void test_control_rewrite(Cpu cpu, u64 when) {
  auto nds = std::make_unique<NDS>();
  nds->sched.reset();
  nds->cpu(Cpu::ARM9).halted = true;
  nds->cpu(Cpu::ARM7).halted = true;
  auto& registers = nds->io.cpu_io[cpu == Cpu::ARM9 ? 0 : 1];

  // Timer 1 overflows every 256 ARM9 cycles; timer 2 counts its wraps.
  nds->io.write(cpu, 0x04000108, 16, 0xFF00);
  nds->io.write(cpu, 0x0400010A, 16, 0xC4);
  registers.timers[2].counter = 0xFFFF;
  nds->io.write(cpu, 0x04000104, 16, 0xFFFE);
  nds->io.write(cpu, 0x04000106, 16, 0xC1);

  // HBlank has an earlier event ID than either CPU's timers, so a rewrite
  // at cycle 256 runs before the pending overflow's normal dispatch.
  nds->sched.schedule(EventId::HBlank, when, rewrite_control, static_cast<u32>(cpu));
  nds->sched.run_until(when);
  nds->sched.run_until(260);
  CHECK(registers.timers[1].counter == 0xFFFE);
  CHECK(registers.timers[2].counter == 0xFF00);
  CHECK(registers.if_ & (1u << (io::IRQ_TIMER0 + 1)));
  CHECK(registers.if_ & (1u << (io::IRQ_TIMER0 + 2)));

  // The settled event must fire once, then retain its original phase.
  nds->sched.run_until(512);
  CHECK(registers.timers[2].counter == 0xFF01);
}

int main() {
  for (Cpu cpu : {Cpu::ARM9, Cpu::ARM7}) {
    for (u64 when : {254, 256, 260}) {
      std::printf("timer control rewrite: %s at cycle %llu\n", cpu == Cpu::ARM9 ? "ARM9" : "ARM7",
                  static_cast<unsigned long long>(when));
      test_control_rewrite(cpu, when);
    }
  }
  std::puts("timer: reload, IRQ, cascade and phase passed");
  return 0;
}
