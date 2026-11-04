#!/usr/bin/env python3
# gw-libretro keeps its Lua as C arrays beside the source: writes one again after a patch to the .lua.
import sys
from pathlib import Path

source = Path(sys.argv[1])
name = "gwlua_lua_" + source.stem + "_lua"
data = source.read_bytes()
rows = ["  " + ", ".join("0x%02x" % b for b in data[i:i + 12]) for i in range(0, len(data), 12)]
source.with_suffix(".h").write_text("const unsigned char %s[] = {\n%s\n};\nconst unsigned int %s_len = %d;\n" % (name, ",\n".join(rows), name, len(data)))
