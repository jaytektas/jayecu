#include "Log.h"

// Default weak no-op definitions for the EFI diagnostic log hooks.
//
// Log.h only *declares* efi_log_debug()/efi_log_warn() as weak. Without a
// definition linked in, the ARM GNU linker silently resolves the calls to a
// no-op, but a host (x86) build resolves the undefined-weak symbol to address
// 0 and crashes the moment any EFI_LOG_* macro fires (e.g. ConfigBank::save).
//
// Defining them here as weak no-ops makes the "no-ops by default" contract real
// on every target, while still letting a platform sink (USB/UART/SWD logger)
// override them with a strong definition.
extern "C" {

__attribute__((weak)) void efi_log_debug(const char* /*category*/, const char* /*msg*/) {}
__attribute__((weak)) void efi_log_warn (const char* /*category*/, const char* /*msg*/) {}

}  // extern "C"
