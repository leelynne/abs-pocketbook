#ifndef ABS_VERSION_H
#define ABS_VERSION_H

#define ABS_CLIENT_VERSION "1.0.0"

/*
 * Human-readable one-liner for the footer, e.g. "ABS Client 0.2.0 (build 1421)".
 *
 * The build stamp comes from -DABS_BUILD_ID at compile time and exists so a
 * glance at the device answers "is this the build I just deployed?" -- over
 * USB that is otherwise guesswork.
 */
const char *abs_version_string(void);

#endif /* ABS_VERSION_H */
