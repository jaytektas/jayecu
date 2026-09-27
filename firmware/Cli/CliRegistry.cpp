#include "CliRegistry.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace Cli {

// --- Out: every write clamped to cap (never overruns the caller's buffer) ---
void Out::putc(char c)            { if (buf && len + 1u < cap) buf[len++] = c; }
void Out::put(const char* s)      { if (s) while (*s) putc(*s++); }
void Out::print_i32(int32_t v)    { char t[12]; int n = snprintf(t, sizeof(t), "%ld",  (long)v);          for (int i = 0; i < n; i++) putc(t[i]); }
void Out::print_u32(uint32_t v)   { char t[12]; int n = snprintf(t, sizeof(t), "%lu", (unsigned long)v);  for (int i = 0; i < n; i++) putc(t[i]); }

namespace {

Command g_cmds[CLI_MAX_COMMANDS];
size_t  g_count = 0;

// Next whitespace-delimited token, NUL-terminated in place; advances p past it.
// A "double quoted" token may contain spaces. Returns nullptr at end of line.
char* next_token(char*& p) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') return nullptr;
    char* start;
    if (*p == '"') {
        ++p; start = p;
        while (*p && *p != '"') ++p;
        if (*p == '"') { *p = '\0'; ++p; }
    } else {
        start = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) { *p = '\0'; ++p; }
    }
    return start;
}

// strtol/strtof that reject trailing garbage (the whole token must be numeric).
bool parse_i32(const char* s, int32_t& out) {
    if (!s || !*s) return false;
    char* end; long v = strtol(s, &end, 0);   // base 0 → decimal or 0x-hex
    if (*end != '\0') return false;
    out = (int32_t)v; return true;
}
bool parse_f(const char* s, float& out) {
    if (!s || !*s) return false;
    char* end; float v = strtof(s, &end);
    if (*end != '\0') return false;
    out = v; return true;
}

const Command* find(const char* tok) {
    for (size_t i = 0; i < g_count; i++)
        if (strcmp(g_cmds[i].token, tok) == 0) return &g_cmds[i];
    return nullptr;
}

} // namespace

size_t register_commands(const Command* cmds, size_t n) {
    size_t added = 0;
    for (size_t i = 0; i < n && g_count < CLI_MAX_COMMANDS; i++) {
        g_cmds[g_count++] = cmds[i];
        ++added;
    }
    return added;
}

void   reset()         { g_count = 0; }
size_t command_count() { return g_count; }

void execute(char* line, Out& out) {
    char* p = line;
    char* name = next_token(p);
    if (!name) return;                                  // empty line — no reply

    const Command* c = find(name);
    if (!c) { out.put("?unknown command: "); out.put(name); out.put("\r\n"); return; }

    Argv a;
    bool ok = true;
    switch (c->args) {
        case Args::NONE: break;
        case Args::I:    ok = parse_i32(next_token(p), a.i[0]); break;
        case Args::OPT_I: { char* t = next_token(p);
                            a.i[0] = 0;
                            if (t) { ok = parse_i32(t, a.i[0]); }
                            break; }
        case Args::II:   { char* t0 = next_token(p); char* t1 = next_token(p);
                           ok = parse_i32(t0, a.i[0]) && parse_i32(t1, a.i[1]); break; }
        case Args::III:  { char* t0 = next_token(p); char* t1 = next_token(p); char* t2 = next_token(p);
                           ok = parse_i32(t0, a.i[0]) && parse_i32(t1, a.i[1]) && parse_i32(t2, a.i[2]); break; }
        case Args::IIII: { char* t0 = next_token(p); char* t1 = next_token(p);
                           char* t2 = next_token(p); char* t3 = next_token(p);
                           ok = parse_i32(t0, a.i[0]) && parse_i32(t1, a.i[1])
                             && parse_i32(t2, a.i[2]) && parse_i32(t3, a.i[3]); break; }
        case Args::F:    ok = parse_f(next_token(p), a.f[0]); break;
        case Args::S:    a.s[0] = next_token(p); ok = (a.s[0] != nullptr); break;
        case Args::OPT_S: a.s[0] = next_token(p); break;   // absent -> nullptr, handler decides
        case Args::SS:   a.s[0] = next_token(p); a.s[1] = next_token(p);
                         ok = (a.s[0] != nullptr && a.s[1] != nullptr); break;
    }
    if (!ok) { out.put("?usage: "); out.put(c->help ? c->help : c->token); out.put("\r\n"); return; }

    c->fn(a, out);
}

} // namespace Cli
