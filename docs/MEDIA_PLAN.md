# Media plan: videos, books and music in one place

Status: plan, 2026-11-21. Nothing built. Owner asked to bring the music player "up to bananaplayer level with video,
pdf, manga whatever, just 100x better: more beautiful and feature complete".

The bar is not "plays an MP4". Other handhelds already have that: an mpv/ffplay port with a file list. What none of
them have is two screens, a hinge, touch on both panels, and a launcher that already looks good. The plan is built
around those.

## 1. What it is

One **Media** tile on Home (it replaces the Music tile when switched on; Settings → Sound → Music player becomes
Settings → Apps → Media). Inside, three shelves with the same library look as Music has now:

| Shelf | What | Formats |
| --- | --- | --- |
| **Music** | the player as it is today | as docs/MUSIC.md |
| **Videos** | films, series, clips, recordings | MKV, MP4, WebM, AVI, MOV, TS; H.264, HEVC, VP8/9, AV1 (software), MPEG-2/4; SRT/ASS/PGS subtitles |
| **Books** | manga, comics, PDFs, ebooks | CBZ, CBR, CB7, folders of pictures, PDF (scans and real text), EPUB, TXT |

Folders: `/data/videos`, `/data/books`, next to `/data/music`, each a file-sharing share and found over USB drive mode.

## 2. The ideas that make it the DS's own

**Videos**
- Upper screen is the picture, lower screen is the remote: a scrub strip of thumbnails you drag with a finger,
  chapters, audio and subtitle tracks, speed, a sleep timer. Nothing ever draws over the picture.
- Close the lid halfway and stand it like a tent: the picture moves to the panel facing you (the hinge angle is not
  sensed, so this is a button: "Stand mode").
- Subtitles drawn on the lower screen instead of over the picture, as an option ("Subtitles below"), in the launcher's
  own font, large.
- Resume everywhere, per file; series in a folder play on, with "Next episode in 5" and skip intro by chapter name.
- Battery and time left in the remote; brightness and volume by dragging on the picture's edges (finger on the upper
  panel).

**Books**
- **Book mode**: hold the handheld sideways like a book. Each panel is a 480x640 portrait page; a manga spread is one
  page per screen, right to left. Flip with the D-pad, a swipe, or the shoulder buttons (which are then where your
  thumbs are).
- Straight mode for a single tall page: the page on the upper screen, a page strip and the outline on the lower.
- Panel-by-panel zoom for comics (find the panels from the gutters, as in phase 4), pinch zoom (the two-finger decoder
  from MANUALS_PLAN exists), a remembered page per book, a reading direction per series.
- Real PDF text and EPUB reflow in the launcher's font sizes, with night and sepia papers.

**Music** keeps everything it has; the shared parts (library rows, tabs, the bottom bar, resume, the shoulder tabs)
are pulled out so Videos and Books are the same app, not three.

## 3. What the code forces

- **Video needs its own process and the panels**, like a game: the launcher draws through SDL software fills and a
  menu frame costs ~37 ms (project-menu-speed). A video frame has to go decode → texture → scanout without the menu.
  So `rgds-media` is a separate program the launcher starts the way it starts Play, built from Play's two-screen
  frame (SDL KMS + GLES, the shader pass, the game menu) and from `play/src/reader` for Books. Music stays its own
  engine (`rgds-music`) so a song carries on underneath.
- **Hardware decode.** The kernel (7.2.7) has `VIDEO_HANTRO_ROCKCHIP` (H.264 to 1080p, VP8, MPEG-2) and
  `VIDEO_ROCKCHIP_VDEC`. Whether rkvdec handles the RK3566's VDPU346 (HEVC, VP9, H.264 above 1080p) in 7.2 is not
  known: the series was in review in early 2026. FFmpeg needs the V4L2 request hwaccel (upstream or Kwiboo's patches,
  as ROCKNIX and LibreELEC carry); the image has FFmpeg 6.1.5. Decoded frames come out as dma-bufs, and the PSP
  shared-texture path already imports dma-bufs into GL on Panfrost.
- **Software decode** on four A55s is the fallback: the panels are 640x480, so FFmpeg can drop to a lower-resolution
  decode path (skip loop filter, skip frames) for 720p H.264 and HEVC. 1080p HEVC in software will not hold 30 fps.
