/*
 * Host-side tests for src/core. Build and run with `make test` on macOS --
 * no SDK, no container, no device.
 */
#include <stdio.h>
#include <string.h>

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

    /* Truncation must still NUL-terminate and not overrun. */
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

int main(void)
{
    test_sanitize();
    test_version();

    if (failures) {
        printf("\n%d failure(s)\n", failures);
        return 1;
    }
    printf("\nall tests passed\n");
    return 0;
}
