#pragma once
// ---------------------------------------------------------------------------
// EFI diagnostic logging stubs.
//
// efi_log_debug() and efi_log_warn() are weak-linked no-ops by default.
// The communication layer (UART, CAN, SWD ITM) overrides them at link time
// in a later firmware turn.
//
// EFI_LOG_DEBUG / EFI_LOG_WARN format into a 96-byte stack buffer with
// __builtin_snprintf (no heap, no blocking). These macros must NOT be called
// from the crank-edge ISR hot path — only from task context or infrequent
// callback paths (sync state changes, fault detection, etc.).
// ---------------------------------------------------------------------------

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Weak DECLARATIONS. Log.cpp carries weak definitions, but a translation unit that does not link it
// leaves these unresolved — and an unresolved weak symbol resolves to ADDRESS ZERO, it does not
// vanish. The comment here used to say the call sites "silently do nothing" in that case; they do not,
// they branch to 0. Nothing noticed because the only call sites are failure paths (a denied pin claim)
// that nothing had ever exercised — the first host test to contest a coil pin segfaulted with no
// output at all, before its first line printed. Hence the null test in the macros below: taking a weak
// symbol's address is exactly how you ask whether it was linked, and it costs one compare on a path
// that only runs when something has already gone wrong.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void efi_log_debug(const char* category, const char* msg);
__attribute__((weak)) void efi_log_warn (const char* category, const char* msg);
#else
// Non-GCC/Clang: declare as external; provide no-op definitions in a
// platform-specific translation unit.
void efi_log_debug(const char* category, const char* msg);
void efi_log_warn (const char* category, const char* msg);
#endif

#ifdef __cplusplus
}
#endif

// Variadic macros: format into a fixed-size stack buffer, then call the stub.
#define EFI_LOG_DEBUG(cat, ...)                                             \
    do {                                                                    \
        char _efi_log_buf[96];                                              \
        __builtin_snprintf(_efi_log_buf, sizeof(_efi_log_buf),              \
                           __VA_ARGS__);                                    \
        if (efi_log_debug) efi_log_debug((cat), _efi_log_buf);              \
    } while (0)

#define EFI_LOG_WARN(cat, ...)                                              \
    do {                                                                    \
        char _efi_log_buf[96];                                              \
        __builtin_snprintf(_efi_log_buf, sizeof(_efi_log_buf),              \
                           __VA_ARGS__);                                    \
        if (efi_log_warn) efi_log_warn((cat), _efi_log_buf);                \
    } while (0)
