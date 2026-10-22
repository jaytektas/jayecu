#pragma once

// AiCommands — the studio's own commands on the AI bus (JF_AI_BUS=1): the tune and the live data BY
// NAME, for an agent or a script, with or without a window.
//
// The bus's generic actions drive WIDGETS: what is on screen, by the 64 characters of its caption. That
// reaches one page at a time and nothing a page does not show. These reach the model instead — every
// setting, every table, every live channel the meta describes — and answer in JSON, so the same commands
// work from the studio's window and from `studio --headless`, where there is no widget at all.
//
//   jf-busctl call help                      the command list, with examples
//   jf-busctl call status                    link, ECU, tune, definition
//   jf-busctl call 'settings fuel'           every setting whose path or label contains "fuel"
//   jf-busctl call 'get engine.cylinders'    one setting: value, units, range, options, help
//   jf-busctl call 'set engine.cylinders 6'  write it (to the ECU too, when connected; RAM until burn)
//   jf-busctl call 'tables ve'               the tables, with their sizes
//   jf-busctl call 'table fuel_calculator.ve_table'      axes and cells
//   jf-busctl call 'table-set <path> [[..],[..]]'        write cells (rows of columns), one undo step
//   jf-busctl call 'cell <path> <col> <row> <value>'     write one cell
//   jf-busctl call 'channels rpm'            the live channels
//   jf-busctl call 'live rpm map clt'        their values now (no names: every channel)
//   jf-busctl call burn                      persist the ECU's RAM tune to flash
//   jf-busctl call save                      save the tune to the ECU's project folder
//
// Every reply is one JSON object with "ok": true, or "ok": false and "error". Values are in the units
// the studio shows (engineering units, "units" says which); options are named by label and id.
//
// Documented for users in the manual (part 6, "Driving the studio from a script or an AI").

#include <functional>
#include <map>
#include <string>

#include <j/config/Json.h>

namespace aicmd {

// What only the running app can do or say. Each is optional; a command whose hook is missing says so.
struct Host {
    std::function<jf::JJson()>                               status;   // link / ECU / tune facts
    std::function<bool(std::string& why)>                    burn;     // false + why when it cannot
    std::function<bool(std::string& why)>                    save;
    std::function<bool(const std::string& path, std::string& why)> page;   // show a page (window only)

    // EVERYTHING ELSE THE APP CAN DO, by name. What a tuner reaches through a button — connect, Auto
    // Tune, the card's logs, the trouble codes — a script has to reach too, or it is left asking the
    // person at the wheel to click for it. The app registers each with its own usage line, so `help`
    // lists exactly what this build answers. `run` fills `out` (merged into the reply) or says why not.
    struct Verb {
        std::string usage, help;
        std::function<bool(const std::string& args, jf::JJson& out, std::string& why)> run;
    };
    std::map<std::string, Verb> verbs;
};

// Handle `action` if it is one of these commands: fills `reply` (JSON) and returns true. False means
// the action is not ours, and the caller tries its other handlers.
bool handle(const std::string& action, const Host& host, std::string& reply);

}  // namespace aicmd
