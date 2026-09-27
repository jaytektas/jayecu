// AXIS HEADINGS THAT ARE NAMES, from the definition that declares them.
//
// Some axes have no quantity to print. The VVT long-term trim's four rows are the cams — Intake B1,
// Exhaust B1, Intake B2, Exhaust B2 — and the bank trim's two columns are banks 1 and 2. The schema
// has been able to say so since the learned region was added (`row_labels`), and nothing carried it:
// codegen's meta emitter dropped the key, so every one of those axes showed its storage index and the
// declaration did nothing at all. This checks the whole chain the studio depends on — meta -> model ->
// descriptor — because each link in it was, at some point, the one that was missing.
//
//   cmake --build build --target axis_labels_test && ./build/axis_labels_test
#include "model/MetaModel.h"

#include <cstdio>
#include <string>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& detail = "") {
    std::printf("  %s  %s%s\n", ok ? "PASS" : "FAIL", what.c_str(),
                detail.empty() ? "" : ("   - " + detail).c_str());
    if (!ok) ++fails;
}

int main() {
    MetaModel m;
    if (!m.loadFile(REAL_META)) { std::fprintf(stderr, "cannot load %s\n", REAL_META); return 1; }

    const TableImage vvt  = m.resolveTable("vvt_control.vvt_ltt");
    const TableImage bank = m.resolveTable("fuel_calculator.lambda_bank_trim");
    ck(vvt.valid && bank.valid, "both learned tables resolve");

    // The cams. Four rows, four names, in the loop order the firmware writes them.
    ck(vvt.rowLabels.size() == 4, "the VVT trim's rows are named",
       std::to_string(vvt.rowLabels.size()) + " label(s)");
    if (vvt.rowLabels.size() == 4) {
        ck(vvt.rowLabels[0] == "Intake B1" && vvt.rowLabels[3] == "Exhaust B2",
           "…with the cams in the firmware's own loop order",
           vvt.rowLabels[0] + " … " + vvt.rowLabels[3]);
    }

    // The banks. "1" over a two-column table is as easily a count as a bank number.
    ck(bank.colLabels.size() == 2, "the bank trim's columns are named",
       std::to_string(bank.colLabels.size()) + " label(s)");
    if (bank.colLabels.size() == 2)
        ck(bank.colLabels[0] == "Bank 1" && bank.colLabels[1] == "Bank 2",
           "…as banks, not indices", bank.colLabels[0] + " / " + bank.colLabels[1]);

    // AND AN ORDINARY MAP CARRIES NONE. The breakpoints ARE the heading there, and a label list would
    // hide the rpm and load a tuner is reading.
    const TableImage ve = m.resolveTable("fuel_calculator.ve_table");
    ck(ve.valid && ve.rowLabels.empty() && ve.colLabels.empty(),
       "the VE table has no labels - its breakpoints are its headings");

    std::printf("\n%s\n", fails ? (std::to_string(fails) + " FAILED").c_str() : "ALL PASSED");
    return fails ? 1 : 0;
}
