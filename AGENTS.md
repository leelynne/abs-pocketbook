# Working on this repo

A native Audiobookshelf client for PocketBook e-readers, in C against the InkView SDK.
Read `docs/PLAN.md` for architecture and the full record of what the device actually does.
This file is the short version: the things that will cost you a day if you don't know them.

## The one-paragraph summary

The app does everything *except* play audio. It signs in, browses the library, downloads
books into `/mnt/ext1/Audio Books/<Author> - <Title>/`, and hands them to the firmware's
own audiobook player. Playback, chapters and Bluetooth are the firmware's job. Listening
position is read back out of the firmware's SQLite database on the next launch and pushed
to the server.

## Build and deploy

```bash
./scripts/setup-sdk.sh     # one-time, ~1.3 GB
./scripts/build.sh         # cross-compile in a linux/amd64 container
./scripts/deploy.sh        # copy to a mounted PocketBook
make test                  # host-side tests, no SDK or device needed
```

The toolchain is x86_64 Linux and runs under emulation on Apple Silicon. `sdk/` is
gitignored and reproducible from the script.

**Every build stamps an id into the binary and shows it on screen** (`build 202942`, in
the book list footer). This exists because two deploys in a row silently failed to reach
the device and there was no way to tell which build was running. Check it after deploying.

## Platform rules — each of these was learned by breaking it

**Never do blocking work inside an InkView event handler.** InkView finishes its own show
sequence after your handler returns; blocking inside `EVT_SHOW` or a tap handler means the
panel never flushes what you drew. It presents as a ~30-second freeze that "clears when
you press a button" — the app is idle, the display is simply behind. Paint, return, and do
the work from a `SetWeakTimer` callback. See `defer_items` / `defer_detail` in `main.c`.

**Never `FullUpdate()` on a timer.** A full e-ink refresh takes about a second and
flashes. Repaint only what changed with `PartialUpdate` — `draw_download_dynamic` is the
model. Two consecutive `FullUpdate`s in one action is also a bug, not a detail.

**Never make a hardware key the only route to an action.** InkView can detect a
touchscreen (`QueryTouchpanel`) but *not* hardware keys, and users remap them. Every key
binding needs a touch equivalent.

**Hit-testing must match on both axes.** `add_row_at` takes x and width. Matching y alone
made every side-by-side button pair resolve to whichever was registered first — Delete
played the book, "Next" fired Prev. Use `draw_button_at` rather than hand-rolling
rectangles, so geometry and hit area cannot disagree.

**The player getters crash if no player exists.** `GetPlayerState`, `GetTrackPosition`,
`GetTrackSize`, `GetCurrentTrack` all kill the app if called before a player is running.

**`PlayFile()` never returns.** It hands the screen to the firmware player and terminates
this app, with or without `OpenPlayer()`. This is the platform working as designed, not a
crash. Save state before calling it. Use `OpenBook(path, NULL, 0)` — which routes through
the firmware's file-handler association and opens an `.m4b` in the audiobook app —
rather than `PlayFile`, which goes to the music player and lands the user on the home
screen.

**Home cannot be intercepted.** It is a global binding (`gkey.26.0=@KA_mmnu` in
`system/config/global.cfg`), consumed before applications see it. No key event arrives.
The mitigation is state restore, so the exit is lossless.

## The firmware's audiobook database

`/mnt/ext1/system/config/audiobooks/audiobooks.db`, read-only, via `src/ui/pbdb.c`.

```sql
audiobooks(id, type, title, artist, genre, duration, added_timestamp)
files(id, book_id, filename, folder_id, duration)
folders(id, storageid, name)
chapters(book_id, no, file_id, start_position, end_position, title)
book_state(id, book_id, read_position TEXT, read_percents, last_read_ts)
```

`read_position` looks like `/path/to/Book.m4b:#loc(87)`.

**It is in WAL mode.** Copy only `audiobooks.db` and every table reads as empty while the
real data sits in `audiobooks.db-wal`. Copy all three files, or open the live database.
Do **not** open it with `immutable=1` — that skips the WAL and silently returns stale
data. This nearly produced the conclusion that the firmware records nothing.

**Units are inconsistent inside this one database.** `audiobooks.duration` is seconds;
`chapters.start_position` is milliseconds. `#loc()` reads as seconds. `abs_sync_position_seconds`
guards this, and the guard itself had a bug worth understanding: the firmware measures the
audio file while the server sums tracks, so those durations differ by a second or so, and
a *finished* book lands slightly past the server's duration. An earlier version concluded
"must be milliseconds" and pushed 19 seconds over a completed book.

Treat this database as read-only. The firmware owns it.

## Don't touch firmware config

Registering the app in the launcher's `view.json` was tried and reverted: it made the app
vanish from the Applications list entirely, because `hash.txt` carries an integrity marker
that is not CRC32, Adler-32 or a byte sum, and we cannot regenerate it. The same caution
applies to `global.cfg` and anything else under `system/config/`. `scripts/restore-desktop.sh`
and `device-backup/` exist because of this.

## Code layout and conventions

`src/core/` must never include `inkview.h`. It holds parsing, URL construction, progress
math and path handling, and is covered by ~250 host assertions in `tests/test_core.c` that
run in a second on any machine. On-device iteration is USB-copy-and-relaunch, so anything
with interesting logic belongs here. Protect that boundary.

`src/ui/` is everything InkView-dependent. `main.c` is the event loop and all screens.

Request bodies go through cJSON, never `snprintf` — passwords and titles routinely contain
quotes and backslashes. The manifest is tab-separated because titles contain `=` and `:`.

The ABS API surface is small and documented in `docs/PLAN.md` §4. Notable: API keys are
plain JWTs accepted as `Authorization: Bearer` *or* `?token=`, `/login` sits above the
`/api` router, key creation is admin-only, and `isActive: true` must be sent explicitly or
the server mints a key that authenticates nothing.

## Releasing

`scripts/release.sh` is the only supported way to produce a distributable binary. It
refuses a dirty tree or failing tests, then gates the output on the ARM ABI, the expected
shared libraries, SQLite being statically linked, no `token=` in any URL format string,
and no embedded JWT. The last two exist because a binary carrying a credential-leaking bug
was once committed and installed after the source was already fixed.

The binary is **not** in the repo -- it belongs on a GitHub Release. A committed binary
drifts from its source silently, which is exactly how that happened.

Note for anyone editing that script: do not write `strings "$file" | grep -q ...` under
`set -o pipefail`. `grep -q` exits on first match, `strings` takes SIGPIPE, and the
pipeline reports failure precisely when the match succeeds.

## Verify your edits landed

Several bugs in this repo's history were **silent patch failures** — a scripted edit whose
anchor didn't match, leaving the code unchanged while the work looked done. `items_per_page`
stayed 0 (which makes the server return an entire library, since it reads `limit=0` as no
limit) and `cover_w` stayed 0 (which disabled list covers and the whole progressive-loading
path for two milestones). After any scripted edit, grep for the change. After any UI change,
deploy and check the build stamp.

## Testing on hardware

There is no emulator. `spike/` holds throwaway probe apps built with `./scripts/build.sh spike`
and copied to `applications/` by hand; that is how the playback behaviour above was
established. When device behaviour is in question, instrument and measure rather than
reason about it — the event log settled three separate arguments that plausible reasoning
had got wrong.
