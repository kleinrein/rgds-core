# Firmware built into the kernel

`regulatory.db` and `regulatory.db.p7s` are the Wi-Fi regulatory database and its signature from wireless-regdb
2026.05.30 (ISC licence, https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git), the same bytes
Buildroot's `wireless-regdb` package installs under `/lib/firmware`.

They are compiled into the kernel (`CONFIG_EXTRA_FIRMWARE` in `linux-rgds.fragment`, copied into the kernel tree by
`external.mk`) because cfg80211 is built in and asks for the database before the root filesystem is mounted. From
the card it never loaded: the request failed, the sysfs fallback waited 60 s for a helper the OS does not have, and
the chip ran with the world channel plan (no active scanning on 5 GHz, channels 12 and 13 off) on every boot.

`post-build.sh` fails the build when these files differ from the package's: bump both together.
