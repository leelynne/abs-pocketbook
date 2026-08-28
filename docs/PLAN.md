# Audiobookshelf client for PocketBook — implementation plan

Target device: **PocketBook Verse Pro**. Target server: **Audiobookshelf v2.x**.

## 1. Scope: what this app is

The Verse Pro already plays audiobooks. This app does **not** decode or output audio.
It is the *library layer* the firmware is missing: it connects to Audiobookshelf,
browses your library, pulls files onto the device, hands them to the built-in player,
and keeps listening position in sync with the server.

| Concern | Owner |
|---|---|
| Auth to ABS, library browse, search, covers, metadata | **this app** |
| Downloading audio files to local storage | **this app** |
| Deciding which track + offset to resume at | **this app** |
| Decoding, audio output, Bluetooth routing, volume, lock-screen panel | firmware (`MPLAYERTASK`) |
| Reading back position and pushing it to ABS | **this app** |

Non-goals for v1: streaming, transcoding, podcasts, ebooks, multi-user, uploading.

## 2. Key findings from research (already verified in source)

**The firmware player is fully driveable from an app.** `inkview.h` exposes:

```c
void OpenPlayer(); void ClosePlayer();
void PlayFile(const char *filename);
void LoadPlaylist(char **pl); char **GetPlaylist();
void PlayTrack(int n); void NextTrack(); void PreviousTrack();
int  GetCurrentTrack(); int GetTrackSize();
void SetTrackPosition(int pos); int GetTrackPosition();
void SetPlayerState(int state); int GetPlayerState();  /* MP_PLAYING, MP_PAUSED, ... */
void SetVolume(int n); int GetVolume();
```

It runs as a **separate task** (`MPLAYERTASK -6`), and posts `EVT_MP_STATECHANGED` (81)
and `EVT_MP_TRACKCHANGED` (82) into our event handler. So our UI stays foreground and
alive while audio plays — we can poll `GetTrackPosition()` on a timer and sync.

**The player consumes file paths, not URLs.** Hence: download-then-play. (See spike S6
for whether the mplayer backend will accept an `http://` URL — if it does, streaming
becomes a v2 option, but do not design for it.)

**ABS API keys are ordinary JWTs.** `server/Auth.js:126` builds the JWT strategy with
`ExtractJwt.fromExtractors([fromAuthHeaderAsBearerToken(), fromUrlQueryParameter('token')])`.
So a key from *ABS → Settings → API Keys* works as `Authorization: Bearer <key>` **and**
as `?token=<key>`. No login, no refresh-token dance, no cookie jar in v1.

## 3. Architecture decision: native InkView app

Build a native C app against the PocketBook SDK, modelled on
`j2robin/Pocketbook-OPDSClient`, shipping as `ABSClient.app` in `/mnt/ext1/applications/`.

Rejected alternative: a **KOReader plugin** (the lane `AlexKucera/absaudio.koplugin`
occupies — same goal, Lua, calls the same inkview player through LuaJIT FFI). It is a
legitimate approach and worth reading as reference, but it requires the user to install
and live in KOReader. A native app appears in the stock Applications menu and works on a
stock device — which is the point of targeting the PocketBook ecosystem rather than
KOReader's.

Consequence: no LuaJIT FFI, no KOReader widget toolkit. We draw our own UI with InkView
primitives, exactly as the OPDS client does.

### Dependencies

| Library | Use | Source |
|---|---|---|
| `libinkview` | UI, events, wifi, player handoff | SDK sysroot |
| `libcurl` | all HTTP (needs custom headers — InkView's `Download()` can't set them) | SDK sysroot |
| `libfreetype` | text rendering | SDK sysroot |
| `stb_image.h` | decode JPEG/PNG covers | vendored header |
| **cJSON** | parse ABS responses | **vendor `cJSON.c`/`cJSON.h`** — ABS is JSON, so unlike the OPDS client we need no `libxml2`, and we should not assume a JSON lib is in the sysroot |

### Source layout — split core from UI

