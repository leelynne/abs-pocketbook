#ifndef ABS_PATHS_H
#define ABS_PATHS_H

#include <stddef.h>

/* Root of our on-device state (config, manifest, cover cache). */
#define ABS_APP_DIR    "/mnt/ext1/applications/ABSClient"
#define ABS_CONFIG_PATH ABS_APP_DIR "/abs_client.cfg"
#define ABS_STATE_PATH  ABS_APP_DIR "/abs_state.cfg"
#define ABS_LOG_PATH    ABS_APP_DIR "/abs_client.log"
/* Presence of this file turns on verbose logging (OPDSClient convention). */
#define ABS_LOG_TRIGGER ABS_APP_DIR "/LOGTRIGGER.TXT"

/*
 * Where downloaded audio lands.
 *
 * This is not our choice: the firmware's audiobook player scans exactly this
 * path (system/config/audiobooks/audiobooks.cfg holds `path=/mnt/ext1/Audio
 * Books`). Downloading anywhere else means books never appear in the stock
 * audiobook UI. Note the space in the name.
 */
#define ABS_AUDIO_DIR  "/mnt/ext1/Audio Books"

/*
 * Make `in` safe to use as a single FAT32 path component.
 *
 * Replaces the characters FAT32 and the PocketBook library indexer choke on,
 * collapses whitespace runs, trims leading/trailing dots and spaces, and
 * truncates to fit. Always writes a NUL-terminated string; falls back to
 * "untitled" if nothing usable survives.
 *
 * Returns the length written (excluding the NUL).
 */
size_t abs_sanitize_component(const char *in, char *out, size_t out_size);

#endif /* ABS_PATHS_H */
