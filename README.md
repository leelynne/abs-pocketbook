# Audiobookshelf client for PocketBook

A native [Audiobookshelf](https://www.audiobookshelf.org/) client for PocketBook
e-readers, built with the InkView SDK.

The Verse Pro already plays audiobooks perfectly well. What it can't do is talk to your
server. This app is that missing half: it browses your Audiobookshelf library, downloads
books to the device, hands them to the built-in player, and syncs your listening position
back to the server.

It deliberately does **not** implement playback. The firmware's own audiobook player
handles chapters, bookmarks, Bluetooth and the lock screen far better than a third-party
app could — so books are downloaded where the firmware indexes them, and opened with it.

## Features

- **Sign in once, no key typing.** Enter an admin username and password and the app mints
  its own Audiobookshelf API key over the API, stores that, and forgets the password.
  Pasting a JWT on an e-ink keyboard is not a thing anyone should have to do.
- **Browse your library** — paged book lists with cover art, and a detail view with
  author, narrator, series, length, file count, chapter count and description.
- **Search** any library.
- **Download books** with a progress bar and cancel, resuming interrupted transfers rather
  than starting a 300 MB file over. Sleep is held off while transferring, free space is
  checked first, and a cover is written alongside the audio.
- **Play in the stock audiobook app**, at the right book, with chapters.
- **Progress syncs back to Audiobookshelf** on launch — including listening you did
  entirely outside this app, straight from the native player.
- **See the server's position** for a book, so you can tell where you got to on another
  device.
- **Picks up where you left off.** The firmware closes apps when you press Home; the app
  saves your place and returns to it.

## Tested devices

| Device | Model | Firmware | Notes |
|---|---|---|---|
| PocketBook Verse Pro | PB634 | 6.10 (`U634.6.10.3425`) | Audio over Bluetooth or USB-C; no speaker |

Nothing else has been tried. The UI scales off `ScreenWidth()`/`ScreenHeight()` rather
than assuming the Verse Pro's 1072x1448, and the binary is built against the B288 SDK, so
other 6.x PocketBooks with audio have a fair chance of working — but that is an
expectation, not a claim. Reports welcome.

## Installing

1. Download **[`release/ABSClient.app`](release/ABSClient.app)**.
2. Connect the reader over USB and choose the storage/connect option on its screen.
3. Copy `ABSClient.app` into the `applications` folder on the device.
4. Eject the device. The app appears under **Applications**.

Then open it and sign in: enter your server address (`https://abs.example.com`), an admin
username and password, and optionally the account the key should belong to. Creating API
keys requires an admin account on the server; the credentials are used once and never
stored.

If you would rather not type credentials on the device, `scripts/configure-device.sh`
writes the config file directly over USB:

```bash
./scripts/configure-device.sh https://abs.example.com     # prompts for an API key
```

### Where things go on the device

```
applications/ABSClient.app                    the app
applications/ABSClient/abs_client.cfg         server address and API key
applications/ABSClient/downloads.tsv          which books are downloaded, and where
applications/ABSClient/covers/                cover cache (20 MB, oldest evicted)
Audio Books/<Author> - <Title>/               downloaded audio, where the firmware finds it
```

Verbose logging is off unless a trigger file exists:

```
applications/ABSClient/LOGTRIGGER.TXT   ->   writes abs_client.log
```

## Requirements

- An Audiobookshelf v2.x server.
- An **admin** account, if you want the app to create its own API key. Otherwise supply a
  key through `scripts/configure-device.sh`.
- A PocketBook with audio — see [Tested devices](#tested-devices).

## Building

Requires Docker. The PocketBook toolchain is x86_64 Linux, so on Apple Silicon it runs
under emulation inside a container — nothing is installed on the host.

```bash
./scripts/setup-sdk.sh    # one-time: fetch and unpack the SDK into sdk/ (~1.3 GB)
./scripts/build.sh        # cross-compile -> build/ABSClient.app
./scripts/deploy.sh       # copy to a PocketBook mounted over USB
./scripts/build.sh release # stripped binary into release/
```

Logic in `src/core/` has no InkView dependency and builds natively, so it can be tested
without the SDK, a container, or the device:

```bash
make test
```

## Layout

```
src/core/     pure C -- API shapes, parsing, progress math, paths. Host-testable.
src/ui/       InkView-dependent -- event loop, screens, drawing, networking, downloads.
src/vendor/   cJSON, stb_image
spike/        throwaway hardware probes
tests/        host-side tests for src/core
docs/PLAN.md  architecture, the ABS API surface, and what the device actually does
AGENTS.md     orientation for anyone (or anything) working on this next
```

## Security notes

- **The API key is stored in cleartext** in `abs_client.cfg`. PocketBook has no keystore
  and FAT32 has no permissions, so anyone with the device or its USB storage can read it.
  This is why the app uses a scoped, separately revocable API key rather than your
  password — revoke it in Audiobookshelf if the device is lost.
- **Tokens are never put in URLs**, only in an `Authorization` header, so they do not reach
  this app's log, the server's access log, or any proxy between. The logger redacts
  `token=` regardless.
- **`insecure=1` disables certificate checking** for every request, including the sign-in
  that carries your password. The app shows a warning banner whenever it is set. Only use
  it for a self-signed certificate on a server you control.
- Everything the server sends is treated as untrusted: filenames, ids, titles and image
  data are sanitized before they reach a filesystem path or an on-disk record.

## Known limitations

- **Home exits the app.** It is bound globally in firmware (`gkey.26.0=@KA_mmnu`) and
  never reaches applications, so it cannot be intercepted. The app saves your place and
  restores it; use the on-screen `<` to navigate back within the app.
- **Progress syncs one way**, device to server. Reading the position from another device
  is shown on the book screen, but is not applied automatically — that would mean writing
  to the firmware's own database.
- **No cover art in the stock audiobook list.** The firmware does not appear to render it
  for audiobooks; a `cover.jpg` is written alongside the audio regardless.
- Podcast libraries are filtered out.

## License

MIT — see [LICENSE](LICENSE).

Vendored third-party code keeps its own terms: [cJSON](https://github.com/DaveGamble/cJSON)
(MIT) and [stb_image](https://github.com/nothings/stb) (public domain / MIT), both under
`src/vendor/`.

## References

* Audiobookshelf — https://github.com/advplyr/audiobookshelf
* PocketBook SDK — https://github.com/pocketbook/SDK_6.3.0
* PocketBook OPDS client, the structural model for this app —
  https://github.com/j2robin/Pocketbook-OPDSClient
