/*
 * Host-side tests for src/core. Build and run with `make test` on macOS --
 * no SDK, no container, no device.
 */
#include <stdio.h>
#include <string.h>

#include "core/abs_api.h"
#include "core/config.h"
#include "core/paths.h"
#include "core/state.h"
#include "core/version.h"

static int failures = 0;

static void check_str(const char *what, const char *got, const char *want)
{
    if (strcmp(got, want) != 0) {
        printf("  FAIL %s\n       got  \"%s\"\n       want \"%s\"\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %s -> \"%s\"\n", what, got);
    }
}

static void check_int(const char *what, long got, long want)
{
    if (got != want) {
        printf("  FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    } else {
        printf("  ok   %s -> %ld\n", what, got);
    }
}

static void test_sanitize(void)
{
    char buf[64];

    printf("abs_sanitize_component:\n");

    abs_sanitize_component("The Hobbit", buf, sizeof buf);
    check_str("plain title", buf, "The Hobbit");

    abs_sanitize_component("Hitchhiker's Guide: Part 2/3", buf, sizeof buf);
    check_str("colon and slash", buf, "Hitchhiker's Guide Part 2 3");

    abs_sanitize_component("  spaced   out  ", buf, sizeof buf);
    check_str("whitespace runs", buf, "spaced out");

    abs_sanitize_component("trailing dots...", buf, sizeof buf);
    check_str("trailing dots", buf, "trailing dots");

    abs_sanitize_component("???", buf, sizeof buf);
    check_str("nothing usable", buf, "untitled");

    abs_sanitize_component(NULL, buf, sizeof buf);
    check_str("null input", buf, "untitled");

    char small[8];
    abs_sanitize_component("abcdefghijklmnop", small, sizeof small);
    check_str("truncated", small, "abcdefg");
}

static void test_version(void)
{
    printf("abs_version_string:\n");
    const char *v = abs_version_string();
    if (v == NULL || strstr(v, ABS_CLIENT_VERSION) == NULL ||
        strstr(v, "build") == NULL) {
        printf("  FAIL version string missing %s\n", ABS_CLIENT_VERSION);
        failures++;
    } else {
        printf("  ok   %s\n", v);
    }
}

static void test_config_parse(void)
{
    abs_config cfg;

    printf("abs_config_parse:\n");

    abs_config_parse("server=https://abs.example.com\ntoken=abc123\n", &cfg);
    check_str("server", cfg.server, "https://abs.example.com");
    check_str("token", cfg.token, "abc123");

    /* Trailing slash must go, or every URL we build gets a double slash. */
    abs_config_parse("server=https://abs.example.com/\n", &cfg);
    check_str("trailing slash stripped", cfg.server, "https://abs.example.com");

    /* Whitespace around the '=' is easy to introduce by hand-editing. */
    abs_config_parse("  server  =  https://x.test  \n  token = tok \n", &cfg);
    check_str("spaces around =", cfg.server, "https://x.test");
    check_str("spaces around = (token)", cfg.token, "tok");

    /* Comments, blanks, CRLF, and keys we don't know must not break parsing. */
    abs_config_parse("# comment\r\n\r\nserver=https://y.test\r\nfuture_key=1\r\n", &cfg);
    check_str("crlf + comment + unknown key", cfg.server, "https://y.test");

    abs_config_parse("", &cfg);
    check_str("empty input server", cfg.server, "");
    check_str("empty input token", cfg.token, "");

    abs_config_parse(NULL, &cfg);
    check_str("null input", cfg.server, "");

    /* A line with no '=' should be skipped, not crash or half-parse. */
    abs_config_parse("garbage line\nserver=https://z.test\n", &cfg);
    check_str("line without =", cfg.server, "https://z.test");

    abs_config_parse("server=https://z.test\n", &cfg);
    check_int("insecure defaults off", cfg.insecure, 0);
    abs_config_parse("server=https://z.test\ninsecure=1\n", &cfg);
    check_int("insecure=1 honoured", cfg.insecure, 1);
    abs_config_parse("server=https://z.test\ninsecure=0\n", &cfg);
    check_int("insecure=0 honoured", cfg.insecure, 0);
}

static void test_config_roundtrip(void)
{
    abs_config in, out;
    char buf[2048];

    printf("abs_config_serialize round-trip:\n");

    memset(&in, 0, sizeof in);
    snprintf(in.server, sizeof in.server, "%s", "https://abs.example.com");
    snprintf(in.token, sizeof in.token, "%s", "eyJhbGciOiJIUzI1NiJ9.payload.sig");

    size_t n = abs_config_serialize(&in, buf, sizeof buf);
    if (n == 0) {
        printf("  FAIL serialize returned 0\n");
        failures++;
        return;
    }

    in.insecure = 1;
    n = abs_config_serialize(&in, buf, sizeof buf);

    abs_config_parse(buf, &out);
    check_str("server survives", out.server, in.server);
    check_str("token survives", out.token, in.token);
    check_int("insecure survives", out.insecure, 1);

    /* Too small a buffer must fail cleanly rather than truncate silently. */
    char tiny[8];
    check_int("serialize into tiny buffer", (long)abs_config_serialize(&in, tiny, sizeof tiny), 0);
}

static void test_config_validate(void)
{
    abs_config cfg;

    printf("abs_config_validate:\n");

    memset(&cfg, 0, sizeof cfg);
    check_int("empty -> NO_SERVER", abs_config_validate(&cfg), ABS_CONFIG_NO_SERVER);

    snprintf(cfg.server, sizeof cfg.server, "%s", "abs.example.com");
    check_int("no scheme -> BAD_SCHEME", abs_config_validate(&cfg), ABS_CONFIG_BAD_SCHEME);

    snprintf(cfg.server, sizeof cfg.server, "%s", "https://abs.example.com");
    check_int("no token -> NO_TOKEN", abs_config_validate(&cfg), ABS_CONFIG_NO_TOKEN);

    snprintf(cfg.token, sizeof cfg.token, "%s", "tok");
    check_int("complete -> OK", abs_config_validate(&cfg), ABS_CONFIG_OK);

    snprintf(cfg.server, sizeof cfg.server, "%s", "http://192.168.1.10:13378");
    check_int("plain http allowed", abs_config_validate(&cfg), ABS_CONFIG_OK);
}

static void test_urls(void)
{
    abs_config cfg;
    char buf[2048];

    printf("URL builders:\n");

    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.server, sizeof cfg.server, "%s", "https://abs.example.com");
    snprintf(cfg.token, sizeof cfg.token, "%s", "tok123");

    abs_url_libraries(&cfg, buf, sizeof buf);
    check_str("libraries", buf, "https://abs.example.com/api/libraries");

    abs_url_item_cover(&cfg, "li_abc", buf, sizeof buf);
    check_str("cover", buf, "https://abs.example.com/api/items/li_abc/cover?token=tok123");

    abs_auth_header(&cfg, buf, sizeof buf);
    check_str("auth header", buf, "Authorization: Bearer tok123");

    /* Overflow must return 0 and not emit a half-formed URL. */
    char tiny[10];
    check_int("url into tiny buffer", (long)abs_url_libraries(&cfg, tiny, sizeof tiny), 0);
    check_str("tiny buffer cleared", tiny, "");
}

