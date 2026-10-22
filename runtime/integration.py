"""Native RGDS bridge selection. Engine/vendor libraries are separate backends."""
import os
from pathlib import Path
from typing import NamedTuple, Optional

from engines import CORES

SDL_NAMES = ("libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so")
SYSTEM_LIBRARY_DIRS = (Path("/usr/lib"), Path("/lib"), Path("/mnt/ports/libs/lib64"), Path("/mnt/ports/libs/SDL64"))


def engine_libraries(environment, private=(), vendor=(), stock_independent=False):
    """Use the same SDL object in the dynamic loader and the RGDS bridge.

    Passing renderer/window pointers between two different SDL installations is
    invalid. A session's explicit SDL selection must remain in the engine's
    search path after the inherited compatibility environment is discarded.
    """
    directories = [Path(path) for path in (*private, *vendor)]
    selected = environment.get("RGDS_SDL_LIBRARY")
    selected = Path(selected) if selected and Path(selected).is_file() else None
    system = (
        Path('/usr/lib'), Path('/lib'), Path('/usr/lib/aarch64-linux-gnu'), Path('/lib/aarch64-linux-gnu')
    ) if stock_independent else SYSTEM_LIBRARY_DIRS
    if stock_independent and selected is not None and any(
        str(path).startswith(('/mnt/vendor/', '/mnt/ports/', '/oem/')) for path in (selected, selected.resolve())
    ):
        raise ValueError(
            'This engine requires an OS SDL2 library or a bundled library, not a stock/ports SDL override.'
        )
    if selected is None:
        selected = next(
            (
                directory / name for directory in (*directories, *system)
                for name in SDL_NAMES if (directory / name).is_file()
            ), None
        )
    if selected is not None:
        environment["RGDS_SDL_LIBRARY"] = str(selected)
        # Pin SDL first, then resolve engine-private dependencies. The verified
        # NNDDSS payload has shims, but deliberately contains no replacement SDL.
        directories.insert(0, selected.parent)
    else:
        environment.pop("RGDS_SDL_LIBRARY", None)
    directories.extend(system)
    environment["LD_LIBRARY_PATH"] = ":".join(dict.fromkeys(str(path) for path in directories))


class Hooks(NamedTuple):
    bridge: Optional[Path]


def select_hooks(core, directory, environment=None):
    if core not in CORES:
        raise ValueError("Unknown emulator: " + core)
    environment = os.environ if environment is None else environment
    if environment.get("RGDS_DISABLE_HOOKS") == "1":
        return Hooks(None)
    bridge = Path(directory) / "librgdsbridge.so"
    return Hooks(bridge if bridge.is_file() else None)