```
core/            # pure C, NO inkview include. Compiles and unit-tests on macOS.
  abs_api.c/h    # endpoint URL construction, request/response shapes
  abs_json.c/h   # parse libraries, items, tracks, chapters, progress
  progress.c/h   # global-time <-> (track index, track offset) math
  manifest.c/h   # local state: what's downloaded, last known position
  paths.c/h      # filename sanitization, storage layout
ui/              # inkview-dependent
  main.c         # event loop, screen router
  screens_*.c    # home / browse / detail / player / downloads / settings
  net.c          # libcurl wrapper: auth header, timeouts, resume, progress cb
  render.c       # list rows, cover cache, scaling from ScreenWidth/Height
tests/           # host-side tests for core/ (plain C, no device)
```

Rule: anything with interesting logic goes in `core/` and is testable without hardware.
`ui/` should be thin enough that its bugs are visible. This matters because on-device
iteration is USB-copy-and-relaunch, which is slow.

## 4. ABS API surface

Verified against `server/routers/ApiRouter.js` on `master`. All requests carry
`Authorization: Bearer <api_key>`.

| Purpose | Call |
|---|---|
| Validate credentials / list libraries | `GET /api/libraries` |
| Browse a library | `GET /api/libraries/:id/items?limit=&page=&sort=media.metadata.title` |
| Search | `GET /api/libraries/:id/search?q=` |
| Item detail: audio files, chapters, duration | `GET /api/items/:id?expanded=1` |
| Cover image | `GET /api/items/:id/cover?token=<key>` |
| Download one audio file | `GET /api/items/:id/file/:ino/download?token=<key>` |
| Read progress | `GET /api/me/progress/:id` (404 when none) |
| Write progress | `PATCH /api/me/progress/:id` — JSON body `{currentTime, duration, progress, isFinished}` |
| Continue-listening shelf | `GET /api/me/items-in-progress` |

**Use `PATCH /api/me/progress` — do not model playback sessions.** The session lifecycle
(`POST /api/items/:id/play` → `POST /api/session/:id/sync` → `.../close`) exists and is
what the mobile apps use, but it buys us only listening-time statistics and costs an
open-session state machine that must survive the app being killed. Revisit in v2 if you
want listening stats.

Files are addressed by **inode** (`audioFiles[].ino`), not track index — see
`AudioTrack.setData` building `/api/items/${itemId}/file/${audioFile.ino}`.

## 5. The playback handoff (the part with real design in it)

ABS stores `currentTime` as **seconds from the start of the whole book**. The firmware
player reports position **within the current track**. The bridge is
`audioTracks[i].startOffset`, which ABS gives us per track.

```
global_time  = startOffset[current_track] + GetTrackPosition()
resume:      find i where startOffset[i] <= t < startOffset[i] + duration[i]
             PlayTrack(i); SetTrackPosition(t - startOffset[i])
```

Single-file M4B is the degenerate case (one track, `startOffset = 0`) — but implement the
general form from day one, because retrofitting it touches the manifest, the sync loop,
the chapter list and the resume path all at once.

Flow:

1. `OpenPlayer()`, `LoadPlaylist()` with the book's files in track order.
2. `PlayTrack(i)`, then seek. **Seek after playback has actually started** — expect to
   have to wait for `EVT_MP_STATECHANGED` with `MP_PLAYING` before `SetTrackPosition()`
   takes (spike S2).
3. `SetWeakTimer` every ~5 s: read position, update the manifest on disk.
4. `PATCH` progress to ABS on: pause, track change, screen exit, and every ~60 s while
   playing. Never on the 5 s tick — that would hammer the server and the radio.
5. On `EVT_MP_TRACKCHANGED`, recompute the global offset from the new track index.

### The firmware keeps its own audiobook database

Found on the device at `system/config/audiobooks/audiobooks.db` — a SQLite file the
stock player owns:

```sql
audiobooks(id, type, title, artist, genre, duration, added_timestamp)
files(id, book_id, filename, folder_id, duration)
chapters(book_id, no, file_id, start_position, end_position, title)
book_state(id, book_id, read_position TEXT, read_percents, last_read_ts)
bookmarks(id, book_id, position, location, title, note)
```

This matters for two reasons.