static void test_parse_libraries(void)
{
    abs_library libs[8];

    printf("abs_parse_libraries:\n");

    /* Shape mirrors a real GET /api/libraries response, trimmed. */
    const char *body =
        "{\"libraries\":["
        "{\"id\":\"lib_1\",\"name\":\"Audiobooks\",\"mediaType\":\"book\"},"
        "{\"id\":\"lib_2\",\"name\":\"Podcasts\",\"mediaType\":\"podcast\"},"
        "{\"id\":\"lib_3\",\"name\":\"Fiction\",\"mediaType\":\"book\"}"
        "]}";

    int n = abs_parse_libraries(body, libs, 8);
    check_int("book libraries only", n, 2);
    check_str("first name", libs[0].name, "Audiobooks");
    check_str("first id", libs[0].id, "lib_1");
    check_str("podcast skipped, next is Fiction", libs[1].name, "Fiction");

    /* Entries with no id are unusable downstream. */
    n = abs_parse_libraries("{\"libraries\":[{\"name\":\"No ID\",\"mediaType\":\"book\"}]}", libs, 8);
    check_int("entry without id skipped", n, 0);

    /* max must be respected. */
    n = abs_parse_libraries(body, libs, 1);
    check_int("respects max", n, 1);

    /* Malformed or unexpected bodies must report failure, not garbage. */
    check_int("not json", abs_parse_libraries("<html>500</html>", libs, 8), -1);
    check_int("wrong shape", abs_parse_libraries("{\"error\":\"nope\"}", libs, 8), -1);
    check_int("null body", abs_parse_libraries(NULL, libs, 8), -1);
    check_int("empty array", abs_parse_libraries("{\"libraries\":[]}", libs, 8), 0);
}

