// Dumps the dictionary tree verbatim, so a refactor of its builder can be proved to change nothing.
#include "model/Cache.h"
#include "model/MetaModel.h"
#include "ui/DictionaryTree.h"
#include <cstdio>
#include <string>
static void dump(const JTreeViewNode& n, int d) {
    std::printf("%*s%s [%s] icon=%d%s\n", d * 2, "", n.label.c_str(), n.userData.c_str(), n.icon,
                n.placeholder ? " placeholder" : "");
    for (const auto& c : n.children) dump(c, d + 1);
}
int main(int argc, char** argv) {
    MetaModel m;
    if (!m.loadFile(REAL_META)) return 1;
    Cache::instance().setMeta(&m);
    Cache::instance().setConfigImage(m.defaultImage());
    const bool cfgOnly = argc > 1 && std::string(argv[1]) == "config";
    dump(cfgOnly ? buildConfigTree(m) : buildDictionary(m), 0);
}
