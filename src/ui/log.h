#ifndef ABS_LOG_H
#define ABS_LOG_H

/*
 * Logging is off unless ABS_LOG_TRIGGER exists, so a normal install writes
 * nothing. Convention borrowed from PocketBook-OPDSClient: on a device with no
 * console, a trigger file is the only practical way to turn on diagnostics
 * after the fact.
 */
void abs_log_init(void);

/* Monotonic milliseconds, for timing calls that might be blocking the UI. */
unsigned long abs_now_ms(void);

/*
 * Return `url` with any `token=` query value replaced by "REDACTED".
 *
 * The log lives on removable storage and is the first thing a user attaches to
 * a bug report -- an API key must never reach it. Tokens are no longer put in
 * URLs at all; this is the backstop for when someone adds one again.
 *
 * Returns a pointer to a static buffer, valid until the next call.
 */
const char *abs_log_redact_url(const char *url);
void abs_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  abs_log_enabled(void);

#endif /* ABS_LOG_H */