static void test_signin_bodies(void)
{
    char buf[1024];

    printf("sign-in request bodies:\n");

    abs_build_login_body("alice", "hunter2", buf, sizeof buf);
    check_str("login body", buf, "{\"username\":\"alice\",\"password\":\"hunter2\"}");

    /* Passwords with quotes and backslashes must survive encoding -- this is
     * why the bodies go through a JSON encoder rather than snprintf. */
    abs_build_login_body("bob", "a\"b\\c", buf, sizeof buf);
    check_str("password with quote and backslash", buf,
              "{\"username\":\"bob\",\"password\":\"a\\\"b\\\\c\"}");

    /* isActive must be present and true, or the key authenticates nothing. */
    abs_build_api_key_body("PocketBook PB634", "usr_1", buf, sizeof buf);
    check_str("api key body", buf,
              "{\"name\":\"PocketBook PB634\",\"userId\":\"usr_1\",\"isActive\":true}");

    char tiny[8];
    check_int("body into tiny buffer", (long)abs_build_login_body("a", "b", tiny, sizeof tiny), 0);
    check_int("null username", (long)abs_build_login_body(NULL, "b", buf, sizeof buf), 0);
}

static void test_parse_login(void)
{
    char token[ABS_MAX_TOKEN], id[ABS_MAX_ID];

    printf("abs_parse_login:\n");

    const char *body =
        "{\"user\":{\"id\":\"usr_9\",\"username\":\"alice\",\"type\":\"admin\","
        "\"accessToken\":\"eyJhbG.tok.sig\",\"refreshToken\":null}}";

    check_int("parses", abs_parse_login(body, token, sizeof token, id, sizeof id), 1);
    check_str("access token", token, "eyJhbG.tok.sig");
    check_str("user id", id, "usr_9");

    /* A login that returns no token must fail rather than store an empty key. */
    check_int("no token", abs_parse_login("{\"user\":{\"id\":\"u\"}}",
                                          token, sizeof token, id, sizeof id), 0);
    check_int("no user object", abs_parse_login("{\"error\":\"nope\"}",
                                                token, sizeof token, id, sizeof id), 0);
    check_int("not json", abs_parse_login("<html>", token, sizeof token, id, sizeof id), 0);
}

static void test_parse_users(void)
{
    abs_user us[8];

    printf("abs_parse_users:\n");

    const char *body =
        "{\"users\":["
        "{\"id\":\"usr_1\",\"username\":\"root\",\"type\":\"root\"},"
        "{\"id\":\"usr_2\",\"username\":\"lee\",\"type\":\"admin\"},"
        "{\"username\":\"broken\",\"type\":\"user\"}"
        "]}";

    check_int("count (entry without id skipped)", abs_parse_users(body, us, 8), 2);
    check_str("first username", us[0].username, "root");
    check_str("first type", us[0].type, "root");
    check_str("second id", us[1].id, "usr_2");

    check_int("wrong shape", abs_parse_users("{\"nope\":[]}", us, 8), -1);
    check_int("empty", abs_parse_users("{\"users\":[]}", us, 8), 0);
}

static void test_parse_api_key(void)
{
    char key[ABS_MAX_TOKEN];

    printf("abs_parse_api_key:\n");

    /* The secret is the inner "apiKey" string inside the "apiKey" record. */
    const char *body =
        "{\"apiKey\":{\"id\":\"k_1\",\"name\":\"PocketBook\","
        "\"apiKey\":\"eyJ.secret.value\",\"isActive\":true}}";

    check_int("parses", abs_parse_api_key(body, key, sizeof key), 1);
    check_str("key", key, "eyJ.secret.value");

    check_int("record without key", abs_parse_api_key("{\"apiKey\":{\"id\":\"k\"}}",
                                                      key, sizeof key), 0);
    check_int("not json", abs_parse_api_key("<html>", key, sizeof key), 0);
}

