#pragma once

#include <cstddef>
#include <cstdarg>

#include "../../generated/schema_meta.h"   // JAYECU_BLOCK_SIZE — one 'D' drain must fit one comms reply

// ---------------------------------------------------------------------------
// TextLog — a plain-text console the firmware streams to the host. Any thread (or ISR) calls printf()/puts(); the lines
// accumulate in a write buffer. The comms task calls swap_drain() to atomically
// take everything logged since the last drain (a double-buffer swap) and ship it
// to the host, which appends it to a flat scrolling console.
//
// There is no record structure, no seq, no cursor — just text. It is lossy when
// full (best-effort, like a real serial console): a producer that overruns the
// buffer between drains has its tail truncated rather than blocking. Producers
// never block beyond a short critical section around the append (formatting
// happens on the caller's stack, outside the lock). The double buffer means a
// drain never blocks a producer either — they touch different buffers.
// ---------------------------------------------------------------------------

class TextLog {
public:
    // Per buffer. One drain returns at most BUF_SIZE-1 bytes; sized to the comms block
    // (JAYECU_BLOCK_SIZE, the single source of truth) so a full drain always fits one 'D'
    // reply frame — no text is silently dropped past the block boundary. Track the block:
    // when the tuning block shrank 4K->1K, a hardcoded 2048 here would overrun one reply.
    static constexpr size_t BUF_SIZE  = JAYECU_BLOCK_SIZE;
    static constexpr size_t LINE_MAX  = 256;   // one formatted line, on the caller's stack

    TextLog() { buf_[0][0] = '\0'; buf_[1][0] = '\0'; }   // both start as valid empty strings

    // printf a line into the log. Append a '\n' yourself if you want line breaks
    // (the host shows the text verbatim).
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void vprintf(const char* fmt, va_list ap);
    void puts(const char* s);   // append raw text, no formatting

    // Comms reader: swap write<->read, reset write, return the drained read buffer
    // (null-terminated) and its length. Valid until the next swap_drain().
    const char* swap_drain(size_t* out_len);

private:
    void append_(const char* s, size_t len);

    char   buf_[2][BUF_SIZE];
    char*  write_     = buf_[0];
    char*  read_      = buf_[1];
    size_t write_len_ = 0;
    size_t read_len_  = 0;
};

extern TextLog g_text_log;   // the single shared instance (like g_config_generation)
