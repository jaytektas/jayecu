#pragma once

// Headless — `studio --headless`: the studio with no window, no display and no GPU, serving the AI bus.
//
// It opens one tune — a library ECU's (offline), or the ECU on a port (connected) — and answers the same
// commands the window answers (AiCommands.h) for as long as it runs. Nothing on screen, so nothing that
// needs a screen: the model, the ECU link and the bus. Stops on SIGINT / SIGTERM or the `quit` command.
//
//   studio --headless --ecu <uid | car name>          open that ECU's tune from the library, offline
//   studio --headless --ecu <...> --tune <name>       …a named tune of it
//   studio --headless --connect [port]                connect to the ECU (the only one, or on that port)
//   studio --headless --connect --ecu <uid>           connect to that one of several
//   studio --headless --meta <file>                   a definition's defaults (no ECU at all)
//
// One JSON line on stdout says it is ready (and another when a connection completes); everything else
// goes to the studio log.

#include <string>

namespace headless {

struct Args {
    std::string ecu, tune, port, meta;
    bool        connect = false;
};

int run(const Args& args);

// Windows: the studio is a GUI program, which gets no console — so what it prints for --help and
// --headless would go nowhere when run from one. This attaches to the console it was started from
// (output already redirected to a file or a pipe is left alone). A no-op everywhere else.
void useParentConsole();

}  // namespace headless
