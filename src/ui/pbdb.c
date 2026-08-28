#include "ui/pbdb.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#include "core/sync.h"
#include "ui/log.h"

#define PB_AUDIOBOOK_DB "/mnt/ext1/system/config/audiobooks/audiobooks.db"

/*
 * Join positions to their books.
 *
 * Only rows with a position are of interest, and only ones that parse as a
 * real path -- the firmware stores archive members and other shapes here too.
 */
static const char *SQL =
    "SELECT bs.read_position, a.duration, a.title, bs.last_read_ts "
    "FROM book_state bs JOIN audiobooks a ON a.id = bs.book_id "
    "WHERE bs.read_position IS NOT NULL AND bs.read_position <> '' "
    "ORDER BY bs.last_read_ts DESC";

int abs_pbdb_read_states(abs_pb_state *out, int max)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int count = 0;

    if (out == NULL || max <= 0) return -1;

    /*
     * Read-only, and NOT immutable: this database is in WAL mode, and the
     * recent writes -- the ones we actually care about -- live in the -wal
     * sidecar. Opening it immutable would skip the WAL and report a stale,
     * usually empty, database.
     */
    int rc = sqlite3_open_v2(PB_AUDIOBOOK_DB, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        abs_log("pbdb: cannot open (%s)", db ? sqlite3_errmsg(db) : "?");
        if (db != NULL) sqlite3_close(db);
        return -1;
    }

    /* The firmware holds this open; do not wait forever behind it. */
    sqlite3_busy_timeout(db, 2000);

    rc = sqlite3_prepare_v2(db, SQL, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        abs_log("pbdb: prepare failed (%s)", sqlite3_errmsg(db));
        sqlite3_close(db);
        return -1;
    }

    while (count < max && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *pos = sqlite3_column_text(stmt, 0);
        if (pos == NULL) continue;

        abs_pb_state st;
        memset(&st, 0, sizeof st);

        if (!abs_parse_read_position((const char *)pos, st.path, sizeof st.path,
                                     &st.raw_loc)) {
            continue;   /* not a shape we understand */
        }

        st.duration = sqlite3_column_double(stmt, 1);

        const unsigned char *title = sqlite3_column_text(stmt, 2);
        if (title != NULL) {
            snprintf(st.title, sizeof st.title, "%s", (const char *)title);
        }
        st.last_read = sqlite3_column_int64(stmt, 3);

        out[count++] = st;
    }

    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        abs_log("pbdb: step ended with %d (%s)", rc, sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    abs_log("pbdb: %d position row(s)", count);
    return count;
}
