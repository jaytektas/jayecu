// A UID NAMES EXACTLY ONE WIDGET — children included.
//
// A panel keeps its children as JSON inside its own "children" prop. Every path that builds a widget from
// COPIED props — paste, duplicate, a page duplicated onto another node — went through PanelModel::add(),
// which minted a fresh uid for the panel and handed its children the source's. The copy and the original
// then answered to the same Widget ID: "[@<uid>.selectedRow]" could resolve to either, and the document
// carried duplicate ids that nothing would ever notice, because nothing checked.
//
// Pinned here, at add() — the one place a new widget is born (load() restores uids and is a different
// path, deliberately):
//   - a copied panel's children get fresh uids, at every depth
//   - a reference INSIDE the copied subtree follows the copy
//   - a reference to something NOT copied is left alone
//   - the uid map is published so a sibling's reference into those children can be relinked too
//   - restoring a document (load) does NOT re-mint: a saved uid is an identity, not a fresh allocation
//
//   cmake --build build --target child_uids_test && ./build/child_uids_test

#include "../src/surface/PanelModel.h"

#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("[child-uid] %-62s %s%s\n", what, ok ? "PASS" : "FAIL",
                detail.empty() ? "" : ("  - " + detail).c_str());
    if (!ok) ++fails;
}

// Every uid in a children blob, at every depth.
static void collect(const std::string& json, std::set<std::string>& out) {
    auto j = jf::JJson::tryParse(json);
    if (!j || !j->isArray()) return;
    for (const jf::JJson& w : j->arr()) {
        out.insert(w["uid"].str());
        collect(w["props"]["children"].str(), out);
    }
}
static std::string childrenOf(const PanelModel& m, int id) {
    const PanelElement* e = m.get(id);
    return e ? e->prop("children") : std::string();
}

int main() {
    // A panel holding a table and an inner panel, which itself holds a gauge bound to that table by uid —
    // the shape a real page has, and the shape that makes the relink matter.
    const std::string kTable = "11111111-1111-1111-1111-111111111111";
    const std::string kInner = "22222222-2222-2222-2222-222222222222";
    const std::string kGauge = "33333333-3333-3333-3333-333333333333";
    const std::string kOuter = "44444444-4444-4444-4444-444444444444";
    const std::string children =
        "[{\"id\":1,\"uid\":\"" + kTable + "\",\"type\":\"table\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"props\":{}},"
        " {\"id\":2,\"uid\":\"" + kInner + "\",\"type\":\"panel\",\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"props\":{"
        "     \"children\":\"[{\\\"id\\\":1,\\\"uid\\\":\\\"" + kGauge + "\\\",\\\"type\\\":\\\"gauge\\\",\\\"x\\\":0,\\\"y\\\":0,\\\"w\\\":5,\\\"h\\\":5,"
        "        \\\"props\\\":{\\\"minExpr\\\":\\\"[@" + kTable + ".max]\\\",\\\"maxExpr\\\":\\\"[@" + kOuter + ".max]\\\"}}]\"}}]";

    PanelModel m;
    const int src = m.add("panel", 0, 0, 100, 100, { { "children", children } });

    std::set<std::string> orig;
    collect(childrenOf(m, src), orig);
    check(orig.size() == 3, "the fixture's three nested widgets are readable", std::to_string(orig.size()));
    check(!orig.count(kTable) && !orig.count(kInner) && !orig.count(kGauge),
          "even the FIRST add re-mints — copied props are copied props");

    // The copy: the same props again, exactly as paste/duplicate hands them over.
    const PanelElement* se = m.get(src);
    const int cpy = m.add("panel", 20, 20, 100, 100, se->props);
    const auto map = m.lastAddChildUids();

    std::set<std::string> copied;
    collect(childrenOf(m, cpy), copied);
    check(copied.size() == 3, "the copy has the same three widgets", std::to_string(copied.size()));

    std::set<std::string> shared;
    for (const std::string& u : copied) if (orig.count(u)) shared.insert(u);
    check(shared.empty(), "and NOT ONE uid is shared with the original",
          shared.empty() ? "" : *shared.begin());
    check(m.get(src)->uid != m.get(cpy)->uid, "the panels themselves still differ too");

    // The gauge referenced the table it was copied WITH: it must now follow the copy.
    const std::string blob = childrenOf(m, cpy);
    bool refFollows = false, refToOutsideKept = blob.find("@" + kOuter) != std::string::npos;
    for (const std::string& u : copied)
        if (blob.find("@" + u) != std::string::npos) refFollows = true;
    check(refFollows, "a reference inside the copied subtree points at the COPY");
    check(blob.find("@" + kTable) == std::string::npos, "... and no longer at the original's table");
    check(refToOutsideKept, "a reference to something NOT copied is left alone");

    // The map is what lets a SIBLING's reference into these children be relinked (Surface::paste).
    check(map.size() == 3, "add() publishes the old->new child uids", std::to_string(map.size()));
    bool mapSane = true;
    for (const auto& [o, n] : map) if (o == n || !copied.count(n)) mapSane = false;
    check(mapSane, "... and every entry maps an old uid to one of the copy's own");

    // LOADING IS NOT COPYING. A saved document restores identities; re-minting here would give every
    // widget a new Widget ID on every open and break every reference pointing at it from anywhere.
    jf::JJson doc = jf::JJson::object();
    jf::JJson arr = jf::JJson::array();
    arr.push(PanelModel::elementToJson(*m.get(src)));
    doc["widgets"] = std::move(arr);
    PanelModel reload;
    reload.load(doc);
    std::set<std::string> after;
    collect(childrenOf(reload, reload.elements().front().id), after);
    check(after == orig, "a reloaded page keeps every uid it was saved with");

    std::printf("[child-uid] %s\n", fails ? "FAILURES" : "all good");
    return fails ? 1 : 0;
}