- **Audio**: video audio goes through the same SDL/ALSA path as music, the same EQ per output and the Bluetooth delay
  (a fifth of a second) is used to delay the picture instead.
- **PDF text and EPUB** need a renderer; the reader only pulls scan JPEGs out. MuPDF does PDF, EPUB, CBZ and XPS in
  one library (AGPL-3.0; it can be combined with this GPL-3.0 tree, the binary is then AGPL). Poppler is GPL and does
  PDF only.
- **CBR** needs unrar's free decoder (libarchive reads RAR4 and most RAR5); CB7 needs libarchive too.

## 4. Phases

| Phase | What | Done when |
| --- | --- | --- |
| 0 | Measure on the handheld: `/sys/class/video4linux` names, a v4l2-request FFmpeg in the VM, 720p/1080p H.264 and HEVC decode time in software and on Hantro, dma-buf import into GL, battery draw per hour | numbers in §8 |
| 1 | `rgds-media` shell from Play's frame: library, Videos shelf, software decode, the remote on the lower screen, resume, subtitles (SRT/ASS via libass) | a 720p MKV with subtitles plays at full speed on the device |
| 2 | Hardware decode through V4L2 request + dma-buf, zero copy to the upper panel; thumbnails strip | 1080p H.264 under 30 % CPU |
| 3 | Books on `play/src/reader`: CBZ/CBR/CB7/folders, book mode, spreads, pinch, per-book place | a manga volume read sideways on the device |
| 4 | MuPDF: PDF text, EPUB reflow, panel-by-panel comics, night/sepia | an EPUB novel and a text PDF read on the device |
| 5 | One Media tile: Music moved in as the third shelf, shared library parts, Settings → Apps → Media | the owner switches the old Music tile off for good |
| 6 | Later, if wanted: network shares (SMB/DLNA/Jellyfin) as sources, recently watched on Home | — |

Each phase is its own change and goes to the handheld only with the owner's go, as a RAM bind like the earlier
players, never touching saves or userdata.

## 5. Decisions (owner, 2026-11-21)

1. One Media tile with three shelves.
2. MuPDF, for PDF, EPUB and the rest.
3. FFmpeg 8.x in the image, with the V4L2 request hwaccel.
4. "All of it": network sources stay in, as phase 6.
5. Performance first: incredibly snappy, consistent with the launcher, beautiful touch, touch effects, animations
   and the status bar.

## 5a. How it is built (from 2026-11-21)

- `play/src/media/`: the `rgds-media` program, built by `tools/build_media.py`. It shares Play's `ui/` (the
  launcher's look, font, kit and translations), so its screens are drawn by the same code as the companion screen.
- Everything moving is a GPU texture drawn by SDL's GLES renderer: the video picture (YUV textures, converted on
  the GPU), book pages (rendered once by MuPDF on a worker thread, then only moved), the remote's layers. The CPU
  canvas is redrawn only when what it shows changes, never for a slide or a fade.
- Decoding runs on its own threads (demux, video, audio, page renders); the draw thread never waits on a file.

## 6. Testing

- Host: the preview binary on the Mac with sample files made by `ffmpeg -f lavfi` (no copyrighted media in the tree),
  tests in `tests/test_media_*.py` the way Music has them, the reader's existing 43 checks.
- Device: decode timings and battery per phase; the owner watches and reads, I report only what I measured.

## 7. Open questions to measure first

- Does 7.2.7 bind rkvdec on the RK3566 (`rockchip,rk3568-vdec`), and which codecs does it list?
- Hantro H.264 at 1080p: frame time, and does Panfrost import its NV12 dma-buf without a copy?
- Software 720p HEVC with skip-loop-filter: does it hold 30 fps on three cores while audio plays on the fourth?
- Battery: an hour of 720p H.264 in software against Hantro, panels at the owner's usual brightness.

## 8. Measurements

None yet. The handheld was not on the network on 2026-11-21.

## 9. Built, 2026-11-21 (Mac only, not on the handheld)

`play/src/media/`, built by `python3 tools/build_media.py host` (brew ffmpeg 8, libass, mupdf, sdl2), tests in
`tests/test_media.py` (makes its own film, PDF, EPUB and CBZ; 7 tests pass) driving `tests/media_tests.cpp`.