static void test_signin_urls(void)
{
    abs_config cfg;
    char buf[1024];

    printf("sign-in URLs:\n");

    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.server, sizeof cfg.server, "%s", "https://abs.example.com");

    /* /login sits above the /api router -- getting this wrong 404s. */
    abs_url_login(&cfg, buf, sizeof buf);
    check_str("login", buf, "https://abs.example.com/login");

    abs_url_users(&cfg, buf, sizeof buf);
    check_str("users", buf, "https://abs.example.com/api/users");

    abs_url_api_keys(&cfg, buf, sizeof buf);
    check_str("api keys", buf, "https://abs.example.com/api/api-keys");

    abs_auth_header_token("tok", buf, sizeof buf);
    check_str("explicit bearer", buf, "Authorization: Bearer tok");
}

static void test_format(void)
{
    char buf[32];

    printf("formatting:\n");

    abs_format_duration(12240, buf, sizeof buf);   check_str("3h 24m", buf, "3h 24m");
    abs_format_duration(2820, buf, sizeof buf);    check_str("47m", buf, "47m");
    abs_format_duration(38, buf, sizeof buf);      check_str("38s", buf, "38s");
    abs_format_duration(0, buf, sizeof buf);       check_str("zero", buf, "--");
    abs_format_duration(-5, buf, sizeof buf);      check_str("negative", buf, "--");
    /* Rounding must not produce "3h 60m". */
    abs_format_duration(3599.7, buf, sizeof buf);  check_str("rounds to 1h", buf, "1h 0m");

    abs_format_size(1503238553LL, buf, sizeof buf); check_str("GB", buf, "1.4 GB");
    abs_format_size(327155712LL, buf, sizeof buf);  check_str("MB", buf, "312 MB");
    abs_format_size(0, buf, sizeof buf);            check_str("no size", buf, "--");
}

static void test_strip_html(void)
{
    char buf[256];

    printf("abs_strip_html:\n");

    abs_strip_html("<p>Hello <b>world</b>.</p>", buf, sizeof buf);
    check_str("tags removed", buf, "Hello world .");

    abs_strip_html("Tom &amp; Jerry &lt;3", buf, sizeof buf);
    check_str("entities decoded", buf, "Tom & Jerry <3");

    abs_strip_html("line one<br/>line two", buf, sizeof buf);
    check_str("br becomes space", buf, "line one line two");

    abs_strip_html("  lots   of\n\n whitespace  ", buf, sizeof buf);
    check_str("whitespace collapsed", buf, "lots of whitespace");

    abs_strip_html("plain text", buf, sizeof buf);
    check_str("plain passthrough", buf, "plain text");

    /* An unterminated tag must not run off the end of the buffer. */
    abs_strip_html("before <span oops", buf, sizeof buf);
    check_str("unterminated tag", buf, "before");

    abs_strip_html(NULL, buf, sizeof buf);
    check_str("null input", buf, "");

    char small[8];
    abs_strip_html("aaaaaaaaaaaaaaaaaaaa", small, sizeof small);
    check_int("truncated length", (long)strlen(small), 7);
}

static void test_parse_items(void)
{
    abs_item items[8];
    int total = -1;

    printf("abs_parse_items:\n");

    const char *body =
        "{\"results\":["
        "{\"id\":\"li_1\",\"media\":{\"duration\":12240.5,\"numTracks\":1,"
        "\"metadata\":{\"title\":\"Dune\",\"authorName\":\"Frank Herbert\"}}},"
        "{\"id\":\"li_2\",\"media\":{\"duration\":3600,\"numTracks\":12,"
        "\"metadata\":{\"title\":\"Neuromancer\",\"authorName\":\"William Gibson\"}}}"
        "],\"total\":57,\"limit\":2,\"page\":0}";

    check_int("count", abs_parse_items(body, items, 8, &total), 2);
    check_int("total drives paging", total, 57);
    check_str("title", items[0].title, "Dune");
    check_str("author", items[0].author, "Frank Herbert");
    check_int("tracks", items[1].num_tracks, 12);

    /* An item with no media should still list rather than vanish. */
    check_int("bare item", abs_parse_items("{\"results\":[{\"id\":\"li_3\"}],\"total\":1}",
                                           items, 8, &total), 1);
    check_str("untitled fallback", items[0].title, "(untitled)");

    check_int("no id skipped",
              abs_parse_items("{\"results\":[{\"media\":{}}],\"total\":1}", items, 8, NULL), 0);
    check_int("wrong shape", abs_parse_items("{\"nope\":1}", items, 8, NULL), -1);
    check_int("not json", abs_parse_items("<html>", items, 8, NULL), -1);
}

