# Cheats

DSCore runs Action Replay codes from a `usrcheat.dat` database, the same file
DraStic and R4 cards use. The in-game menu lists the codes for the running game
behind the **Cheats** tile (bottom row, beside the game switcher), and each one
switches on or off while the game keeps running. Which codes are on is remembered per game.

## Where the database comes from

DSCore looks for the database in this order when a game starts:

1. `usrcheat.dat` beside your games (`Roms/NDS/usrcheat.dat`). Put your own
   copy there and it wins.
2. `userdata/emulators/rgds-experimental/cheats/usrcheat.dat`. On a stock card
   RG DS Core copies DraStic's database here the first time DSCore starts, so
   cheats work out of the box. On an independent install, or when DraStic has
   no database, drop a file here yourself.
3. DSCore's own fallback beside the configuration, for a hand-edited setup.

A game is matched by its game code and header checksum. When the database has
no entry for it, or the game runs with another engine, the Cheats bar stays
muted and says why.

## In the menu

Databases run to hundreds of codes for a popular game (Mario Kart DS has 483
in 31 groups, Pokémon HeartGold 1174), so the page is built for long lists:

- **Groups first.** The page opens on the database's groups, each with its code
  count or how many are on; codes outside any group are listed under *Other
  cheats*. A opens a group. A database without groups opens straight on its
  codes.
- **One group at a time.** Inside a group, **Left / Right** (or L / R) step to
  the previous or next group without going back. A group marked *One at a
  time* keeps a single code on: turning one on turns its neighbour off.
- **X searches** every code by name and description. The top screen shows the
  match count and the first names while you type; Start lists the matches, and
  an empty search lists every code flat. In a search or the on-list, Left /
  Right move a page of rows.
- **Y shows the cheats that are on**, across all groups, so you can review and
  switch them off in one place. Y again returns to the groups.
- **The header's segmented control** (Groups · Search · On) shows which view
  is open and can be tapped; the top screen shows the focused group or code
  with its description from the database, how many codes are on, and a key
  bar for the keys that apply on the current view.
- **Notes** (entries without code words) are listed but cannot be toggled. A
  change takes a moment: the engine applies it between frames and answers.
- Codes stay on across checkpoints and restarts. They are stored by name in
  `userdata/emulators/rgds-experimental/states/<GAMECODE>.cheats`, so a newer
  database keeps your choices as long as the names match.

Some codes change memory the game checks at start; if a code does nothing,
save, return home and start the game again with it on.

## How it works

The launcher never reads the database itself. When the menu opens, the bridge
inside the engine writes the loaded codes to `session/emulator.control.cheats`
(name, description, group, state) and rewrites it after every change. The menu
sends `cheat:<index>:<0|1>` through the runtime; the bridge calls the engine's
`rgds_set_cheat` between emulated frames, the engine saves the enabled names,
and the answer (`cheat-on`, `cheat-off` or `cheat-failed`) comes back the way
the other live settings do. `C2` and `C4` code types (native code injection)
are not supported by DSCore's engine and are skipped with a log line.
