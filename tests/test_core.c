/*
 * Host-side tests for src/core. Build and run with `make test` on macOS --
 * no SDK, no container, no device.
 */
#include <stdio.h>
#include <string.h>

#include "core/abs_api.h"
#include "core/config.h"
#include "core/paths.h"
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
    if (v == NULL || strstr(v, ABS_CLIENT_VERSION) == NULL) {
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

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
