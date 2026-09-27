#pragma once

#include <cstdint>
#include <cstddef>

// ---------------------------------------------------------------------------
// Cli — a tiny text command registry.
//
// A second, plaintext command protocol that rides the existing binary
// (msEnvelope) link: the 'E'/execute command (and any raw console)
// feeds a text line in here; the matched command writes its reply into a
// bounded Out sink, which CommsManager returns as the packet payload.
//
// Design constraints (embedded): no heap, fixed-size registry (CLI_MAX_COMMANDS),
// bounded output (never overruns its buffer). Commands self-register via static
// arrays — adding one is a local change in the owning module, never a central edit.
// ---------------------------------------------------------------------------

namespace Cli {

// Wire/handling limits. REPLY_MAX is the reply BUFFER size (may exceed one frame): the CLI handler
// caps the transmitted reply at OMNI_MAX_PAYLOAD, so a reply larger than a frame is truncated on send.
static constexpr uint16_t REPLY_MAX = 2048;  // max bytes of text reply
static constexpr uint16_t LINE_MAX  = 128;   // max incoming command line

#ifndef CLI_MAX_COMMANDS
#define CLI_MAX_COMMANDS 48                  // registry capacity (single source)
#endif

// Bounded output sink a command writes into. The buffer is owned by the caller;
// every write is clamped to `cap`, so a command can never overrun it.
struct Out {
    char*    buf = nullptr;
    uint16_t cap = 0;
    uint16_t len = 0;
    void putc(char c);
    void put(const char* s);
    void print_i32(int32_t v);
    void print_u32(uint32_t v);
};

// Argument shapes a command can declare. The dispatcher parses the line's
// tail into Argv per this before invoking the handler.
// OPT_I: one OPTIONAL integer, defaulting to 0 when absent. For commands that page or take a level
// but must still work bare -- Args::I rejects the bare call before the handler runs, so a command that
// gains an optional argument would quietly start printing usage for the form everyone types.
// OPT_S: one OPTIONAL string, nullptr when absent -- for a command whose bare form REPORTS and whose
// argument form ACTS (`dtc` shows the counts, `dtc clear` wipes). Args::S rejected the bare call
// before the handler ran, so the reporting branch was unreachable and typing `dtc` printed usage.
enum class Args : uint8_t { NONE, I, OPT_I, II, III, IIII, F, S, OPT_S, SS };

// Parsed arguments handed to a handler (only the fields its Args declares are valid).
struct Argv {
    int32_t     i[4] = { 0, 0, 0, 0 };
    float       f[2] = { 0.0f, 0.0f };
    const char* s[2] = { nullptr, nullptr };
};

using Handler = void (*)(const Argv& args, Out& out);

struct Command {
    const char* token;     // command name (first whitespace-delimited word)
    Args        args;      // argument shape
    Handler     fn;        // callback
    const char* help;      // one-line usage, shown on an arg error
};

// Append a static array of commands to the registry. Returns the number added
// (fewer than n only if the registry is full).
size_t register_commands(const Command* cmds, size_t n);

// Clear the registry (used by tests).
void reset();

// Current registered command count.
size_t command_count();

// Parse and run one command line (mutated in place during tokenisation) and
// write any reply into `out`. Empty line → no output. Unknown command or bad
// args → a short "?…" diagnostic.
void execute(char* line, Out& out);

} // namespace Cli
