# Profiles

Share the handheld without sharing saves. Everyone who plays gets their own in-game saves, resume points, save slots,
favourites, recents, playtime, look and DS name.

With one person nothing changes: no extra screen, and every file stays where it was. Add a profile in **Settings →
Profiles**. Once there are two, **Who's playing?** opens at startup (you can turn that off), and a chip on Home's bottom bar
changes who is playing. On Home's carousel layout, use **Settings → Profiles → Change who's playing**.

## Adult and Kid

An **Adult** profile has its own saves and look, with the settings open. Managing profiles needs the owner's PIN.

A **Kid** profile shows only games that fit its age, and needs the owner's PIN for anything that changes the handheld:
Power, Library, Connectivity, System, Updates, Controls and managing profiles. Theme, Display, Sound and Statistics stay
open. Ports, Homebrew shelves, PictoChat, cheats and local wireless are off, each with a switch on the profile's page.

## What a Kid profile sees

Choose an age from 3 to 16, **No limit**, or **Only games I pick**. Ratings come from the same public catalogs as game
information, so run **Library → Fetch missing artwork** while online. "Everyone" counts as every age.

**Many games have no rating at all**, including plenty of children's games, every PSP game and nearly every PS1 game. Those
stay hidden on a Kid profile until you allow them: creating a Kid profile ends on its **No rating** list, where **Allow all**
takes a whole system at once. On a profile's **Games** page, A changes one game between automatic, always show and always
hide. A choice you make always beats the age.

Any profile can hide a game from itself: **Game options → Hide this game**, or **Visible to…** once there are several
profiles. The filter sheet's **Hidden games** lists them, to bring them back.

## The PIN

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
