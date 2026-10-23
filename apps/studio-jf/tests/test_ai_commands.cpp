// The studio's AI-bus commands (src/app/AiCommands.h) against the real meta: what an agent reads is what
// the tune holds, and what it writes lands in the tune — in the units the studio shows, by name.
//
//   cmake --build build --target ai_commands_test && ./build/ai_commands_test
#include "app/AiCommands.h"
#include "model/Cache.h"
#include "model/MetaModel.h"

#include <j/config/Json.h>

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}
static jf::JJson call(const std::string& action) {
    std::string reply;
    if (!aicmd::handle(action, aicmd::Host{}, reply)) return jf::JJson();
    auto j = jf::JJson::tryParse(reply);
    return j ? *j : jf::JJson();
}

int main() {
    std::puts("=== nothing open: the commands say so ===");
    ck(!call("get engine.cylinder_count")["ok"].boolean(true), "a read with no tune is refused, not answered with zeros");
    ck(call("help")["ok"].boolean(), "help answers anyway");
    std::string r;
    ck(!aicmd::handle("click", aicmd::Host{}, r), "a widget action is not ours (the bus's own dispatch has it)");

    // What a button does, by name: the app registers a verb, and the bus answers it and lists it.
    {
        aicmd::Host h;
        h.verbs["echo"] = { "echo <words>", "say it back",
            [](const std::string& args, jf::JJson& out, std::string& why) {
                if (args.empty()) { why = "echo needs words"; return false; }
                out["said"] = jf::JJson(args);
                return true;
            } };
        std::string rr;
        ck(aicmd::handle("echo hello there", h, rr) && jf::JJson::tryParse(rr)->operator[]("said").str() == "hello there",
           "an app verb is answered with what it put in the reply", rr);
        ck(aicmd::handle("echo", h, rr) && !(*jf::JJson::tryParse(rr))["ok"].boolean(true)
           && (*jf::JJson::tryParse(rr))["error"].str() == "echo needs words", "…and its refusal says why", rr);
        ck(aicmd::handle("help", h, rr) && rr.find("echo <words>") != std::string::npos, "help lists it");
    }

    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&m);
    c.setConfigImage(m.defaultImage());

    std::puts("\n=== settings ===");
    jf::JJson g = call("get engine.cylinder_count");
    ck(g["ok"].boolean() && g["value"].isNumber(), "get reads a number", g.dump());
    jf::JJson s = call("set engine.cylinder_count 6");
    ck(s["ok"].boolean() && s["value"].number() == 6.0, "set writes it", s.dump());
    ck(call("get engine.cylinder_count")["value"].number() == 6.0, "…and it reads back");
    ck(!call("set engine.cylinder_count 99")["ok"].boolean(true), "out of range is refused");
    ck(!call("set engine.cylinder_count six")["ok"].boolean(true), "not a number is refused");

    ck(call("set sensors.sensor[clt].interface digital_freq")["ok"].boolean(), "a choice is set by its id");
    ck(call("get sensors.sensor[clt].interface")["id"].str() == "digital_freq", "…and reads back by id and label");
    ck(call("set sensors.sensor[clt].interface Analogue Voltage")["ok"].boolean()
       && call("get sensors.sensor[clt].interface")["id"].str() == "analog_voltage", "…or by its label");
    // A MODULE-LEVEL condition program. Its byte length was never carried for a plain module field, so
    // the bus judged every program "too long: N bytes of 0" — Kelly's launch Arm When could not be set.
    {
        const auto e = call("set launch.arm_expr brake_sw > 0.5 and vehicle_spd < 5 and pedal_demand > 20");
        ck(e["ok"].boolean(), "a module-level expression (launch Arm When) is set", e.dump());
        ck(call("get launch.arm_expr")["value"].str().find("brake_sw") != std::string::npos, "…and reads back");
        ck(call("set launch.arm_expr ")["ok"].boolean() && call("get launch.arm_expr")["value"].str().empty(),
           "…and empty puts the built-in rule back");
    }
    ck(call("set vehicle.Name Bench Car")["ok"].boolean()
       && call("get vehicle.Name")["value"].str() == "Bench Car", "text is written whole, spaces and all");

    jf::JJson cals = call("tables lambda");
    bool named = false;
    for (const jf::JJson& e : cals["tables"].arr()) if (e["path"].str() == "sensors.sensor[lambda_1].cal") named = true;
    ck(named, "a sensor's calibration curve is listed by the sensor's id, so 'tables lambda' finds it");

    jf::JJson list = call("settings cylinder_count");
    ck(list["ok"].boolean() && list["count"].number() >= 1, "settings finds by filter", list.dump().substr(0, 120));

    std::puts("\n=== tables ===");
    const std::string T = "fuel_calculator.ve_table";
    jf::JJson t = call("table " + T);
    const int cols = t["cols"].number<int>(), rows = t["rows"].number<int>();
    ck(t["ok"].boolean() && cols > 1 && rows > 1, "table reads the VE table", std::to_string(cols) + "x" + std::to_string(rows));
    ck(int(t["cells"].arr().size()) == rows && int(t["cells"][0].arr().size()) == cols, "…cells are rows of columns");
    ck(int(t["x"]["bins"].arr().size()) == cols && int(t["y"]["bins"].arr().size()) == rows, "…with each axis's bins");
    ck(call("cell " + T + " 1 2 87.5")["ok"].boolean(), "cell writes one cell");
    ck(call("table " + T)["cells"][2][1].number() == 87.5, "…the one at column 1, row 2");
    std::string grid = "[";
    for (int r2 = 0; r2 < rows; ++r2) {
        grid += r2 ? ",[" : "[";
        for (int c2 = 0; c2 < cols; ++c2) grid += (c2 ? "," : "") + std::to_string(50 + r2);
        grid += "]";
    }
    grid += "]";
    ck(call("table-set " + T + " " + grid)["ok"].boolean(), "table-set writes the whole grid");
    jf::JJson t2 = call("table " + T);
    ck(t2["cells"][0][0].number() == 50.0 && t2["cells"][rows - 1][cols - 1].number() == 50.0 + rows - 1,
       "…every cell where it was put");
    ck(!call("table-set " + T + " [[1,2]]")["ok"].boolean(true), "a grid of the wrong shape is refused");
    ck(call("table " + T)["cells"][0][0].number() == 50.0, "…and writes nothing");

    // The breakpoints: the part of a table its cells cannot reach.
    std::string bins = "[";
    for (int c2 = 0; c2 < cols; ++c2) bins += (c2 ? "," : "") + std::to_string(500 + 500 * c2);
    bins += "]";
    ck(call("axis-set " + T + " x " + bins)["ok"].boolean(), "axis-set writes an axis's breakpoints");
    ck(call("table " + T)["x"]["bins"][1].number() == 1000.0, "…and they read back");
    ck(!call("axis-set " + T + " x [1,2]")["ok"].boolean(true), "the wrong number of breakpoints is refused");
    std::string down = "[";
    for (int c2 = 0; c2 < cols; ++c2) down += (c2 ? "," : "") + std::to_string(9000 - 500 * c2);
    down += "]";
    ck(!call("axis-set " + T + " x " + down)["ok"].boolean(true), "…and so are breakpoints that do not ascend");
    ck(call("table " + T)["x"]["bins"][1].number() == 1000.0, "…which write nothing");
    ck(!call("axis-set " + T + " q " + bins)["ok"].boolean(true), "an axis that is not x, y or z is refused");

    std::puts("\n=== the tune is locked ===");
    c.setReadOnly(true);
    ck(!call("set engine.cylinder_count 4")["ok"].boolean(true), "a locked tune refuses a write");
    c.setReadOnly(false);

    std::puts("\n=== channels ===");
    jf::JJson ch = call("channels rpm");
    ck(ch["ok"].boolean() && ch["count"].number() >= 1, "channels lists the live channels");
    jf::JJson lv = call("live rpm");
    ck(lv["ok"].boolean() && !lv["note"].str().empty(), "live with no ECU says nothing has arrived");

    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "the AI commands read and write the tune",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
