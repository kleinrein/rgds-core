# Known issues

## Hotel Dusk: Room 215 (AWIx) crashes during boot

Status: open. Same failure in upstream DSperate. Stock DraStic runs it at full
speed, so RG DS Core starts this game in DraStic by default
(`runtime/library.py`, `COMPATIBILITY`).

What happens:

- About 1.6 s after boot (≈29.27 million ARM9 instructions, frame ~99), a
  function in the game's ITCM code at `0x01FF8xxx` returns through
  `ldmfd sp!, {r4-r11, pc}` and pops `pc = 0`.
- At that point `r4`/`r5` hold float constants (0.01 and 1.333), and the tail of
  the function stores through `r4` as if it were a pointer. The stack frame it
  pops was never written: a watch on the DTCM word (`0x027E3B84`) shows no
  write during the whole run, although DTCM is mapped correctly
  (`cp15 dtcm 027e000a`, 16 KB at `0x027E0000`). So SP at the epilogue differs
  from SP at the prologue.
- From address 0 the ARM9 runs into ITCM filled with `0x7FFF7FFF` (white
  pixels), which decodes as a conditional `SWI`. It slides through memory one
  SWI at a time, through every ITCM mirror, until it leaves mapped memory at
  `0x80000000`.
- In the JIT this shows as ~74 000 new translations per frame and an arena reset
  every ~3 frames (`DS_JIT_CHURN=1`), so the game runs at 1 fps instead of
  showing a black screen quickly.

Ruled out:

- The recompiler: the interpreter (`--interp`) fails identically.
- FreeBIOS: the real Nintendo BIOS dumps fail at the same instruction count.
- The divider and square-root units: they match GBATEK, including the
  divide-by-zero results.
- Code loading: the instructions in RAM match the ROM's ARM9 binary.

Next step: a differential trace against melonDS from boot to the divergence.
The function that derails calls the hardware square root (`0x040002B0`) in a
loop, so DS math or timing state there is the first thing to compare.

Reproduce with the headless engine:

```sh
DS_TRAP_WILD=2L dsperate-headless "Hotel Dusk - Room 215.nds" --direct --interp \
  --frames 160 --trace /dev/null --max 0
```

`DS_TRAP_WILD` keeps the last 64K ARM9 instructions and prints them at the first
fetch outside mapped code (`=1`), also from odd ITCM mirrors (`=2`), or also
from low ITCM (`=2L`). `DS_TRAP_FOCUS=lo-hi` limits the dump to a PC range, and
`DS_WATCH_DTCM=<address>` reports every change of one DTCM word.
