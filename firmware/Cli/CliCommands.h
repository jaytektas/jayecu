#pragma once

// Built-in CLI commands. Call once at startup (CommsManager does this lazily on
// the first 'E' command). Idempotent.
namespace Cli { void register_default_commands(); }
