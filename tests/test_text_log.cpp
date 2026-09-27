// Host unit test for TextLog — the flat text console (line append + double-buffer
// swap-drain on read). Replaces the old structured 'D' record ring.
//   build: tests/CMakeLists.txt -> ctest -R text_log
#include "test_helpers.h"
#include "Diagnostics/TextLog.h"
#include <cstring>

int main() {
    fprintf(stdout, "=== TextLog ===\n");

    SECTION("empty drain returns zero length");
    {
        TextLog log;
        size_t n = 123;
        const char* t = log.swap_drain(&n);
        CHECK(n == 0);
        CHECK(t[0] == '\0');
    }

    SECTION("lines accumulate, one drain takes them all, next drain is empty");
    {
        TextLog log;
        log.printf("hello %d\n", 7);
        log.puts("world\n");
        size_t n = 0;
        const char* t = log.swap_drain(&n);
        CHECK(strcmp(t, "hello 7\nworld\n") == 0);
        CHECK(n == strlen("hello 7\nworld\n"));
        // drained — the next read is empty (the write buffer was reset on swap)
        const char* t2 = log.swap_drain(&n);
        CHECK(n == 0);
        CHECK(t2[0] == '\0');
    }

    SECTION("writes after a drain land in the fresh buffer");
    {
        TextLog log;
        log.puts("first\n");
        size_t n = 0;
        (void)log.swap_drain(&n);
        log.puts("second\n");
        const char* t = log.swap_drain(&n);
        CHECK(strcmp(t, "second\n") == 0);
    }

    SECTION("overrun truncates (lossy) and never overflows the buffer");
    {
        TextLog log;
        // Each line is LINE_MAX-bounded; push well past BUF_SIZE worth.
        char chunk[200];
        memset(chunk, 'x', sizeof(chunk) - 1);
        chunk[sizeof(chunk) - 1] = '\0';
        for (int i = 0; i < 100; i++) log.puts(chunk);   // ~20 KB into a 2 KB buffer
        size_t n = 0;
        const char* t = log.swap_drain(&n);
        CHECK(n <= TextLog::BUF_SIZE - 1);     // never exceeds capacity
        CHECK(strlen(t) == n);                 // stays null-terminated and consistent
    }

    return test_summary();
}
