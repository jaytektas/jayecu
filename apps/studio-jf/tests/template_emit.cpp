// Run the OUTPUT WIZARD'S OWN APPLY for every template, and emit what it would write.
//
// Not a re-implementation: templateWrites() is the function OutputWizardDialog::_apply() calls, and
// the two condition compiles below are the same writeExpr() it does. What this skips is the dialog —
// the spin boxes and the Apply button — because a template's DEFAULTS are what a wizard offers before
// anybody touches them, and that is exactly the configuration worth proving works.
#include "model/OutputTemplate.h"
#include "model/ExprCompiler.h"
#include "model/MetaModel.h"
#include "model/Cache.h"

#include <j/config/Json.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static MetaModel M;

static void emitJsonStr(const std::string& s) {
    std::putchar('"');
    for (char c : s) {
        if (c == '"' || c == '\\') { std::putchar('\\'); std::putchar(c); }
        else if (c == '\n') std::fputs("\\n", stdout);
        else std::putchar(c);
    }
    std::putchar('"');
}

int main(int argc, char** argv) {
    if (argc < 2 || !M.loadFile(argv[1])) { std::fputs("no meta\n", stderr); return 1; }
    Cache& c = Cache::instance();
    c.setMeta(&M);
    c.setConfigImage(M.defaultImage());

    const auto templates = outputTemplatesFromMeta(jf::JJson::parseFile(M.path()));
    const int slotIdx = argc > 2 ? std::atoi(argv[2]) : 0;
    const std::string slot = "outputs.output[" + std::to_string(slotIdx) + "]";

    std::fputs("[\n", stdout);
    bool firstT = true;
    for (const OutputTemplate& t : templates) {
        std::vector<double> vals;
        for (const auto& p : t.params) vals.push_back(p.def);       // what the wizard offers
        const TemplateWrites w = templateWrites(t, slot, vals, templateDefaultChoice(t));

        if (!firstT) std::fputs(",\n", stdout);
        firstT = false;
        std::fputs("{\"id\":", stdout);      emitJsonStr(t.id);
        std::fputs(",\"name\":", stdout);    emitJsonStr(w.name);
        std::fputs(",\"inputs\":[", stdout);
        // The inputs the DEFAULT choice uses — the same list the wizard's wired/not-wired rows show,
        // so a bench that wires what this says has wired what the emitted conditions actually read.
        const std::vector<std::string> ins = templateInputs(t, templateDefaultChoice(t));
        for (size_t i = 0; i < ins.size(); ++i) { if (i) std::putchar(','); emitJsonStr(ins[i]); }
        std::fputs("],\"values\":{", stdout);
        bool firstV = true;
        for (const auto& [path, v] : w.values) {
            if (!firstV) std::putchar(',');
            firstV = false;
            emitJsonStr(path.substr(slot.size() + 1));
            std::printf(":%.10g", v);
        }
        std::fputs("},\"src\":{", stdout);
        std::fputs("\"on\":", stdout);  emitJsonStr(w.onSource);
        std::fputs(",\"off\":", stdout); emitJsonStr(w.offSource);
        std::fputs("},\"code\":{", stdout);
        bool firstC = true;
        for (const char* f : {"on_expr", "off_expr", "freq_expr"}) {
            int off = 0, size = 0;
            std::string hex, err;
            // A TEMPLATE THAT COMPUTES ITS CARRIER (the tachometer) compiles it too; the rest emit "".
            const std::string src = std::string(f) == "on_expr"  ? w.onSource
                                  : std::string(f) == "off_expr" ? w.offSource : w.freqSource;
            if (!src.empty() && M.resolveBlob(slot + "." + f, off, size)) {
                const auto r = ExprCompiler::compile(src, M, uint32_t(M.configSize()), uint16_t(size));
                if (r.ok) { char b[4]; for (uint8_t x : r.code) { std::snprintf(b, sizeof b, "%02x", x); hex += b; } }
                else err = r.error;
            }
            if (!firstC) std::putchar(',');
            firstC = false;
            emitJsonStr(f); std::putchar(':'); emitJsonStr(hex);
            if (!err.empty()) { std::fputs(",\"" , stdout); std::fputs(f, stdout);
                                std::fputs("_error\":", stdout); emitJsonStr(err); }
        }
        std::fputs("}}", stdout);
    }
    std::fputs("\n]\n", stdout);
    return 0;
}
