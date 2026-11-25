# The Dreamcast's 16 MB of main RAM out of a running Flycast (python3 dc_guest_ram.py PID OUT): Flycast keeps guest
# memory in an unlinked /dev/shm file, main RAM at offset 0, so it is read through the player's own mapping of it.
import sys

pid, out = sys.argv[1], sys.argv[2]
for line in open(f"/proc/{pid}/maps"):
    parts = line.split()
    if len(parts) > 5 and "dcnzorz_mem" in parts[5] and int(parts[2], 16) == 0:
        start, end = (int(x, 16) for x in parts[0].split("-"))
        if end - start >= 16 << 20:
            with open(f"/proc/{pid}/mem", "rb") as mem, open(out, "wb") as f:
                mem.seek(start)
                f.write(mem.read(16 << 20))
            print(f"guest RAM from {start:#x}")
            break
else:
    print("no 16 MB mapping of guest RAM at file offset 0")