- `video.cpp`: FFmpeg reader, picture and sound threads; the sound is the clock; exact seeks; YUV/NV12 textures;
  speeds 0.5-2x through atempo (pitch kept); a V4L2 request decoder is used when FFmpeg has one
  (`RGDS_MEDIA_SOFTWARE=1` turns it off); libass subtitles from the file, Matroska fonts, or an .srt/.ass beside the
  film; picture subtitles (PGS, DVD); a subtitle reader of its own from 30 s before a seek, so a line already on
  screen is not lost; thumbnails and posters on their own decoder.
- Speaker EQ: the music player's equaliser (`src/music/eq.hpp`) on the film's sound, the curve for the output in
  use (speakers, Bluetooth, headphones) from the music folder's `eq.ini`, the speakers' +6 dB make-up gain and
  limiter. Sound → Equaliser picks a preset; it goes to the running music engine as its `eq` command, or into the
  file, so the two never overwrite each other (`sound_out.hpp`).
- `book.cpp`: MuPDF on one worker thread (PDF, EPUB, MOBI, FB2, XPS, CBZ/CBT, pictures), folders of pictures
  through `reader/pages.cpp`; pages cached by size and paper and the next spreads rendered ahead; EPUB relaid out
  for the text size, keeping the place; outline (EPUB links resolved); White, Sepia and Night paper.
- Screens: `library_screen.cpp` (Videos and Books shelves: hero with a GPU blur on the upper screen, poster grid
  with the launcher's flick and rubber band, focus corners gliding, sliding tab pill, folders, Continue first),
  `watch_screen.cpp` (remote: thumbnail strip scrubbing with live picture, chapters, Skip intro, Sound / Subtitles /
  Speed / Picture / Sleep sheets, Subtitles below, Swap screens, lights down after 6 s, Up next with countdown,
  place saved every 10 s), `read_screen.cpp` (book view held sideways with spreads, wide pages across both panels,
  spine shading, drag-to-turn with springs, manga direction, left or right hand; page view with slider and
  chapters). Ripples on touch, screen transitions as a fade-and-scale of a snapshot.
- Wired, 2026-11-22: Home's **Media** tile takes Music's place when rgds-media is installed
  (`src/launcher/media_launch.hpp`; `media_at`, `app_slot` in main.cpp). It starts `runtime/media.py launch`
  through the same display handoff as a game; the profile's own places (`RGDS_MEDIA_STATE`) and look
  (`RGDS_MEDIA_PREFERENCES`) are passed on. With the music player on, rgds-media has a third tab, Music, which
  leaves with exit code 42, and the launcher opens its music player. On the handheld rgds-media reads the lower
  touch panel itself (`RGDS_TOUCH_DEV`, no bridge) and shows the volume level the runner's volume keys set.
  Folders: /data/videos and /data/books (RG DS OS).
- Buildroot package `os/buildroot/package/rgds-media` (in rgds_os_defconfig, cleaned by own_os_build.py) builds
  against the image's FFmpeg 6.1.5, libass 0.17.4 and MuPDF 1.23.9; without a V4L2 request decoder it decodes in
  software. Built in the VM on 2026-11-22 (984 KB aarch64; the image's SDL is KMS only, so the Wayland window
  roles are compiled out), and tests/media_tests.cpp built for the device passes against the image's libraries.
- 63 strings in all 23 locale files.
- Not done: FFmpeg 8 with V4L2 request in the image, dma-buf zero copy, pinch zoom, network sources, shares for
  Videos and Books in file sharing, anything on the handheld.

## 10. One app in the launcher, 2026-11-22 (Mac only, not on the handheld)

Owner: the Media app and the Music app looked like two different programs, Music left for another one, nothing
was remembered; keep the Media app's pills and blur, but make it one fast app. So the shelves moved into the
launcher, next to the music player, and rgds-media now only plays a film or shows a book.

- `src/launcher/media_shelf.hpp`: the music player's level gains section pills along the top of the lower screen,
  **Music · Videos · Books** (a lit glass pill slides between them). Music's own tabs become a second, smaller pill
  row. L2/R2 step the sections from a first page. On Videos and Books, L/R step them as well (those shelves have no
  tabs). A tap on a pill does the same. The section is kept in the profile's `media/launcher.ini` and Media opens
  on it next time. Sections travel sideways with the launcher's own view transitions; folders go forward and Back.
- Videos and Books are the Media app's grid (3 films or 4 books across, length chip, progress bar, folder chips,
  gliding focus corners, flick and rubber band). The upper screen is the picked item large over a blur of its own
  picture, cross-fading as the focus moves. Music's first page shows the focused row the same way, blurring its
  44 px cover. X: Play / Play from the start / Mark as finished. Start resumes the newest one in progress.
