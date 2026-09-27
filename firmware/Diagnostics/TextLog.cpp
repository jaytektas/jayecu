#include "TextLog.h"
#include "../Signal/SignalLock.h"   // SigLockGuard — a FreeRTOS critical section (host: no-op)

#include <cstdio>
#include <cstring>

TextLog g_text_log;

void TextLog::printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void TextLog::vprintf(const char* fmt, va_list ap) {
    char line[LINE_MAX];
    const int n = vsnprintf(line, sizeof(line), fmt, ap);   // formats on the caller's stack, no lock
    if (n <= 0) return;
    size_t len = (static_cast<size_t>(n) < sizeof(line)) ? static_cast<size_t>(n) : sizeof(line) - 1;
    append_(line, len);
}

void TextLog::puts(const char* s) {
    if (s) append_(s, strlen(s));
}

void TextLog::append_(const char* s, size_t len) {
    SigLockGuard g(true);   // brief critical section: makes the append atomic vs a concurrent drain
    size_t avail = BUF_SIZE - 1 - write_len_;   // keep one byte for the null terminator
    if (len > avail) len = avail;               // overrun -> truncate this line (lossy, best-effort)
    if (len) {
        memcpy(write_ + write_len_, s, len);
        write_len_ += len;
        write_[write_len_] = '\0';
    }
}

const char* TextLog::swap_drain(size_t* out_len) {
    {
        SigLockGuard g(true);
        char* t = write_; write_ = read_; read_ = t;   // swap
        read_len_  = write_len_;
        write_len_ = 0;
        write_[0]  = '\0';                              // the new write buffer starts empty
    }
    if (out_len) *out_len = read_len_;
    return read_;
}
