#include "test_helpers.h"
#include "../firmware/Cli/CliRegistry.h"
#include <cstring>

using namespace Cli;

// --- dummy commands exercising every Args shape + side effects ---------------
static int     g_ping = 0;
static int32_t g_i0 = 0, g_i1 = 0;
static float   g_f0 = 0.0f;
static char    g_s0[32] = {0}, g_s1[32] = {0};

static void h_ping(const Argv&, Out& o)  { g_ping++; o.put("pong"); }
static void h_i  (const Argv& a, Out& o) { g_i0 = a.i[0]; o.print_i32(a.i[0]); }
static void h_ii (const Argv& a, Out&)   { g_i0 = a.i[0]; g_i1 = a.i[1]; }
static void h_f  (const Argv& a, Out&)   { g_f0 = a.f[0]; }
static void h_ss (const Argv& a, Out&)   { strncpy(g_s0, a.s[0], 31); strncpy(g_s1, a.s[1], 31); }
static void h_big(const Argv&, Out& o)   { for (int i = 0; i < 1000; i++) o.putc('x'); }

static const Command kCmds[] = {
    { "ping",  Args::NONE, h_ping, "ping" },
    { "seti",  Args::I,    h_i,    "seti <n>" },
    { "setii", Args::II,   h_ii,   "setii <a> <b>" },
    { "setf",  Args::F,    h_f,    "setf <x>" },
    { "setss", Args::SS,   h_ss,   "setss <a> <b>" },
    { "big",   Args::NONE, h_big,  "big" },
};

// run a command line through a fresh sink (line is copied so the literal stays const)
static void run(const char* line, Out& o) {
    char buf[Cli::LINE_MAX];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    execute(buf, o);
}
static Out sink(char* rb, uint16_t cap) { Out o; o.buf = rb; o.cap = cap; o.len = 0; return o; }

int main() {
    fprintf(stdout, "=== Cli ===\n");

    reset();
    CHECK(register_commands(kCmds, 6) == 6);
    CHECK(command_count() == 6);

    SECTION("NONE command runs and emits output");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb)); g_ping = 0;
        run("ping", o);
        CHECK(g_ping == 1);
        CHECK(o.len == 4);
        CHECK(memcmp(rb, "pong", 4) == 0);
    }

    SECTION("int arg — decimal and 0x-hex");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("seti 7000", o);  CHECK(g_i0 == 7000);
        o.len = 0;
        run("seti 0x10", o);  CHECK(g_i0 == 16);
    }

    SECTION("two ints");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("setii 3 9", o);
        CHECK(g_i0 == 3); CHECK(g_i1 == 9);
    }

    SECTION("float arg");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("setf 12.5", o);
        CHECK_NEAR(g_f0, 12.5, 1e-4);
    }

    SECTION("two strings, second quoted with a space");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("setss hello \"two words\"", o);
        CHECK(strcmp(g_s0, "hello") == 0);
        CHECK(strcmp(g_s1, "two words") == 0);
    }

    SECTION("unknown command → ?unknown");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb)); g_ping = 0;
        run("frobnicate", o);
        CHECK(o.len > 0);
        CHECK(strncmp(rb, "?unknown", 8) == 0);
        CHECK(g_ping == 0);          // no handler ran
    }

    SECTION("missing arg → ?usage, handler not run");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb)); g_i0 = 12345;
        run("seti", o);
        CHECK(strncmp(rb, "?usage", 6) == 0);
        CHECK(g_i0 == 12345);        // unchanged — h_i never called
    }

    SECTION("non-numeric arg → ?usage");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("seti abc", o);
        CHECK(strncmp(rb, "?usage", 6) == 0);
    }

    SECTION("empty / whitespace line → no output");
    {
        char rb[64]; Out o = sink(rb, sizeof(rb));
        run("   ", o);
        CHECK(o.len == 0);
    }

    SECTION("output is clamped to cap (never overruns)");
    {
        char rb[16]; Out o = sink(rb, sizeof(rb));
        run("big", o);              // tries to write 1000 bytes into 16
        CHECK(o.len < sizeof(rb));  // bounded, no overflow
    }

    SECTION("registry capacity is bounded");
    {
        reset();
        Command pad[CLI_MAX_COMMANDS + 4];
        for (auto& c : pad) { c.token = "x"; c.args = Args::NONE; c.fn = h_ping; c.help = "x"; }
        size_t added = register_commands(pad, CLI_MAX_COMMANDS + 4);
        CHECK(added == CLI_MAX_COMMANDS);
        CHECK(command_count() == CLI_MAX_COMMANDS);
    }

    return test_summary();
}