static void test_parse_item_detail(void)
{
    abs_item_detail d;

    printf("abs_parse_item_detail:\n");

    const char *body =
        "{\"id\":\"li_1\",\"media\":{\"duration\":12240,\"size\":327155712,"
        "\"numTracks\":1,\"numChapters\":24,"
        "\"metadata\":{\"title\":\"Dune\",\"subtitle\":\"Book One\","
        "\"authorName\":\"Frank Herbert\",\"narratorName\":\"Simon Vance\","
        "\"seriesName\":\"Dune #1\",\"publishedYear\":\"1965\","
        "\"description\":\"<p>A <i>desert</i> planet &amp; a boy.</p>\"}}}";

    check_int("parses", abs_parse_item_detail(body, &d), 1);
    check_str("title", d.title, "Dune");
    check_str("narrator", d.narrator, "Simon Vance");
    check_str("series", d.series, "Dune #1");
    check_str("year as string", d.published_year, "1965");
    check_int("chapters", d.num_chapters, 24);
    check_str("description flattened", d.description, "A desert planet & a boy.");

    /* publishedYear is a number on some items and a string on others. */
    check_int("numeric year parses",
              abs_parse_item_detail("{\"id\":\"x\",\"media\":{\"metadata\":"
                                    "{\"title\":\"T\",\"publishedYear\":1984}}}", &d), 1);
    check_str("numeric year", d.published_year, "1984");

    check_int("no id fails", abs_parse_item_detail("{\"media\":{}}", &d), 0);
    check_int("not json", abs_parse_item_detail("<html>", &d), 0);
}

static void test_browse_urls(void)
{
    abs_config cfg;
    char buf[1024];

    printf("browse URLs:\n");

    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.server, sizeof cfg.server, "%s", "https://abs.example.com");

    abs_url_library_items(&cfg, "lib_1", 0, 25, buf, sizeof buf);
    check_str("items page 0", buf,
              "https://abs.example.com/api/libraries/lib_1/items"
              "?limit=25&page=0&sort=media.metadata.title");

    abs_url_library_items(&cfg, "lib_1", 3, 25, buf, sizeof buf);
    check_str("items page 3", buf,
              "https://abs.example.com/api/libraries/lib_1/items"
              "?limit=25&page=3&sort=media.metadata.title");

    abs_url_item(&cfg, "li_9", buf, sizeof buf);
    check_str("expanded item", buf, "https://abs.example.com/api/items/li_9?expanded=1");
}

static void test_state(void)
{
    abs_state in, out;
    char buf[1024];

    printf("abs_state round-trip:\n");

    memset(&in, 0, sizeof in);
    in.screen = 3;
    in.page = 4;
    snprintf(in.library_id, sizeof in.library_id, "%s", "lib_1");
    snprintf(in.library_name, sizeof in.library_name, "%s", "My Books");
    snprintf(in.item_id, sizeof in.item_id, "%s", "li_42");

    size_t n = abs_state_serialize(&in, buf, sizeof buf);
    check_int("serialized", n > 0, 1);

    abs_state_parse(buf, &out);
    check_int("screen", out.screen, 3);
    check_int("page", out.page, 4);
    check_str("library id", out.library_id, "lib_1");
    check_str("library name (spaces kept)", out.library_name, "My Books");
    check_str("item id", out.item_id, "li_42");

    /* Missing or damaged state must degrade to "start at the top". */
    abs_state_parse("", &out);
    check_int("empty screen", out.screen, 0);
    check_str("empty library", out.library_id, "");

    abs_state_parse(NULL, &out);
    check_int("null input", out.page, 0);

    abs_state_parse("page=-5\nscreen=2\n", &out);
    check_int("negative page clamped", out.page, 0);

    abs_state_parse("garbage\n\n#comment\nlibrary_id=lib_9\n", &out);
    check_str("survives junk", out.library_id, "lib_9");

    char tiny[8];
    check_int("tiny buffer", (long)abs_state_serialize(&in, tiny, sizeof tiny), 0);
}

int main(void)
{
    test_sanitize();
    test_version();
    test_config_parse();
    test_config_roundtrip();
    test_config_validate();
    test_urls();
    test_parse_libraries();
    test_signin_urls();
    test_signin_bodies();
    test_parse_login();
    test_parse_users();
    test_parse_api_key();
    test_browse_urls();
    test_format();
    test_strip_html();
    test_parse_items();
    test_parse_item_detail();
    test_state();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
