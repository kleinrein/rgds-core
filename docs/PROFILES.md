# Profiles

Share the handheld without sharing saves. Everyone who plays gets their own in-game saves, resume points, save slots,
favourites, recents, playtime, look and DS name.

With one person nothing changes: no extra screen, and every file stays where it was. Add a profile in **Settings →
Profiles**. Once there are two, **Who's playing?** opens at startup (you can turn that off), and a chip on Home's bottom bar
changes who is playing. On Home's carousel layout, use **Settings → Profiles → Change who's playing**.

## Adult, Kid and Little kid

An **Adult** profile has its own saves and look, with the settings open. Managing profiles needs the owner's PIN.

A **Kid** profile shows only games that fit its age, and needs the owner's PIN for anything that changes the handheld:
Power, Library, Connectivity, System, Updates, Controls and managing profiles. Theme, Display, Sound and Statistics stay
open. Ports, Homebrew shelves, PictoChat, cheats and local wireless are off, each with a switch on the profile's page; with
PictoChat off its tile leaves Home. A Kid profile wears a **Kids** mark on **Who's playing?**.

A **Little kid** profile is a Kid profile for the smallest players, with no menus at all. Its whole launcher is one
screen: every game it may play, from every system, as big pictures, six to a page and always in the same order (by
name). The chosen game shows large on the upper screen with its name. A tap on a picture, or A, plays it; the D-pad,
L/R, the arrow buttons and a sideways swipe move through the pages, round and round. There is no Home, no settings, no
search and no game switcher: any other screen (a key, a restored state, a game ending) comes back to this one. A
grown-up leaves with **Select**, the **Menu** button or the small picture at the bottom (**Who's playing?**), then the
owner's PIN. A new Little kid profile starts at age 3 with unrated games hidden, so approve its games as for a Kid.

In a game, DS or any system RG DS Play runs, the Menu button opens just two big buttons over the paused game:
**Play** (back to the game; B does the same) and **Home** (leaves it as Return home does, saving as the profile's
auto-save says, back to the wall of games). There is no switcher, cheats, checkpoints or settings. **Select** is the
grown-up's way to the whole game menu: the owner's PIN first, or straight there when the owner has none.

A new profile opens **Games** straight onto every game in one list, sorted by name, instead of a Systems page first.
Change that on the profile's own page under **Settings → Profiles** (**Games opens**: All games or Systems), or, for
whoever is playing, in **Settings → Library → Games opens**. In that list
the Y sheet's **System** row narrows it to one system, and its **Systems page** chip still reaches Ports and Homebrew.

## What a Kid profile sees

Choose an age from 3 to 16, **No limit**, or **Only games I pick**. Ratings come from the same public catalogs as game
information, so run **Library → Fetch missing artwork** while online. "Everyone" counts as every age.

**Many games have no rating at all**, including plenty of children's games, every PSP game and nearly every PS1 game. Those
stay hidden on a Kid profile until you allow them: creating a Kid profile ends on its **No rating** list.

A profile's **Games** page lists every game with a box: ticked is shown on that profile. A ticks or unticks one game, and
the box on a system's heading ticks every game listed under it (after asking) or unticks them all. A heading never ticks a
game rated above the age: tick that one by itself if you want it. A game you tick or untick stays where it is until you
change a filter. Three buttons narrow the list: **Search** (X), one **System** (Y), and **Showing** (L and R: all, shown,
hidden, no rating). B clears a search. A choice you make always beats the age.

Any profile can hide a game from itself: **Game options → Hide this game**, or **Visible to…** once there are several
profiles. The filter sheet's **Hidden games** lists them, to bring them back.

Each profile also has its own **Language** on its page: menus switch to it whenever that profile is playing.

## The PIN

On the PIN pad, A or a tap types the lit key, **X** deletes one digit, and **B** deletes one more, or leaves once the
dots are empty. A wrong PIN shakes the dots before they clear.

Set the owner's PIN on the owner's page under **Settings → Profiles**. Without it a Kid profile is filtered but not locked,
and its page says so. Any profile can also have a PIN of its own. A child cannot enter a grown-up's profile just because
that one has no PIN: the owner's stands guard.

**Forgot it?** On a computer, create an empty file named `reset-pin` in the card's `rgds` folder. The next start removes the
owner's PIN.

**What a PIN does not do.** It stops a child using the handheld. It does not stop someone with the card and a computer,
because the card can be read by anything: saves are not encrypted, and a four-digit PIN can be worked out from its stored
form.

## Removing a profile

**Remove this profile** makes a backup in `userdata/exports`, then moves the profile's folder to `userdata/profiles/.removed`.
Nothing is deleted, and **Back up all saves** still includes it. **Erase removed profiles**, on the owner's page, deletes
those folders for good, and asks first.

## Shared between profiles

Two things cannot be separated: **Ports** keep their saves in their own folders, and a game that writes to a **loose C64 or ZX
disk image** writes to that one file. Controls, volume, Wi-Fi and updates belong to the handheld, not to a profile.

For where the files live, see [SAVES.md](SAVES.md). The design and what was measured are in
[PROFILES_PLAN.md](PROFILES_PLAN.md).
