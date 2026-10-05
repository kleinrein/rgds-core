# ScreenScraper artwork

Players enter their own ScreenScraper username and password in Settings → Library → Artwork & ScreenScraper. The app verifies the account before remembering it. A touch keyboard and controller navigation are available on the device; a desktop keyboard also works. Fetch missing artwork runs in the background, displays progress and can be stopped. Cached artwork remains available offline and after sign-out. Starting a game ends an unfinished background job so it does not compete with emulation.

The top screen displays box art and playtime. The bottom strip uses icons from the ROM's DS banner, so icons do not depend on logging in or scraping. Titles and icons for ZIP games are indexed in the background. Missing or invalid banners use a neutral DS icon. Playtime records emulator-session duration with 30-second checkpoints and a final update on exit; it includes emulator pause/menu time. Kernel suspend time is excluded by the monotonic clock. Existing hours from another frontend are not imported.

## Configure the project once

[ScreenScraper API v2](https://www.screenscraper.fr/webapi2.php) requires application credentials (`devid`, `devpassword`, `softname`) in addition to a signed-in player's credentials (`ssid`, `sspassword`). Register/request legitimate developer credentials for this project. They are configured by the project maintainer; ordinary players only use their personal account. This implementation does not borrow another frontend's application identity.

After deploying this build, run the interactive local setup tool. Passwords are prompted privately and sent through SSH stdin, rather than appearing in command arguments:

```sh
python3 tools/configure_scraper.py --device root@192.168.68.118
```

For a desktop setup:

```sh
python3 tools/configure_scraper.py --data build/dev
```

Desktop image conversion requires the SDL2_image library. It is already on the inventoried stock RG DS card. Install SDL2_image through your normal package manager if you want to scrape on the host. Rendering ROM icons, viewing cached BMP artwork, running ordinary builds and using the device do not require a new host dependency.

The client also accepts `SCREENSCRAPER_DEVID` and `SCREENSCRAPER_DEVPASSWORD` in its process environment for development. Do not put real credentials into source files, shell command arguments, screenshots or Git. Private configuration files are ignored by Git. The setup tool is a development mechanism; a future public build must provide its legitimate app identity as part of release provisioning.

## Storage and matching

On the stock card, credentials are stored with file mode 0600 inside `/userdata/rgds/private/`, whose mode is 0700. That ext4 partition supports permissions; the `/mnt/mmc` FAT partition does not. Host credentials live under the chosen data folder's `private/` directory. Passwords are never written to public status files or request logs. The client uses verified HTTPS and rejects redirects to other hosts.

Artwork, public metadata and playtime are under `rgds/userdata/library/<game-key>/`. The key is derived from the canonical ROM path. A release update keeps these files; moving/renaming a ROM currently gives it a new key. Metadata and artwork are written separately and atomically, so a failed request retains completed downloads.

ROM matching sends CRC32, MD5, SHA-1, size and filename to the Nintendo DS system endpoint. For a ZIP with one DS game, it hashes the contained game rather than the ZIP container. Europe, US, world and Japan are tried in that order when selecting regional names and front box art. Requests run serially and honour the user's advertised per-minute limit, with at least two seconds between requests. Quota, authentication, missing-game and network failures produce short UI messages. Downloads are size-bounded and artwork is normalized to BMP using the installed SDL_image decoder.

End-to-end live authentication and downloads still require project credentials. The automated tests cover request construction, matching, quota pacing, password redaction, private-file permissions, redirects, cache reuse, cancellation, ZIP banners and playtime without making real service requests.
