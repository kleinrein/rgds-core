# The source artwork of the system icons

The generated studio photographs the launcher's system bitmaps are cut from:
one 2048×2048 picture per system, photorealistic, three-quarter view, one
camera and one lighting setup for the whole set, no text and no logos.

They live here rather than in `assets/` because they are the source, not a
shipped asset. `assets/systems/*.bmp` is what the handheld draws; these are
89 MB of photographs that only the pipeline needs.

## Where they come from

`build/systems/systems_prompts.py` holds the brief for each system and
`build/systems/generate_all.py` calls imagegen with it. That part needs the
image service, so it is kept in the build tree rather than in the repository.

## What they become

```sh
python3 tools/prepare_system_icons.py                # artwork -> assets/systems/*.bmp
python3 tools/prepare_system_icons.py --sheet out.png # labelled review sheet
```

The tool cuts each photograph free of its studio backdrop
(`tools/system_matte.py`), crops it to the machine, scales it onto a 128×128
canvas with a 116 px optical bound and writes a 32-bit BGRA BMP.

Re-cutting the artwork does not need imagegen: the photographs are here.