**It is a second, durable source of position.** `book_state.read_position` persists
whatever the stock player reached, including listening the user did entirely outside our
app — which `GetTrackPosition()` polling can never see, because our app was not running.
Reading it on launch closes that hole and gives us a cross-check against our own manifest.

**Treat it as read-only.** The firmware owns this file and holds a lock
(`audiobooks.db.lock`, plus WAL). We read it to learn where the user got to; we do not
write positions into it. Anything we want to persist goes in our own manifest.

`read_position` is TEXT rather than an integer, so it encodes something structured
(file plus offset, most likely) — see spike S9.

Also from `audiobooks.cfg`: the player scans **`/mnt/ext1/Audio Books`**. Downloads go
there, not to a folder of our own naming, or books never appear in the stock UI.

**Sync conflict policy: furthest position wins**, with one guard — if the server says
`isFinished: true` and local position is *earlier*, prompt before overwriting. Cheap to
implement, and it is the policy the KOReader prior art landed on after the same analysis.

Write progress to the local manifest *before* attempting the network call, so a failed
`PATCH` (wifi asleep, server down) is retried on next launch rather than lost.

## 6. On-device layout

```
/mnt/ext1/applications/ABSClient.app          # the binary
/mnt/ext1/applications/ABSClient/
    abs_client.cfg                            # text config: server URL, API key, prefs
    manifest.cfg                              # downloaded books + last position + dirty flag
    covers/                                   # cover cache, LRU-capped (~20 MB)
    LOGTRIGGER.TXT                            # presence enables verbose logging
    abs_client.log
/mnt/ext1/Audiobooks/<Author>/<Title>/        # downloaded audio, sanitized names
```

Text config, not a binary blob — the OPDS client's README documents hitting system
write-protection issues and migrating away from binary saves. Learn that one for free.

## 7. Milestones

Each milestone has an exit criterion you can actually check.

**M0 — Toolchain and hello-app.** Build an app that draws device info and exits cleanly.
*Exit: `ABSClient.app` launches from the Applications menu on the Verse Pro.*

Resolved while setting this up:

- **Platform is B288.** The Verse Pro is dual-core 1 GHz; B288 is the dual-core SDK and
  B300 the quad-core one. `scripts/setup-sdk.sh` pulls `SDK-B288-6.8.7z` from the
  `pocketbook/SDK_6.3.0` repo's 6.8 release.
- **The toolchain is x86_64 Linux**, so on Apple Silicon everything cross-compiles inside
  a `linux/amd64` container (`Dockerfile` + `scripts/build.sh`). The SDK itself stays on
  the host under `sdk/` (gitignored) rather than baked into an image, so rebuilds are fast
  and the SDK is reusable.
- **Sysroot has what we need**: `libinkview` (under `/usr/local`, unlike everything else),
  `libcurl`, `libfreetype`, OpenSSL, zlib.
- **Verse Pro screen is 1072x1448** — confirmed on device. All UI scales off
  `ScreenWidth()`/`ScreenHeight()` anyway; nothing hardcodes those numbers.

Confirmed by running the M0 build on the actual device:

- **The device runs firmware 6.10** (`U634.6.10.3425`), which is newer than the 6.8 SDK
  we build against — and the binary loads and runs fine. PocketBook keeps the inkview ABI
  stable across 6.x, so there is no need to chase a matching SDK.
- **`GetHardwareType()` returns nothing useful** on this device (renders as unknown).
  Use `GetDeviceModel()` (`PB634`) and `GetSoftwareVersion()` for any
  device-conditional behaviour; do not branch on hardware type.
- **7-Zip corrupts this SDK's sysroot on macOS.** It refuses relative symlinks that point
  above the extraction root (`usr/lib/libm.so -> ../../lib/libm.so.6`) and leaves 0-byte
  files, which fail at link time with "file not recognized: File truncated".
  `scripts/setup-sdk.sh` repairs them (21 on this SDK).
- **The toolchain needs `LD_LIBRARY_PATH=/SDK/usr/lib`** so cc1 can find the host
  `libmpfr`/`libgmp` the SDK bundles.

**M1 — Network + auth.** libcurl wrapper with bearer header, 30 s timeouts, `NetConnect()`
before any request. Settings screen with `OpenKeyboard()` for server URL and API key,
persisted to `abs_client.cfg`. Validate by calling `GET /api/libraries`.
*Exit: entering a URL and key shows your real library names on the device.*

