# Plan: fork Tanuki3DS and make 3DS games faster

Nothing here has been started. The only change on the handheld so far is the frame skip limit of 3 in the engine's `run.sh`.

**Yes, fork it, but a fork won't add any speed.** We are effectively running a fork already, kept as a patch file. The speed work below is what fixes the lag.

- Size of our changes: `tools/tanuki3ds/tanuki-rgds.patch` is about 6,100 lines across 32 files.
- Upstream: our base commit `879a1e7` is still its latest, and recent commits there are mostly build and CI changes, so there is little to merge.
- Licence: Tanuki3DS is GPL-3, so the source has to be offered if we ship the engine anyway. A public fork is the simplest way to do that.

## Part 1: Speed

Best lead: the owner's 30-40 fps may match a stretch in the last scripted NSMB2 run. There it shows 19-20 pictures per half second (38-40 fps) instead of 31, because about 12 textures per half second are rebuilt at about 23 ms each, which takes about half of all the time. It is not confirmed that this was the scene being played.

The handheld does not look like the limit. During a hand-played NSMB2 session the CPU and GPU were at full clock, the temperature was 64 °C, and the two busiest threads were at 53% and 71%.

| # | Step | Done when |
|---|------|-----------|
| 1 | Scripted routes into real levels for all three games; the demo and map tests missed the worst problems | The same stretch can be compared between builds |
| 2 | Keep the numbers from hand play; the log is only saved when a game ends and the next launch overwrites it | The exact moment that felt bad can be read back |
| 3 | Find which NSMB2 textures keep rebuilding and why, then stop it or move the work off the render thread | That stretch shows 30 of 30 |
| 4 | Find where the renderer's time goes in Mario Kart 7, which still loses about one picture in six; one option is dropping the extra copy before each screen update | Races lose close to none |
| 5 | Measure how even the pictures are and the delay from button to screen; a steady 30 can still feel bad | We know whether "laggy" means uneven pictures or late buttons |
| 6 | Try extra emulated CPU time in a real 3D Land level, so far only tested in the demo and map | We know whether 30 is really the game's limit |

Targets: NSMB2 and Mario Kart 7 at a steady 60, and 3D Land at a steady 30 unless step 6 shows it can go higher.

If that is not enough, decide per game whether it is worth keeping. Azahar is not a likely rescue: the estimate for it was 30-35 fps at best, and that would need a risky rewrite.

## Part 2: Fork

| # | Step | Why |
|---|------|-----|
| 1 | The owner creates the fork on GitHub, with an `rgds` branch from commit `879a1e7` | Real history |
| 2 | Prepare the patch as a handful of commits: renderer, CPU, save states, menu, sound, build | Makes it possible to track down which change broke something |
| 3 | Sort the `SPIKE_*` test switches into real settings and debug-only ones | Some test hooks are still acting as live settings |
| 4 | Point `tools/tanuki3ds` at a fork commit instead of the patch file | One source of truth |
| 5 | Automated builds and a way to install the engine | New cards do not get the engine today |

## Things to know first

- Any new engine build makes existing 3DS save states and auto-saves unusable; in-game saves are safe. Most speed fixes need a new build, so save in-game before each one. Making states survive updates would be a separate piece of work.
- Recommended order: speed first, because that is what hurts and the fork can wait. The downside is that the speed fixes pile onto the patch file until then.