- `rgds-media --index` (`make_index` in `play/src/media/library.cpp`) writes `STATE/index.tsv` with every shelf at
  once. Then, in the background (nice 10), it makes each poster (the `.px` cache as before, plus `.bmp` and a
  24-px `.blur.bmp` for the launcher's picture loader) and rewrites the index every 350 ms. Items in progress get
  their posters first. The launcher starts it each time Media opens and re-reads index.tsv and places.tsv whenever
  they change.
- A or a tap on a film or book: `launch_media()` with `--open FILE`, through the usual display handoff. Back in
  rgds-media leaves it, and the launcher comes back on the same shelf and item with the new place shown.
  The Home tile opens the level directly (`open_music`) and no longer starts rgds-media. Its Music tab, exit
  code 42, is unused by the launcher.
- Tests: tests/test_media_ui.py (tile and remembered section, folder in and out, four sideways transitions).
- Not measured on the handheld: grid frame time with many posters, indexer CPU while browsing, and the open
  time of a film. That still goes through runtime/media.py (Python start-up) before rgds-media. Exec'ing
  rgds-media straight from the launcher is the next speed step if it feels slow.

Motion (2026-11-23): the house motion, nothing of its own. The lit pill glides 160 ms ease-out like a segmented
control; a section or a Music tab crossfades the page in place under the pills (no sideways slide); the focus
corners sit on the focused tile as every grid's focus does (only the scroll glides); the hero crossfades in 140 ms
with no lift, and not at all while a section change's own crossfade runs.

## 11. Reader orientations, both touch screens, covers (2026-11-26, Mac only)

- Three orientations in `read_screen.cpp`, kept per book (`mode=book|stack|page` in places.tsv): **Sideways** (the
  book view as before), **Upright** (new: held normally, the book runs down both screens; a reflowing book shows two
  pages, upper first, a fixed page such as a PDF or comic is one tall page across both), and **With controls** (the
  old page view). Select steps through them; a change crossfades both panels (`Context::crossfade`).
- Both touch screens turn pages: rgds-media opens the upper panel's Goodix (`RGDS_TOP_TOUCH_DEV`, else the one on
  fe5c0000, as the launcher finds it). Touches carry `TouchEvent::upper` and reach only screens whose `upper_touch()`
  is true; one finger at a time across both panels. Drag follows the finger on either screen; a tap near an outer
  edge turns, a tap in the middle (around the spine sideways) opens the menu. On a desktop the mouse on the upper
  half is the upper panel; test scripts have `utouch`.
- The menu replaces the old list: title and place, a slider through the book (chapters as ticks, reversed for right
  to left), the three orientations as small pictures of the handheld, Left/Right hand (sideways), paper swatches,
  text size A / A (only for reflowing books), direction, Contents and Close. It is drawn upright for the reader
  (turned when sideways), every change shows at once behind it, the D-pad moves a focus ring through it (turned with
  the reader), B/X/Start or a tap outside or on the other screen puts it away.
- Covers: Project Gutenberg EPUBs showed white posters and a blank first page, because MuPDF pushed the tall cover
  picture to page 2 when the body's top and bottom margins left it short. A user stylesheet drops the body's vertical
  margins (`fz_set_user_css` on the image's MuPDF 1.23, `fz_style_document` on 1.26+); the shelf's cover is the
  EPUB's own cover picture (EPUB 3 `cover-image`, EPUB 2 `<meta name="cover">`, the guide's cover page), else the
  first of the first three pages with something on it. Book posters are named `-c2` so old white ones are remade.
- Not done: the handheld (upper touch orientation on the RG DS and the Plus is assumed to match the panel, as the
  launcher assumes), a book-mode fade between hands, pinch zoom.
