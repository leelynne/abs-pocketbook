# Audiobookshelf client for PocketBook

A native [Audiobookshelf](https://www.audiobookshelf.org/) client for PocketBook
e-readers, targeting the **PocketBook Verse Pro** (PB634).

The Verse Pro already plays audiobooks. This app is the library layer the firmware is
missing: it talks to your Audiobookshelf server, browses your library, downloads books to
the device, hands them to the built-in player, and syncs your listening position back to
the server. It does not decode or output audio itself.

**Status:** M0 — toolchain and scaffolding. See [docs/PLAN.md](docs/PLAN.md).

## Building

Requires Docker. The PocketBook toolchain is x86_64 Linux, so on Apple Silicon it runs
under emulation inside a container — nothing is installed on the host.

```bash
./scripts/setup-sdk.sh    # one-time: fetch + unpack the SDK into sdk/ (~1.3 GB)
./scripts/build.sh        # cross-compile -> build/ABSClient.app
./scripts/deploy.sh       # copy to a PocketBook mounted over USB
```

Logic in `src/core/` is free of `inkview.h` and builds natively, so it can be tested
without the SDK, a container, or the device:

```bash
make test
```

## Layout

```
src/core/    pure C -- API shapes, parsing, progress math, paths. Host-testable.
src/ui/      inkview-dependent -- event loop, screens, drawing, networking.
tests/       host-side tests for src/core.
scripts/     setup-sdk.sh, build.sh, deploy.sh
docs/PLAN.md architecture, ABS API surface, milestones, hardware spikes
```

## On-device install

`ABSClient.app` goes in the `applications/` folder of the device's internal storage and
then appears in the Applications menu. `scripts/deploy.sh` does this for you.

Verbose logging is off unless a trigger file exists (a convention borrowed from
PocketBook-OPDSClient):

```
/mnt/ext1/applications/ABSClient/LOGTRIGGER.TXT   ->  writes abs_client.log
```

## References

* Pocketbook OPDS client - https://github.com/j2robin/Pocketbook-OPDSClient
* Audiobookshelf github - https://github.com/advplyr/audiobookshelf
* Audiobookshelf mobile apps - https://github.com/advplyr/audiobookshelf-app
* PocketBook SDK - https://github.com/pocketbook/SDK_6.3.0
