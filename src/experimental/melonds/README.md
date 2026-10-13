# RGDS melonDS frontend

The original SDL frontend and synthetic cartridge/probe in this directory are
GPL-3.0-or-later. melonDS is pinned by commit, tree and source-file fingerprint
in `tools/build_melonds.py`. Upstream source is unmodified. Platform networking,
DSi/camera, Slot-2, add-ons and filesystem extensions are not implemented.

From RGDS, `make build-melonds` prepares the shared SDL link SDK and builds both
the playable Linux ARM64 frontend and headless core probe. SDL is dynamically
linked; the dummy SDL link SDK must never be installed on the handheld.

The distributed `source.tar.gz` contains `melonDS/` (all upstream tracked files,
including notices) and `rgds/` (original frontend and build drivers). To rebuild
from that archive, install Zig 0.13, CMake and Ninja, provide an ARM64 SDL2 SDK
with `include/SDL2/SDL.h` and `lib/libSDL2.so`, and run from the archive's `rgds/`:

```
python3 tools/build_melonds.py --source ../melonDS --archived-source --frontend --sdl-sdk /path/to/sdk
```

`--archived-source` validates the same fingerprint of every upstream file without
requiring Git metadata or a network download. Modified, missing or extra source
files fail validation. Normal checkout builds also check the commit, tree and
clean working copy; `--fetch` obtains that pinned checkout when needed.

The frontend takes `--rom`, `--save` and `--state` paths. It directly boots NDS
software with upstream FreeBIOS/generated firmware. RGDS supplies the shared
menu, control mapping, hardware touch bridge, shaders, fast-forward shortcut,
sleep and audio-volume lifecycle. The native frontend provides F5/F7 checkpoints,
Tab speed toggle, battery-save persistence, audio and microphone capture.

`tools/probe_melonds_frontend.py --output <fresh-directory>` runs inside the local
Linux ARM64 VM with dummy SDL video/audio, the built frontend and built RGDS
bridge. Its original homebrew tests do not certify physical RG DS hardware or
retail-game compatibility. Hardware acceptance is tracked separately.
