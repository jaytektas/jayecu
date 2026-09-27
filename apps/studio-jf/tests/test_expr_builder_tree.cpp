// What the expression BUILDER offers you — not what the language can do, but what the tree says it can.
//
// The builder's tree was every FIELD you could name and nothing about what you could do with them: no
// functions, no tables. So "is there an age()?" was answered by reading the schema help of some field
// that happened to mention one, and a table could only be read by an expression written from memory
// against a numeric id. Both are now in the tree, and both are back-end aware — offering a host-only
// function inside a firmware expression is offering something that compiles to an error.
//
// Every token this tree offers must also COMPILE, which is the property that ties the two halves
// together: the builder cannot advertise a spelling the compiler does not accept.
//
//   cmake --build build --target expr_builder_tree_test && ./build/expr_builder_tree_test
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "model/ExprCompiler.h"
#include "ui/DictionaryTree.h"

#include <j/core/JTreeView.h>

#include <cstdio>
#include <string>
#include <vector>

static int fails = 0;
static void ck(bool ok, const std::string& what, const std::string& note = {}) {
    std::printf("  %-66s %s%s\n", what.c_str(), ok ? "PASS" : "FAIL",
                (ok || note.empty()) ? "" : ("  (" + note + ")").c_str());
    if (!ok) ++fails;
}

static const JTreeViewNode* group(const JTreeViewNode& root, const std::string& label) {
    for (const auto& c : root.children) if (c.label == label) return &c;
    return nullptr;
}

static std::vector<std::string> tokens(const JTreeViewNode& n) {
    std::vector<std::string> out;
    if (!n.userData.empty()) out.push_back(n.userData);
    for (const auto& c : n.children)
        for (auto& t : tokens(c)) out.push_back(std::move(t));
    return out;
}

int main() {
    MetaModel meta;
    if (!meta.loadFile(REAL_META)) { std::printf("FAIL: no meta at %s\n", REAL_META); return 1; }
    Cache& C = Cache::instance();
    C.setMeta(&meta);
    C.setConfigImage(meta.defaultImage());

    std::puts("-- a firmware expression --");
    const JTreeViewNode fw = buildSigilTree(meta, {}, /*firmware=*/true);
    const JTreeViewNode* fns = group(fw, "Functions");
    ck(fns != nullptr, "the builder offers the language's functions");
    const JTreeViewNode* tbls = group(fw, "Tables");
    ck(tbls != nullptr, "…and the tables an expression can read");

    if (fns) {
        bool hasAge = false, hasSqrt = false;
        for (const auto& c : fns->children) {
            if (c.label == "age()")  hasAge = true;
            if (c.label == "sqrt()") hasSqrt = true;
        }
        ck(hasAge, "age() is offered — it reads MCU state, so it is a firmware function");
        ck(!hasSqrt, "sqrt() is NOT offered — the ECU has no instruction for it");
    }

    if (tbls) {
        const std::vector<std::string> toks = tokens(*tbls);
        ck(toks.size() == meta.tableRegistry().size(),
           "every table in the registry is offered", std::to_string(toks.size()) + " of " +
               std::to_string(meta.tableRegistry().size()));
        bool generic = false;
        for (const std::string& t : toks) if (t == "table(generic_tables_table_1)") generic = true;
        ck(generic, "the generic pool is in it, by name");

        // THE JOIN. A token the tree offers that the compiler rejects is a builder that lies, and this
        // is exactly how the table feature shipped once already: the compiler validated every program
        // against a registry size of zero, so every table token compiled to "please report this".
        int bad = 0; std::string firstBad;
        for (const std::string& t : toks) {
            const auto r = ExprCompiler::compile(t, meta, (uint32_t)C.configImage().size());
            if (!r.ok) { ++bad; if (firstBad.empty()) firstBad = t + ": " + r.error; }
        }
        ck(bad == 0, "…and every one of them COMPILES", std::to_string(bad) + " rejected: " + firstBad);
    }

    std::puts("\n-- a host expression (a visibility test, a data source) --");
    const JTreeViewNode host = buildSigilTree(meta, {}, /*firmware=*/false);
    ck(group(host, "Tables") == nullptr, "no tables: the studio has no VM to read one with");
    if (const JTreeViewNode* hf = group(host, "Functions")) {
        bool hasSqrt = false, hasAge = false;
        for (const auto& c : hf->children) {
            if (c.label == "sqrt()") hasSqrt = true;
            if (c.label == "age()")  hasAge = true;
        }
        ck(hasSqrt, "sqrt() IS offered here — the studio can evaluate it");
        ck(!hasAge, "age() is not: there is no MCU to ask");
    } else {
        ck(false, "the host builder offers functions too");
    }

    std::printf("\n%s\n", fails ? "FAILURES" : "all passed");
    return fails ? 1 : 0;
}
