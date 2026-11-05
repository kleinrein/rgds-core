# Mesa's profile

`profile-<version>.tar.gz` holds GCC's counters from OutRun 2006 races on an instrumented `libgallium`. The package
unpacks it into its build folder, and `-Db_pgo=use` with LTO builds Mesa from it. Measured gain and numbers:
`docs/PERFORMANCE.md`, "Mesa with LTO and PGO".

The counters only match the exact source and flags they came from, so retrain after any Mesa upgrade or new Mesa patch.

In the build VM, after the package has been built once with the new source:

```bash
bash os/buildroot/package/mesa3d-panfrost/pgo/train.sh generate
```

Copy `~/own-os/mesa-opt/out/gen/libgallium-<version>.so` to `/data/rgds/psp-spike/mo-gen/` on the handheld and run two
races with it, 115 s each, the dump at 105 s:

```bash
A=GCOV_PREFIX=/run/rgds/gcda:GCOV_PREFIX_STRIP=0:RGDS_GCOV_AFTER=105
sh /data/rgds/psp-spike/mo.sh 115 train-1:mo-gen:$A train-2:mo-gen:$A
```

The log must say `rgds pgo: profile dumped`. Tar `/run/rgds/gcda/<the build folder's full path>` with nothing else on
the stream, pass it to `train.sh pack`, and put the result here as `profile-<version>.tar.gz`.

Check the package's build output afterwards: "source locations ... changed" is expected, for the generated
`glapi_mapi_tmp.h` only, and any other `-Wcoverage-mismatch` means the profile doesn't fit.

Traps: Meson takes `-Dc_args` as a list or spaces, and a comma list silently loses every flag. Ninja does not rebuild when a
`.gcda` changes. `-Wno-missing-profile` hides an empty profile, so confirm the library grew from 15.5 MB to about 18 MB.
