# Settings hierarchy plan (2026-11-11)

The Settings tree grew one page at a time. Each page is fine on its own, but
the root list had twelve entries in one flat list, things that are not settings
in it (Statistics), the same page in two places (Updates), and rows sorted into
Theme or Display without a rule. This plan regroups it, borrowing from the
settings apps people already know (iOS, Android, Switch, PS5).

## Principles

1. **Group, don't flatten.** Short captions split long lists into groups of
   two to four rows (iOS grouped lists). Group captions are drawn between rows;
   they are not rows themselves, so D-pad focus never lands on them.
2. **Say what's inside.** Each root row carries a one-line summary of its
   current state (Android summaries), for example *Dark · Teal*, *Brightness
   60%*, *HomeNet · Bluetooth Off*. You can read the state without opening anything.
3. **One home per setting.** A setting lives on exactly one page. Shortcuts
   from Home (brightness, playtime) jump into the tree; they don't copy it.
4. **Settings before actions, destructive last.** Toggles and choices come
   first. Actions (Sleep now, Restart, Power off, Return to stock) go at the end,
   in their own group.
5. **Stable ids.** Section ids, page numbers and row control ids are stored in
   the state file and used by tests, so they never change. Only the order shown
   on screen changes (`section_order`, `power_rows`, `library_row`,
   `appearance_row_id`).

## Phase 1: regroup (done in this pass)

Root, top to bottom (section ids in brackets):

| Group | Rows |
|---|---|
| (none) | Profiles [11] |
| Look & sound | Theme [0] · Display [10] · Sound [3] |
| Games | Library [2] · Controls [6] |
| Device | Connectivity [4] · Power [1] · System [5] · Updates [9] |
| (none) | Power off [8] |

- **Statistics [7] leaves Settings.** Playtime is a place to look at, not a
  setting, and Home's bottom bar already opens it. Back from it now always
  returns Home.
- **Power off stays at the end** of the root, as its own action. Kid profiles
  can reach it there while every other device page is locked to them (iOS
  keeps Shut Down at the foot of General for the same reason).
- **System › Updates & recovery is gone**; Updates on the root is its only home.
- **Theme vs Display gets a rule.** Theme is how the menus look and move:
  Colour mode, Accent colour, High contrast, Typeface, Icon style, Background,
  Home layout, Animations. Display is the panel and touch: Screen brightness,
  Touch indicator, Touch gestures.
- **Groups inside pages:**
  - Library: *Games* (Scan for games, Systems, Games opens, Browser layout) ·
    *Per system* (System defaults, Default shader, Artwork sources) · *Saves*
    (Auto-load state, Auto-save on exit, Back up all saves).
  - Power: *Battery* (Power mode, Battery percentage) · *Sleep* (Sleep on lid
    close, Menu idle sleep, Dim when idle, Sleep timer in games) · actions
    (Sleep now, Restart, Power off).
  - System: *Language & region* (Language, Timezone, Daylight saving, Date
    format, Time format) · *Device* (Device information, then Return to stock
    or Restart menu, Touch to continue, Run setup again).
- **Fixed:** Back from Device information and from Updates (opened via System)
  landed one row off, because the hard-coded focus numbers in
  `Settings::back()` were never moved when Daylight saving was added.

Mechanism: `App::list_group(page, row)` names the caption above a row (or
an empty caption for a plain gap). `row_top()`, `row_at()` and
`list_content()` replace `index * list_stride` everywhere the list is laid
out, hit-tested, scrolled or culled, so any page can be grouped by returning
captions.

## Phase 2: one table for the tree (next)

Today the tree is implied by code spread over `Settings::enter()`,
`Settings::back()`, `move()`, the page-title chain in `settings_bottom`,
`list_count()`, and the restore checks in `state_persistence.hpp`. Every new
page touches all of them, which is how the off-by-one Back bug got in. Plan:
one `constexpr` table of `{page, parent, parent row, title, icon, row count}`
that drives Back (land on the row you came from), titles, breadcrumbs, the
restore clamp and deep links from Home and the game menu.

## Phase 3: search (next)

The X key already searches on Cheats, and there is an on-screen keyboard. A
settings index (every row's title, page and row, built from the phase 2 table)
lets X on any settings page find a row and jump straight to it, with the row
lit. This is the biggest win for "where was that setting?" (iOS, Android and
macOS all lead with search).

## Phase 4: open questions for the owner

- **Top screen as a detail pane.** On the iPad and the Switch, the second pane
  shows what is inside the focused entry. The DS has two screens. Today the upper
  screen deliberately stays quiet ("navigation belongs on the lower panel"),
  except System and Library, which already show a one-line tip for the focused
  row (`settings_tip`, the Library messages in `settings_bottom`). A middle
  ground is to give every page those tips. `settings_row_hint` in
  `settings_visual.hpp` already holds a line for most rows, but nothing calls it.
- **Quick settings.** Brightness, volume, Wi-Fi, Bluetooth and power mode one
  press from Home and the in-game menu (PS5 Control Center). With that in
  place, the tree can be for rare changes.
- **Default shader.** Library › Default shader and System defaults › Shader
  overlap. Folding the first into System defaults as an "All systems" entry
  would leave one place, but it changes saved files, so it waits.
- **Breadcrumbs** in the header from the third level down (*Library ›
  Artwork*), once phase 2 knows every page's parent.