**M1 is done.** Sign-in, key minting, config persistence and library listing all work
against a real server. Two things changed from the plan as written:

- **The app never asks for an API key.** Typing a JWT on an e-ink keyboard is unusable and
  there is no clipboard from a computer. Instead the setup form takes an admin username,
  password, and optionally the user the key is for; it calls `POST /login`, then
  `POST /api/api-keys`, stores the returned key, and wipes the password. Key creation is
  admin-only (`ApiKeyController.middleware` requires `isAdminOrUp`), and `isActive` must be
  sent explicitly `true` or the server mints a key that authenticates nothing.
- **`scripts/configure-device.sh`** remains as the no-typing path: it writes
  `abs_client.cfg` straight to the mounted device.

**Do not register the app in the launcher's `view.json`.** Tried and reverted. The
firmware documents the mechanism in its own `system/config/desktop/view.json` comment
block -- a `U_`-prefixed entry with `path`, `title` and `icon`, plus the key listed in a
`view.groups[].apps` array -- and `hash.txt`'s `hash2` is a plain md5 of that file, which
we can regenerate. What we cannot regenerate is the `#xxxxxxxx` integrity marker
`hash.txt` itself carries (not crc32, adler32 or a byte sum). With a stale marker the
launcher did not merely ignore the new entry: **the app disappeared from the Applications
list entirely**, which is worse than the predicted fallback-to-defaults. Restored from
`device-backup/`. Revisit only if that checksum is identified; the app is listed by
filename without any of this, which costs only a nicer title and icon.

**Never make a hardware key the only route to an action.** inkview exposes
`QueryTouchpanel()` but has no equivalent for hardware keys, so an app cannot detect
whether the device it is running on has page or back buttons -- and users can remap the
ones that exist. Every key binding needs a touch equivalent: a back chevron in the header,
on-screen pager buttons, tap-zones for scrolling. Keys stay as an accelerator.

**Never block inside an inkview event handler.** This cost a debugging round in M1 and will
cost more later, so it is a rule, not a note. inkview finishes its own show sequence after
your handler returns; blocking inside `EVT_SHOW` means the panel never flushes what you
drew, and the screen stays stale until some later event pumps the loop. It looks exactly
like a 30-second hang that "clears when you press a button" — the app is idle, the display
is simply behind. Paint, return, and do the work from a `SetWeakTimer` callback.

The corollary for **M3**: a 300 MB download cannot live in a handler at all. It needs
chunked transfer that yields to the event loop between chunks, so the UI stays painted,
the progress bar moves, and cancel works. Budget for that rather than discovering it.

**M2 — Browse.** Library list → paginated item list → item detail. Covers fetched,
decoded via `stb_image`, scaled, cached, LRU-evicted. Search. Scale row count and font
size from `ScreenWidth()/ScreenHeight()` — do not hardcode Verse Pro dimensions.
*Exit: you can find any book in your library on-device, with cover and description.*

**M3 — Download.** Chunked download to a `.part` file with HTTP `Range` resume,
`OpenProgressbar`/`UpdateProgressbar` UI, cancellable, atomic rename on completion.
**Hold off sleep for the duration** (`iv_sleepmode(0)`) or a 300 MB download dies when the
device naps — then restore it. Show file size before starting; offer delete-local.
*Exit: a multi-hundred-MB book lands complete on the device and survives a mid-transfer
cancel + resume.*

**M4 — Playback handoff.** Resolve spikes S1–S3 first. Player screen: play/pause, seek
±30 s, track and chapter navigation, position read on a timer.
*Exit: a downloaded book plays, and resuming from the app lands within a second of where
you stopped.*

**M5 — Sync.** Pull progress on item open, push on the events in §5, retry queue for
failed pushes, conflict policy with the `isFinished` prompt. Home screen backed by
`/api/me/items-in-progress`.
*Exit: stop mid-chapter on the Verse Pro, open the ABS web player, and it resumes at the
same second — and the reverse.*

