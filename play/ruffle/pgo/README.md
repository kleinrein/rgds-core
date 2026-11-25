# The Flash core's profile

`flash.profdata` is what the core's hot paths were on the RG DS on 2026-11-24: `tools/build_ruffle.py` builds with it
(`-C profile-use`) unless `--no-pgo` is given. HappyWheels' level, where Ruffle's ActionScript 3 interpreter is nearly
all of the time, runs its script 7% faster with it (20.1 -> 18.7 ms a call; 71% -> 76% of full speed).

A profile from older code still helps and never breaks a build (functions that changed are built without it), so it
only needs making again when the numbers matter:

    python3 tools/build_ruffle.py --headless --pgo-generate /data/rgds/flash-core/pgo
    # on the handheld, with build/ruffle/flash-headless: a HappyWheels level (5400 frames), a Bloons level (2400),
    # Swords and Sandals' title (900); docs/PLAY.md "Flash" has the scripted taps
    llvm-profdata merge --sparse -o play/ruffle/pgo/flash.profdata *.profraw     # rustup's llvm-tools, same toolchain
    python3 tools/build_ruffle.py --headless

The instrumented build is three times the size and a third slower: it is for the training runs only.
