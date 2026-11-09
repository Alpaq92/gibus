#ifndef GIBUS_TEST_H
#define GIBUS_TEST_H

#include <stdio.h>
#include <string.h>

static int failures;

static void
test_ok(const char *what, int cond)
{
    if (!cond) failures++;
    printf("  %-56s %s\n", what, cond ? "ok" : "FAILED");
}

static void
test_eq(const char *what, long got, long want)
{
    if (got == want) {
        printf("  %-56s ok\n", what);
        return;
    }
    failures++;
    printf("  %-56s got %ld, wanted %ld   FAILED\n", what, got, want);
}

static void
test_str(const char *what, const char *got, const char *want)
{
    if (got && want && strcmp(got, want) == 0) {
        printf("  %-56s \"%s\"\n", what, got);
        return;
    }
    failures++;
    printf("  %-56s got \"%s\", wanted \"%s\"   FAILED\n", what, got ? got : "(null)",
           want ? want : "(null)");
}

static int
test_done(const char *name)
{
    if (failures) {
        printf("\n%d check%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("\n%s: all checks passed\n", name);
    return 0;
}

#endif