**M6 — Polish.** Dark mode (firmware 6.8+ inverts the UI; covers must be excluded from
inversion, as the OPDS client does), hardware page-button mappings, downloads manager,
storage summary, error dialogs that never leave a dead screen.

## 8. Spikes — resolve on hardware before committing to M4

These are the unknowns that can invalidate the design. Each is one short test app.
(S8 is already resolved; S7 and S9 were opened or narrowed by inspecting a real device.)

- **S1 — What unit is `GetTrackPosition()`?** Seconds, milliseconds, or samples. Play a
  file of known duration, print position and `GetTrackSize()`. One run answers it
  permanently. *Everything in §5 is written in whatever unit this returns.*
- **S2 — Does `SetTrackPosition()` stick before playback starts?** If seeking immediately
  after `PlayTrack()` is ignored, resume must be deferred until `MP_PLAYING`.
- **S3 — Can we read position cross-task while our UI is foreground?** The whole sync
  design assumes yes (separate `MPLAYERTASK` + `EVT_MP_*` events say it should). If not,
  fall back to writing position on `EVT_BACKGROUND`/`EVT_MP_STATECHANGED` only.
- **S4 — Does `LoadPlaylist()` work from a third-party app,** and does `GetCurrentTrack()`
  track correctly across auto-advance? Fallback: single-file `PlayFile()` per track with
  manual advance on `MP_TRACK_FINISHED`.
- **S5 — Does the firmware player handle M4B** (AAC in MP4) and does it expose embedded
  chapters? If chapters are not exposed, drive them from ABS's chapter list instead —
  which is preferable anyway since ABS chapter times are already global.
- **S6 — (optional, high value) Does `PlayFile("http://…?token=…")` work?** The backend is
  mplayer-derived, which can do HTTP. If it works, streaming is a v2 feature. If not,
  nothing is lost. Do not let this hold up M3.
- **S7 — Verse Pro audio routing.** *Partly resolved:* the Verse Pro has no speaker —
  output is Bluetooth or USB-C. Still to confirm on device: whether pairing must be done
  in system settings before our handoff works, and whether `PlayFile()` fails loudly or
  silently with no sink connected. If we need an affordance, `IsBluetoothEnabled()` and
  `OpenBTdevicesMenu()` exist.
- ~~**S8 — Is a JSON library in the SDK sysroot?**~~ **Resolved: vendor cJSON.** The
  sysroot ships `libjson` (JSONNode), which is C++. We are writing C, so vendor cJSON
  rather than drag in a C++ runtime for one parser.

## 9. Risks

**Sleep and wifi during long downloads** is the most likely source of "it just doesn't
work" reports. Budget real time for M3 robustness — hold sleep off, handle `NetConnect`
failure mid-transfer, always resume rather than restart.

**Storage.** Audiobooks are 100 MB–1 GB. Show sizes before download, surface free space,
make deletion one tap. On a device sold for text, this is a first-class feature, not
polish.

**Device iteration is slow.** Mitigated by the `core/`/`ui/` split and host-side tests —
protect it, it is the main thing keeping this project pleasant.

**ABS API drift.** Only nine endpoints, all wrapped in `core/abs_api.c`, all failures
non-fatal with a dialog. Pin behaviour to the server version you run.

**Firmware player is a black box.** S1–S5 exist precisely because its contract is
undocumented. If several of them fail, the honest fallback is the KOReader-plugin lane,
where prior art has already walked this ground.

## 10. References

- `j2robin/Pocketbook-OPDSClient` — the structural model: Makefile, InkView UI patterns,
  cover cache, dark mode handling, `LOGTRIGGER.TXT` logging, config-file lessons
- `advplyr/audiobookshelf` — `server/routers/ApiRouter.js`, `server/Auth.js`,
  `server/objects/files/AudioTrack.js`, `docs/openapi.json`
- `advplyr/audiobookshelf-app` — reference for progress/session semantics
- `AlexKucera/absaudio.koplugin` — same goal in the KOReader lane; its `docs/spec/` and
  ADRs independently reached the inkview-player and furthest-wins conclusions above
- PocketBook SDK images: `blchinezu/pocketbook-sdk`, `Sean-on-Git/PocketBook-SDK`
